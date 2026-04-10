/*
 * ESP32 BLE-to-MQTT Gateway for Astral Pool Viron eQuilibrium Chlorinator
 *
 * Direct C++ port of ble2mqtt.py for improved reliability on ESP32.
 *
 * Behaviour mirrors the Python version:
 *  - Scans/connects to the chlorinator by BLE MAC address
 *  - Authenticates using the device access code
 *  - Reads encrypted state every POLL_INTERVAL_MS milliseconds
 *  - Publishes JSON state to chlorinator/<name>/state
 *  - Subscribes to chlorinator/<name>/action for commands (integer 0-13)
 *  - Publishes Home Assistant MQTT Discovery config on (re)connect
 *
 * Dependencies (platformio.ini):
 *   h2zero/NimBLE-Arduino @ ^1.4.1
 *   knolleary/PubSubClient @ ^2.8
 *   bblanchon/ArduinoJson  @ ^7.0
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "crypto.h"
#include "chlorinator.h"

// ─── MQTT Topic Strings (built from CHLORINATOR_NAME at startup) ──────────────
static char s_name_lower[32];
static char s_topic_state[80];
static char s_topic_action[80];
static char s_topic_discovery[128];
static char s_device_id[48];

// ─── Global State ─────────────────────────────────────────────────────────────
static ChlorinatorState g_state;
static bool             g_has_state      = false;
static volatile int     g_pending_action = ACTION_NO_ACTION;
static String           g_ble_address;   // Cached after first successful scan

// ─── MQTT & WiFi ──────────────────────────────────────────────────────────────
static WiFiClient   s_wifi;
static PubSubClient s_mqtt(s_wifi);

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

static void publish_state() {
    if (!s_mqtt.connected() || !g_has_state) return;

    JsonDocument doc;
    doc["mode"]                                   = (int)g_state.mode;
    doc["pump_speed"]                             = (int)g_state.pump_speed;
    doc["active_timer"]                           = g_state.active_timer;
    doc["info_message"]                           = info_message_name(g_state.info_message);
    doc["ph_measurement"]                         = g_state.ph_measurement;
    doc["chlorine_control_status"]                = (int)g_state.chlorine_status;
    doc["chemistry_values_current"]               = g_state.chemistry_current;
    doc["chemistry_values_valid"]                 = g_state.chemistry_valid;
    doc["time_hours"]                             = g_state.time_hours;
    doc["time_minutes"]                           = g_state.time_minutes;
    doc["time_seconds"]                           = g_state.time_seconds;
    doc["spa_selection"]                          = g_state.spa_selection;
    doc["pump_is_priming"]                        = g_state.pump_priming;
    doc["pump_is_operating"]                      = g_state.pump_operating;
    doc["cell_is_operating"]                      = g_state.cell_operating;
    doc["sanitising_until_next_timer_tomorrow"]   = g_state.sanitising_tomorrow;

    char buf[512];
    serializeJson(doc, buf, sizeof(buf));
    s_mqtt.publish(s_topic_state, buf, /*retain=*/false);
    Serial.printf("[MQTT] state published\n");
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
    // Build the full discovery document using heap-allocated buffers to avoid
    // blowing the stack. The serialised payload is typically 1.5–2 KB.
    JsonDocument doc;

    // Device metadata
    JsonObject dev  = doc["dev"].to<JsonObject>();
    dev["ids"]  = s_device_id;
    char dev_name[48];
    snprintf(dev_name, sizeof(dev_name), "Chlorinator %s", CHLORINATOR_NAME);
    dev["name"] = dev_name;
    dev["mf"]   = "Open Source";
    dev["mdl"]  = "MQTT Gateway for Astral Pool";

    // Origin / bridge info
    JsonObject origin = doc["o"].to<JsonObject>();
    origin["name"] = "chlorinator-gateway-esp32";
    origin["url"]  = "https://github.com/hwmaier/chlorinator-gateway";

    doc["state_topic"] = s_topic_state;
    doc["qos"]         = 1;

    JsonObject cmps = doc["cmps"].to<JsonObject>();

    // Helper: add a sensor component
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

    // Helper: add a binary_sensor component
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

    // Action select entity
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

        // Map display option → integer action sent to command_topic
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

    // Serialise and publish with retain=true
    char *buf = (char *)malloc(4096);
    if (!buf) {
        Serial.println("[MQTT] autodiscovery: malloc failed");
        return;
    }
    size_t len = serializeJson(doc, buf, 4096);
    s_mqtt.publish(s_topic_discovery, (const uint8_t *)buf, (unsigned int)len, /*retain=*/true);
    Serial.printf("[MQTT] autodiscovery published (%d bytes)\n", (int)len);
    free(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT callbacks and reconnect
// ─────────────────────────────────────────────────────────────────────────────

static void on_mqtt_message(char *topic, byte *payload, unsigned int length) {
    // Payload should be a decimal integer string, e.g. "2"
    char buf[16];
    if (length == 0 || length >= sizeof(buf)) return;
    memcpy(buf, payload, length);
    buf[length] = '\0';

    int action = atoi(buf);
    if (action >= ACTION_NO_ACTION && action <= ACTION_TRIGGER_CELL_REVERSAL) {
        g_pending_action = action;
        Serial.printf("[MQTT] action received: %d\n", action);
    } else {
        Serial.printf("[MQTT] unknown action payload: '%s'\n", buf);
    }
}

static void mqtt_reconnect() {
    static unsigned long s_last_attempt = 0;
    if (millis() - s_last_attempt < 5000) return;
    s_last_attempt = millis();

    char client_id[48];
    snprintf(client_id, sizeof(client_id), "chlorinator-%s-%04x",
             s_name_lower, (unsigned)(esp_random() & 0xFFFF));

    Serial.printf("[MQTT] connecting to %s:%d ...\n", MQTT_BROKER, MQTT_PORT);
    if (s_mqtt.connect(client_id, MQTT_USERNAME, MQTT_PASSWORD)) {
        Serial.println("[MQTT] connected");
        s_mqtt.subscribe(s_topic_action);
        publish_autodiscovery();
    } else {
        Serial.printf("[MQTT] failed, rc=%d, retry in 5s\n", s_mqtt.state());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE: scan, connect, authenticate, read state, optionally write action
// ─────────────────────────────────────────────────────────────────────────────

// Returns true on success and populates g_state.
// On failure clears g_ble_address so the next call will re-scan.
static bool ble_operate(ChlorinatorAction action) {

    // ── 1. Scan if we don't have a cached address ──────────────────────────
    if (g_ble_address.isEmpty()) {
        Serial.printf("[BLE] scanning for %s ...\n", CHLORINATOR_MAC);
        NimBLEScan *pScan = NimBLEDevice::getScan();
        pScan->setActiveScan(true);
        pScan->setInterval(100);
        pScan->setWindow(99);
        NimBLEScanResults results = pScan->start(BLE_SCAN_DURATION, /*is_async=*/false);

        for (int i = 0; i < results.getCount(); i++) {
            NimBLEAdvertisedDevice dev = results.getDevice(i);
            String addr = String(dev.getAddress().toString().c_str());
            String name = String(dev.getName().c_str());

            bool match_mac  = !String(CHLORINATOR_MAC).isEmpty() &&
                               addr.equalsIgnoreCase(CHLORINATOR_MAC);
            bool match_name = name.equals(CHLORINATOR_NAME);

            if (match_mac || match_name) {
                g_ble_address = addr;
                Serial.printf("[BLE] device found: %s (%s)\n",
                              addr.c_str(), name.c_str());
                break;
            }
        }
        pScan->clearResults();

        if (g_ble_address.isEmpty()) {
            Serial.println("[BLE] device not found");
            return false;
        }
    }

    // ── 2. Connect ─────────────────────────────────────────────────────────
    NimBLEClient *pClient = NimBLEDevice::createClient();
    pClient->setConnectTimeout(15);  // seconds

    Serial.printf("[BLE] connecting to %s ...\n", g_ble_address.c_str());
    bool connected = pClient->connect(NimBLEAddress(g_ble_address.c_str()));
    if (!connected) {
        Serial.println("[BLE] connection failed");
        NimBLEDevice::deleteClient(pClient);
        g_ble_address = "";   // Force re-scan next time
        return false;
    }
    Serial.println("[BLE] connected");

    bool success = false;

    // ── 3. Get service ─────────────────────────────────────────────────────
    NimBLERemoteService *pSvc = pClient->getService(UUID_ASTRALPOOL_SERVICE);
    if (!pSvc) {
        Serial.println("[BLE] Astral Pool service not found");
        goto disconnect;
    }

    // ── 4. Read session key ────────────────────────────────────────────────
    {
        NimBLERemoteCharacteristic *pSK = pSvc->getCharacteristic(UUID_SLAVE_SESSION_KEY);
        if (!pSK || !pSK->canRead()) {
            Serial.println("[BLE] session key characteristic unavailable");
            goto disconnect;
        }

        std::string sk_raw = pSK->readValue();
        if (sk_raw.size() != 16) {
            Serial.printf("[BLE] unexpected session key size: %d\n", (int)sk_raw.size());
            goto disconnect;
        }
        const uint8_t *session_key = (const uint8_t *)sk_raw.data();

        Serial.print("[BLE] session key: ");
        for (int i = 0; i < 16; i++) Serial.printf("%02x", session_key[i]);
        Serial.println();

        // ── 5. Authenticate ────────────────────────────────────────────────
        NimBLERemoteCharacteristic *pAuth = pSvc->getCharacteristic(UUID_MASTER_AUTHENTICATION);
        if (!pAuth || !pAuth->canWrite()) {
            Serial.println("[BLE] auth characteristic unavailable");
            goto disconnect;
        }

        uint8_t mac_key[16];
        encrypt_mac_key(session_key,
                        (const uint8_t *)CHLORINATOR_CODE, strlen(CHLORINATOR_CODE),
                        mac_key);

        if (!pAuth->writeValue(mac_key, 16, /*response=*/true)) {
            Serial.println("[BLE] auth write failed");
            goto disconnect;
        }
        Serial.println("[BLE] authenticated");

        // ── 6. Send action (if any) ────────────────────────────────────────
        if (action != ACTION_NO_ACTION) {
            NimBLERemoteCharacteristic *pAct =
                pSvc->getCharacteristic(UUID_CHLORINATOR_APP_ACTION);
            if (pAct && pAct->canWrite()) {
                ChlorinatorActionPacket pkt;
                build_action_packet(action, 0, &pkt);

                uint8_t enc[CHAR_DATA_LEN];
                encrypt_characteristic((const uint8_t *)&pkt, session_key, enc);

                if (pAct->writeValue(enc, CHAR_DATA_LEN, /*response=*/true)) {
                    Serial.printf("[BLE] action %d sent\n", (int)action);
                } else {
                    Serial.println("[BLE] action write failed");
                }
                delay(500);  // Let the device process the action before reading back
            }
        }

        // ── 7. Read state characteristic ──────────────────────────────────
        NimBLERemoteCharacteristic *pState =
            pSvc->getCharacteristic(UUID_CHLORINATOR_STATE);
        if (!pState || !pState->canRead()) {
            Serial.println("[BLE] state characteristic unavailable");
            goto disconnect;
        }

        std::string state_raw = pState->readValue();
        if ((int)state_raw.size() < CHAR_DATA_LEN) {
            Serial.printf("[BLE] state too short: %d bytes\n", (int)state_raw.size());
            goto disconnect;
        }

        // ── 8. Decrypt and parse ───────────────────────────────────────────
        uint8_t decrypted[CHAR_DATA_LEN];
        decrypt_characteristic((const uint8_t *)state_raw.data(), session_key, decrypted);
        parse_chlorinator_state(decrypted, &g_state);
        g_has_state = true;
        success = true;

        Serial.printf("[BLE] state: mode=%d pump=%s cell=%s pH=%.1f\n",
            g_state.mode,
            g_state.pump_operating ? "ON"  : "OFF",
            g_state.cell_operating ? "ON"  : "OFF",
            g_state.ph_measurement);
    }

disconnect:
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);

    if (!success) {
        g_ble_address = "";  // Force re-scan on next attempt
    }
    return success;
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino setup
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[SYS] Chlorinator BLE/MQTT Gateway (ESP32) starting");

    // Build lowercase topic strings
    str_to_lower(CHLORINATOR_NAME, s_name_lower, sizeof(s_name_lower));
    snprintf(s_topic_state,     sizeof(s_topic_state),
             "chlorinator/%s/state",  s_name_lower);
    snprintf(s_topic_action,    sizeof(s_topic_action),
             "chlorinator/%s/action", s_name_lower);
    snprintf(s_topic_discovery, sizeof(s_topic_discovery),
             "homeassistant/device/chlorinator/%s/config", s_name_lower);
    snprintf(s_device_id,       sizeof(s_device_id),
             "chlorinator_%s", s_name_lower);

    Serial.printf("[SYS] state  topic: %s\n", s_topic_state);
    Serial.printf("[SYS] action topic: %s\n", s_topic_action);

    // WiFi – event handler gives us IP on connect and reason code on failure
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                Serial.printf("[WiFi] connected, IP: %s\n",
                              WiFi.localIP().toString().c_str());
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                Serial.printf("[WiFi] disconnected, reason: %d\n",
                              info.wifi_sta_disconnected.reason);
                // reason 201 = SSID not found, 202/15 = wrong password
                break;
            default:
                break;
        }
    });
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] connecting to %s ...\n", WIFI_SSID);

    // MQTT
    s_mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    s_mqtt.setCallback(on_mqtt_message);
    s_mqtt.setBufferSize(4096);   // Needed for the autodiscovery payload
    s_mqtt.setKeepAlive(60);      // Generous keepalive – BLE can block the loop

    // BLE (NimBLE stack – lighter than BlueDroid, coexists better with WiFi)
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max TX power for range

    Serial.printf("[SYS] WiFi MAC : %s\n", WiFi.macAddress().c_str());
    Serial.printf("[SYS] BLE  MAC : %s\n", NimBLEDevice::getAddress().toString().c_str());
    Serial.println("[SYS] setup complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino loop
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    // ── Maintain WiFi ────────────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long s_wifi_check = 0;
        if (millis() - s_wifi_check >= 5000) {
            s_wifi_check = millis();
            Serial.println("[WiFi] not connected, waiting...");
        }
        delay(100);
        return;
    }

    // ── Maintain MQTT ────────────────────────────────────────────────────────
    if (!s_mqtt.connected()) {
        mqtt_reconnect();
        delay(100);
        return;
    }
    s_mqtt.loop();

    // ── Periodic BLE polling ─────────────────────────────────────────────────
    static unsigned long s_next_poll = 0;
    bool pending = (g_pending_action != ACTION_NO_ACTION);
    bool due     = (millis() >= s_next_poll);

    if (!due && !pending) {
        delay(10);
        return;
    }

    // Snapshot and clear the pending action atomically
    ChlorinatorAction action = (ChlorinatorAction)g_pending_action;
    g_pending_action = ACTION_NO_ACTION;
    s_next_poll = millis() + POLL_INTERVAL_MS;

    // Python fallback: if disconnected from MQTT keep device in Auto mode
    // (here MQTT is always connected at this point, but mirror the intent)
    if (action == ACTION_NO_ACTION && g_has_state &&
        g_state.mode != MODE_AUTO && !s_mqtt.connected()) {
        action = ACTION_AUTO;
    }

    bool ok = ble_operate(action);
    if (ok) {
        publish_state();
    } else {
        publish_error("BLE operation failed");
    }
}
