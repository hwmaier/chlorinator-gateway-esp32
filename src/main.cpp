/*
 * ESP32 BLE-to-MQTT Gateway for Astral Pool Viron eQuilibrium Chlorinator
 *
 * Behaviour:
 *  - Scans/connects to the chlorinator by BLE MAC address
 *  - Authenticates using the device access code
 *  - Reads encrypted state every POLL_INTERVAL_MS milliseconds
 *  - Publishes JSON state to chlorinator/<name>/state
 *  - Subscribes to chlorinator/<name>/action for commands (integer 0-13)
 *  - Publishes Home Assistant MQTT Discovery config on (re)connect
 *
 * Architecture: two FreeRTOS tasks
 *  - loop()    — WiFi, MQTT, log/OTA server, LED heartbeat, state publishing
 *  - ble_task  — BLE scan/connect/auth/read, posts results via queue
 *
 * Development tools (single TCP port 23):
 *  - pio device monitor  → streams log output as plain text
 *  - pio run -t upload   → tools/ota_upload.py pushes firmware via "OTA <size>\n" protocol
 *
 * Dependencies (platformio.ini):
 *   h2zero/NimBLE-Arduino @ ^2.0
 *   knolleary/PubSubClient @ ^2.8
 *   bblanchon/ArduinoJson  @ ^7.0
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <atomic>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "crypto.h"
#include "chlorinator.h"

// XIAO ESP32C6: user LED on GPIO15, active low
#define PIN_LED 15

// ─── Log / OTA server (port 23, single TCP connection) ───────────────────────
static WiFiServer s_log_server(23);
static WiFiClient s_log_client;

// ─── MQTT Topic Strings (built from CHLORINATOR_NAME at startup) ──────────────
static char s_name_lower[32];
static char s_topic_state[80];
static char s_topic_action[80];
static char s_topic_discovery[128];
static char s_device_id[48];

// ─── Inter-task synchronisation ───────────────────────────────────────────────
static SemaphoreHandle_t  g_log_mutex;
static QueueHandle_t      g_action_queue;        // int (ChlorinatorAction), depth 1: MQTT → BLE task
static QueueHandle_t      g_state_queue;         // ChlorinatorState, depth 1: BLE task → loop
static std::atomic<bool>  g_mqtt_connected{false};
static std::atomic<bool>  g_ota_in_progress{false};
static TaskHandle_t       g_ble_task_handle = nullptr;

// ─── BLE address cache (BLE task only) ───────────────────────────────────────
static NimBLEAddress g_ble_address;
static bool          g_has_ble_address = false;

// ─── MQTT & WiFi ──────────────────────────────────────────────────────────────
static WiFiClient   s_wifi;
static PubSubClient s_mqtt(s_wifi);

// ─────────────────────────────────────────────────────────────────────────────
// Logging — mutex-protected so both tasks can call safely
// ─────────────────────────────────────────────────────────────────────────────

static void tlog(const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    Serial.print(buf);
    if (s_log_client && s_log_client.connected()) {
        s_log_client.print(buf);
    }
    xSemaphoreGive(g_log_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void str_to_lower(const char *src, char *dst, size_t dst_len) {
    size_t i = 0;
    while (src[i] && i < dst_len - 1) {
        dst[i] = (char)tolower((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT: state JSON publishing
// ─────────────────────────────────────────────────────────────────────────────

static void publish_state(const ChlorinatorState &state) {
    if (!s_mqtt.connected()) return;

    JsonDocument doc;
    doc["mode"]                                   = (int)state.mode;
    doc["pump_speed"]                             = (int)state.pump_speed;
    doc["active_timer"]                           = state.active_timer;
    doc["info_message"]                           = info_message_name(state.info_message);
    doc["ph_measurement"]                         = state.ph_measurement;
    doc["chlorine_control_status"]                = (int)state.chlorine_status;
    doc["chemistry_values_current"]               = state.chemistry_current;
    doc["chemistry_values_valid"]                 = state.chemistry_valid;
    doc["time_hours"]                             = state.time_hours;
    doc["time_minutes"]                           = state.time_minutes;
    doc["time_seconds"]                           = state.time_seconds;
    doc["spa_selection"]                          = state.spa_selection;
    doc["pump_is_priming"]                        = state.pump_priming;
    doc["pump_is_operating"]                      = state.pump_operating;
    doc["cell_is_operating"]                      = state.cell_operating;
    doc["sanitising_until_next_timer_tomorrow"]   = state.sanitising_tomorrow;

    size_t len = measureJson(doc);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        tlog("[MQTT] publish_state: malloc failed\n");
        return;
    }
    serializeJson(doc, buf, len + 1);
    s_mqtt.publish(s_topic_state, buf, /*retain=*/false);
    free(buf);
    tlog("[MQTT] state published\n");
}

