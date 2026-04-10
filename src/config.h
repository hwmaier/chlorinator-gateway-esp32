#pragma once

// Credentials are normally injected at build time from the .env file via
// load_env.py.  The #ifndef fallbacks below allow the project to compile
// without the injection script (e.g. in a CI environment or a fresh clone).
// Replace the dummy strings before flashing to a real device.

// ─── WiFi ────────────────────────────────────────────────────────────────────
#ifndef WIFI_SSID
#define WIFI_SSID         "your_wifi_ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD     "your_wifi_password"
#endif

// ─── MQTT ────────────────────────────────────────────────────────────────────
#ifndef MQTT_BROKER
#define MQTT_BROKER       "homeassistant.home"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT         1883
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME     "your_mqtt_username"
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD     "your_mqtt_password"
#endif

// ─── Chlorinator ─────────────────────────────────────────────────────────────
#ifndef CHLORINATOR_MAC
#define CHLORINATOR_MAC   "AA:BB:CC:DD:EE:FF"
#endif
#ifndef CHLORINATOR_NAME
#define CHLORINATOR_NAME  "POOL01"
#endif
#ifndef CHLORINATOR_CODE
#define CHLORINATOR_CODE  "1234"
#endif

// ─── Timing ──────────────────────────────────────────────────────────────────
#define POLL_INTERVAL_MS  10000   // Milliseconds between state reads
#define BLE_SCAN_DURATION     10  // BLE scan timeout in seconds