static void publish_error(const char *msg) {
    if (!s_mqtt.connected()) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    s_mqtt.publish(s_topic_state, buf, /*retain=*/false);
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT: Home Assistant discovery (device-based, HA >= 2023.12)
// ─────────────────────────────────────────────────────────────────────────────

static void publish_autodiscovery() {
    JsonDocument doc;

    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"]  = s_device_id;
    char dev_name[48];
    snprintf(dev_name, sizeof(dev_name), "Chlorinator %s", CHLORINATOR_NAME);
    dev["name"] = dev_name;
    dev["mf"]   = "Open Source";
    dev["mdl"]  = "MQTT Gateway for Astral Pool";

    JsonObject origin = doc["o"].to<JsonObject>();
    origin["name"] = "chlorinator-gateway-esp32";
    origin["url"]  = "https://github.com/hwmaier/chlorinator-gateway-esp32";

    doc["state_topic"] = s_topic_state;
    doc["qos"]         = 1;

    JsonObject cmps = doc["cmps"].to<JsonObject>();

    auto add_sensor = [&](const char *key, const char *name, const char *vt,
                          const char *dev_class = nullptr, bool enabled = true) {
        JsonObject c = cmps[key].to<JsonObject>();
        c["name"] = name;
        c["p"]    = "sensor";
        c["value_template"] = vt;
        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
        c["unique_id"] = uid;
        if (dev_class) c["device_class"] = dev_class;
        if (!enabled)  c["enabled_by_default"] = false;
    };

    auto add_binary = [&](const char *key, const char *name, const char *vt,
                          const char *dev_class = nullptr) {
        JsonObject c = cmps[key].to<JsonObject>();
        c["name"]           = name;
        c["p"]              = "binary_sensor";
        c["value_template"] = vt;
        c["payload_on"]     = true;
        c["payload_off"]    = false;
        char uid[64];
        snprintf(uid, sizeof(uid), "%s_%s", s_device_id, key);
        c["unique_id"] = uid;
        if (dev_class) c["device_class"] = dev_class;
    };

    add_sensor("mode", "Mode",
        "{% set map = {0:'Off', 1:'Manual on', 2:'Auto'} %}"
        "{{ map[value_json.mode|int] }}");

    add_binary("pump_is_operating", "Pump",
        "{{ value_json.pump_is_operating }}", "running");

    add_binary("pump_is_priming", "Priming",
        "{{ value_json.pump_is_priming }}", "running");

    add_binary("cell_is_operating", "Cell",
        "{{ value_json.cell_is_operating }}");

    add_sensor("active_timer", "Active Timer",
        "{{ value_json.active_timer }}");

    add_binary("sanitising_until_next_timer_tomorrow",
        "Sanitising until next timer tomorrow",
        "{{ value_json.sanitising_until_next_timer_tomorrow }}");

    add_sensor("pump_speed", "Pump Speed",
        "{{ value_json.pump_speed }}", nullptr, /*enabled=*/false);

    add_sensor("ph_measurement", "pH Measurement",
        "{{ value_json.ph_measurement }}", "ph", /*enabled=*/false);

    add_sensor("chemistry_values_current", "Chemistry Values Current",
        "{{ value_json.chemistry_values_current }}", nullptr, false);

    add_sensor("chemistry_values_valid", "Chemistry Values Valid",
        "{{ value_json.chemistry_values_valid }}", nullptr, false);

    add_sensor("chlorine_control_status", "Chlorine Control Status",
        "{{ value_json.chlorine_control_status }}", nullptr, false);

    add_sensor("spa_selection", "Spa Selection",
        "{{ value_json.spa_selection }}", nullptr, false);

    {
        JsonObject c = cmps["action"].to<JsonObject>();
        c["name"]          = "Action";
        c["p"]             = "select";
        c["command_topic"] = s_topic_action;
        char uid[64];
        snprintf(uid, sizeof(uid), "%s_action", s_device_id);
        c["unique_id"] = uid;

        JsonArray opts = c["options"].to<JsonArray>();
        opts.add("No action");
        opts.add("Off");
        opts.add("Automatic");
        opts.add("Manual");
        opts.add("Low speed");
        opts.add("Medium speed");
        opts.add("High speed");
        opts.add("Pool mode");
        opts.add("Spa mode");
        opts.add("Dismiss info message");
        opts.add("Disable acid dosing indefinitely");
        opts.add("Disable acid dosing for period");
        opts.add("Reset statistics");
        opts.add("Trigger cell reversal");

        c["command_template"] =
            "{% set map = {"
            "'No action':0,'Off':1,'Automatic':2,'Manual':3,"
            "'Low speed':4,'Medium speed':5,'High speed':6,"
            "'Pool mode':7,'Spa mode':8,'Dismiss info message':9,"
            "'Disable acid dosing indefinitely':10,"
            "'Disable acid dosing for period':11,"
            "'Reset statistics':12,'Trigger cell reversal':13"
            "} %}{{ map[value] }}";
    }

    size_t len = measureJson(doc);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        tlog("[MQTT] autodiscovery: malloc failed\n");
        return;
    }
    serializeJson(doc, buf, len + 1);
    s_mqtt.publish(s_topic_discovery, (const uint8_t *)buf, (unsigned int)len, /*retain=*/true);
    tlog("[MQTT] autodiscovery published (%d bytes)\n", (int)len);
    free(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT callbacks and reconnect
// ─────────────────────────────────────────────────────────────────────────────

static void on_mqtt_message(char *topic, byte *payload, unsigned int length) {
    char buf[16];
    if (length == 0 || length >= sizeof(buf)) return;
    memcpy(buf, payload, length);
    buf[length] = '\0';

    int action = atoi(buf);
    if (action >= ACTION_NO_ACTION && action <= ACTION_TRIGGER_CELL_REVERSAL) {
        xQueueOverwrite(g_action_queue, &action);
        tlog("[MQTT] action received: %d\n", action);
    } else {
        tlog("[MQTT] unknown action payload: '%s'\n", buf);
    }
}

static void mqtt_reconnect() {
    static unsigned long s_last_attempt = 0;
    if (millis() - s_last_attempt < 5000) return;
    s_last_attempt = millis();

    char client_id[48];
    snprintf(client_id, sizeof(client_id), "chlorinator-%s-%04x",
             s_name_lower, (unsigned)(esp_random() & 0xFFFF));

    tlog("[MQTT] connecting to %s:%d ...\n", MQTT_BROKER, MQTT_PORT);
    if (s_mqtt.connect(client_id, MQTT_USERNAME, MQTT_PASSWORD)) {
        tlog("[MQTT] connected\n");
        s_mqtt.subscribe(s_topic_action);
        publish_autodiscovery();
    } else {
        tlog("[MQTT] failed, rc=%d, retry in 5s\n", s_mqtt.state());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE: scan, connect, authenticate, read state, optionally write action
// ─────────────────────────────────────────────────────────────────────────────

static bool ble_scan() {
    tlog("[BLE] scanning for %s ...\n", CHLORINATOR_MAC);
    NimBLEScan *pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    NimBLEScanResults results = pScan->getResults(BLE_SCAN_DURATION, /*is_continue=*/false);
    tlog("[BLE] scan complete, %d device(s) found\n", results.getCount());

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *dev = results.getDevice(i);
        String addr = String(dev->getAddress().toString().c_str());
        String name = String(dev->getName().c_str());
        tlog("[BLE]   %s  \"%s\"\n", addr.c_str(), name.c_str());

        bool match_mac  = !String(CHLORINATOR_MAC).isEmpty() &&
                           addr.equalsIgnoreCase(CHLORINATOR_MAC);
        bool match_name = name.equals(CHLORINATOR_NAME);

        if (match_mac || match_name) {
            g_ble_address     = dev->getAddress();
            g_has_ble_address = true;
            tlog("[BLE] device found: %s (%s)\n", addr.c_str(), name.c_str());
            pScan->clearResults();
            return true;
        }
    }
    pScan->clearResults();
    tlog("[BLE] device not found\n");
    return false;
}

// Connects to the chlorinator, optionally sends an action, reads and decrypts
// state into out_state. Returns true on success.
static bool ble_operate(ChlorinatorAction action, ChlorinatorState *out_state) {
    if (!g_has_ble_address) {
        if (!ble_scan()) return false;
    }

    NimBLEClient *pClient = NimBLEDevice::createClient();
    pClient->setConnectTimeout(15000);

    tlog("[BLE] connecting to %s ...\n", g_ble_address.toString().c_str());
    bool connected = pClient->connect(g_ble_address);
    if (!connected) {
        tlog("[BLE] connection failed, re-scanning ...\n");
        NimBLEDevice::deleteClient(pClient);
        g_has_ble_address = false;
        // Chlorinator may need a moment to restart advertising after a previous
        // session disconnect — re-scan gives it that time implicitly.
        if (!ble_scan()) return false;

        pClient = NimBLEDevice::createClient();
        pClient->setConnectTimeout(15000);
        tlog("[BLE] retrying connect to %s ...\n", g_ble_address.toString().c_str());
        connected = pClient->connect(g_ble_address);
        if (!connected) {
            tlog("[BLE] connection failed after re-scan\n");
            NimBLEDevice::deleteClient(pClient);
            g_has_ble_address = false;
            return false;
        }
    }
    tlog("[BLE] connected\n");

    bool success = false;

    NimBLERemoteService *pSvc = pClient->getService(UUID_ASTRALPOOL_SERVICE);
    if (!pSvc) {
        tlog("[BLE] Astral Pool service not found\n");
        goto disconnect;
    }

    {
        NimBLERemoteCharacteristic *pSK = pSvc->getCharacteristic(UUID_SLAVE_SESSION_KEY);
        if (!pSK || !pSK->canRead()) {
            tlog("[BLE] session key characteristic unavailable\n");
            goto disconnect;
        }

        std::string sk_raw = pSK->readValue();
        if (sk_raw.size() != 16) {
            tlog("[BLE] unexpected session key size: %d\n", (int)sk_raw.size());
            goto disconnect;
        }
        const uint8_t *session_key = (const uint8_t *)sk_raw.data();

        tlog("[BLE] session key: %02x%02x%02x...%02x\n",
             session_key[0], session_key[1], session_key[2], session_key[15]);

        NimBLERemoteCharacteristic *pAuth = pSvc->getCharacteristic(UUID_MASTER_AUTHENTICATION);
        if (!pAuth || !pAuth->canWrite()) {
            tlog("[BLE] auth characteristic unavailable\n");
            goto disconnect;
        }

        uint8_t mac_key[16];
        encrypt_mac_key(session_key,
                        (const uint8_t *)CHLORINATOR_CODE, strlen(CHLORINATOR_CODE),
                        mac_key);

        if (!pAuth->writeValue(mac_key, 16, /*response=*/true)) {
            tlog("[BLE] auth write failed\n");
            goto disconnect;
        }
        tlog("[BLE] authenticated\n");

        if (action != ACTION_NO_ACTION) {
            NimBLERemoteCharacteristic *pAct =
                pSvc->getCharacteristic(UUID_CHLORINATOR_APP_ACTION);
            if (pAct && pAct->canWrite()) {
                ChlorinatorActionPacket pkt;
                build_action_packet(action, 0, &pkt);

                uint8_t enc[CHAR_DATA_LEN];
                encrypt_characteristic((const uint8_t *)&pkt, session_key, enc);

                if (pAct->writeValue(enc, CHAR_DATA_LEN, /*response=*/true)) {
                    tlog("[BLE] action %d sent\n", (int)action);
                } else {
                    tlog("[BLE] action write failed\n");
                }
                delay(500);  // Let the device process the action before reading back
            }
        }

        NimBLERemoteCharacteristic *pState =
            pSvc->getCharacteristic(UUID_CHLORINATOR_STATE);
        if (!pState || !pState->canRead()) {
            tlog("[BLE] state characteristic unavailable\n");
            goto disconnect;
        }

        std::string state_raw = pState->readValue();
        if ((int)state_raw.size() < CHAR_DATA_LEN) {
            tlog("[BLE] state too short: %d bytes\n", (int)state_raw.size());
            goto disconnect;
        }

        uint8_t decrypted[CHAR_DATA_LEN];
        decrypt_characteristic((const uint8_t *)state_raw.data(), session_key, decrypted);
        parse_chlorinator_state(decrypted, out_state);
        success = true;

        tlog("[BLE] state: mode=%d priming=%s pump=%s cell=%s\n",
            out_state->mode,
            out_state->pump_priming   ? "ON"  : "OFF",
            out_state->pump_operating ? "ON"  : "OFF",
            out_state->cell_operating ? "ON"  : "OFF");
    }

disconnect:
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);

    if (!success) {
        g_has_ble_address = false;  // Force re-scan on next attempt
    }
    return success;
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE task — polls the chlorinator independently of the main loop
// ─────────────────────────────────────────────────────────────────────────────

static void ble_task(void *) {
    static ChlorinatorState state{};
    unsigned long mqtt_last_seen = 0;  // millis() timestamp of last MQTT connection

    while (true) {
        // Pause during OTA — wait for main loop to signal completion
        if (g_ota_in_progress.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Block until an action arrives or the poll interval elapses
        int action = ACTION_NO_ACTION;
        xQueueReceive(g_action_queue, &action, pdMS_TO_TICKS(POLL_INTERVAL_MS));

        if (g_mqtt_connected.load()) {
            mqtt_last_seen = millis();
        }

        if (ble_operate((ChlorinatorAction)action, &state)) {
            // Fallback: force Auto on next poll if MQTT has been absent for 30+ minutes
            bool mqtt_absent_30min = mqtt_last_seen != 0 &&
                                     millis() - mqtt_last_seen >= 30UL * 60 * 1000;
            if (state.mode != MODE_AUTO && mqtt_absent_30min) {
                int fallback = ACTION_AUTO;
                xQueueOverwrite(g_action_queue, &fallback);
            }
        } else {
            state.mode = MODE_BLE_ERROR;
        }
        xQueueOverwrite(g_state_queue, &state);
    }

    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// OTA: receive firmware over the log TCP connection and flash it
//
// Protocol (text command line + raw binary):
//   client → "OTA <size>\n"
//   server → "READY\n"
//   client → <size bytes of firmware binary>
//   server → "OK\n"  (then reboots)  or  "ERROR: <reason>\n"
// ─────────────────────────────────────────────────────────────────────────────

static void handle_ota(size_t fw_size) {
    g_ota_in_progress.store(true);
    tlog("[OTA] starting, %u bytes\n", (unsigned)fw_size);

    if (!Update.begin(fw_size)) {
        tlog("[OTA] ERROR: begin failed: %s\n", Update.errorString());
        s_log_client.println("ERROR: Update.begin failed");
        g_ota_in_progress.store(false);
        return;
    }

    s_log_client.println("READY");

    uint8_t buf[512];
    size_t  written      = 0;
    unsigned int last_pct    = 0;
    unsigned long last_data  = millis();

    while (written < fw_size) {
        if (millis() - last_data > 30000) {
            tlog("[OTA] ERROR: timeout\n");
            s_log_client.println("ERROR: timeout");
            Update.abort();
            g_ota_in_progress.store(false);
            return;
        }

        int avail = s_log_client.available();
        if (avail <= 0) {
            delay(1);
            continue;
        }

        int to_read = min((int)sizeof(buf), min(avail, (int)(fw_size - written)));
        int n = s_log_client.read(buf, to_read);
        if (n > 0) {
            Update.write(buf, n);
            written += n;
            last_data = millis();
            unsigned int pct = written * 100 / fw_size;
            if (pct / 10 > last_pct / 10) {
                tlog("[OTA] %u%%\n", pct);
                last_pct = pct;
            }
        }
    }

    if (Update.end(true)) {
        tlog("[OTA] complete, rebooting\n");
        s_log_client.println("OK");
        delay(200);
        ESP.restart();
    } else {
        tlog("[OTA] ERROR: %s\n", Update.errorString());
        s_log_client.printf("ERROR: %s\n", Update.errorString());
        g_ota_in_progress.store(false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Log server: accept clients and parse incoming commands
// ─────────────────────────────────────────────────────────────────────────────

static void service_log_server() {
    if (s_log_server.hasClient()) {
        if (s_log_client && s_log_client.connected()) {
            s_log_client.stop();
        }
        s_log_client = s_log_server.accept();
        tlog("[SYS] log client connected\n");
    }

    if (!s_log_client || !s_log_client.connected()) return;

    static char s_cmd[64];
    static int  s_cmd_len = 0;

    while (s_log_client.available()) {
        char c = (char)s_log_client.read();
        if (c == '\n') {
            if (s_cmd_len > 0 && s_cmd[s_cmd_len - 1] == '\r') s_cmd_len--;
            s_cmd[s_cmd_len] = '\0';

            if (strncmp(s_cmd, "OTA ", 4) == 0) {
                size_t fw_size = (size_t)atol(s_cmd + 4);
                if (fw_size > 0 && fw_size < 4 * 1024 * 1024) {
                    handle_ota(fw_size);
                } else {
                    s_log_client.println("ERROR: invalid size");
                }
            }

            s_cmd_len = 0;
        } else if (s_cmd_len < (int)sizeof(s_cmd) - 1) {
            s_cmd[s_cmd_len++] = c;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino setup
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    // XIAO ESP32C6: GPIO14 selects antenna — HIGH = external u.FL, LOW = built-in ceramic
    pinMode(14, OUTPUT);
    digitalWrite(14, HIGH);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);  // off

    // Create sync primitives before first tlog() call
    g_log_mutex    = xSemaphoreCreateMutex();
    g_action_queue = xQueueCreate(1, sizeof(int));
    g_state_queue  = xQueueCreate(1, sizeof(ChlorinatorState));

    delay(500);
    tlog("\n[SYS] Chlorinator BLE/MQTT Gateway (ESP32) starting\n");

    str_to_lower(CHLORINATOR_NAME, s_name_lower, sizeof(s_name_lower));
    snprintf(s_topic_state,     sizeof(s_topic_state),
             "chlorinator/%s/state",  s_name_lower);
    snprintf(s_topic_action,    sizeof(s_topic_action),
             "chlorinator/%s/action", s_name_lower);
    snprintf(s_topic_discovery, sizeof(s_topic_discovery),
             "homeassistant/device/chlorinator/%s/config", s_name_lower);
    snprintf(s_device_id,       sizeof(s_device_id),
             "chlorinator_%s", s_name_lower);

    tlog("[SYS] state  topic: %s\n", s_topic_state);
    tlog("[SYS] action topic: %s\n", s_topic_action);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                tlog("[WiFi] connected, IP: %s\n",
                     WiFi.localIP().toString().c_str());
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                tlog("[WiFi] disconnected, reason: %d\n",
                     info.wifi_sta_disconnected.reason);
                // reason 201 = SSID not found, 202/15 = wrong password
                break;
            default:
                break;
        }
    });
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    tlog("[WiFi] connecting to %s ...\n", WIFI_SSID);

    s_mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    s_mqtt.setCallback(on_mqtt_message);
    s_mqtt.setBufferSize(4096);
    s_mqtt.setKeepAlive(15);  // Tight keepalive is now safe — loop() is never blocked by BLE

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max TX power for range

    tlog("[SYS] WiFi MAC : %s\n", WiFi.macAddress().c_str());
    tlog("[SYS] BLE  MAC : %s\n", NimBLEDevice::getAddress().toString().c_str());

    xTaskCreate(ble_task, "ble_task", 8192, nullptr, 1, &g_ble_task_handle);
    tlog("[SYS] setup complete\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino loop — WiFi, MQTT, log/OTA server, LED heartbeat, state publishing
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    static bool s_server_started = false;

    // ── Heartbeat LED — toggle once per second ───────────────────────────────
    static unsigned long s_led_toggle = 0;
    static bool s_led_state = false;
    if (millis() - s_led_toggle >= 1000) {
        s_led_toggle = millis();
        s_led_state = !s_led_state;
        digitalWrite(PIN_LED, s_led_state ? LOW : HIGH);
    }

    // ── Periodic heap / stack logging ────────────────────────────────────────
    static unsigned long s_heap_log = 0;
    if (millis() - s_heap_log >= 30000) {
        s_heap_log = millis();
        tlog("[SYS] free heap: %u  loop HWM: %u  BLE HWM: %u\n",
             esp_get_free_heap_size(),
             uxTaskGetStackHighWaterMark(NULL),
             uxTaskGetStackHighWaterMark(g_ble_task_handle));
    }

    // ── Maintain WiFi ────────────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        g_mqtt_connected.store(false);
        if (s_mqtt.connected()) {
            s_mqtt.disconnect();
        }
        if (s_server_started) {
            s_log_client.stop();
            s_log_server.end();
            s_server_started = false;
        }
        static unsigned long s_wifi_check = 0;
        if (millis() - s_wifi_check >= 5000) {
            s_wifi_check = millis();
            tlog("[WiFi] not connected, waiting...\n");
        }
        return;
    }

    // ── Start log server once WiFi is up ─────────────────────────────────────
    if (!s_server_started) {
        s_log_server.begin();
        tlog("[SYS] log/OTA server on port 23\n");
        s_server_started = true;
    }

    // ── Service log clients and handle OTA commands ──────────────────────────
    service_log_server();

    // ── Maintain MQTT ────────────────────────────────────────────────────────
    if (!s_mqtt.connected()) {
        g_mqtt_connected.store(false);
        mqtt_reconnect();
        return;
    }
    s_mqtt.loop();
    g_mqtt_connected.store(true);

    // ── Publish state or error from BLE task results ─────────────────────────
    static ChlorinatorState new_state;
    if (xQueueReceive(g_state_queue, &new_state, 0) == pdTRUE) {
        if (new_state.mode == MODE_BLE_ERROR) {
            publish_error("BLE operation failed");
        } else {
            publish_state(new_state);
        }
    }
}
