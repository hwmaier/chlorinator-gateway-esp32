# Chlorinator Gateway — Project Context

BLE-to-MQTT gateway for an Astral Pool Viron eQuilibrium chlorinator.

- **Python original**: `python/ble2mqtt.py` — runs on a Pi Zero
- **ESP32 port**: project root — PlatformIO project targeting the ESP32-PICO-DevKitM

## Project Structure

```
chlorinator-gateway2/
├── .claude/CLAUDE.md     ← this file
├── .env                  ← credentials (gitignored, copy from .env.example)
├── .env.example          ← template
├── .gitignore
├── load_env.py           ← pre-build script: reads .env → compiler -D flags
├── platformio.ini        ← board: esp32-pico-devkitm-2, NimBLE-Arduino ^1.4.1
├── src/
│   ├── config.h          ← timing constants + #ifndef fallback dummies for credentials
│   ├── crypto.h/.cpp     ← AES-128-ECB encrypt/decrypt (mbedTLS, no extra library)
│   ├── chlorinator.h/.cpp← enums, structs, state parser, action packet builder
│   └── main.cpp          ← WiFi + MQTT + BLE state machine, HA autodiscovery JSON
└── python/
    ├── .env
    ├── .env.sample
    ├── ble2mqtt.py
    ├── ble2mqtt.service
    ├── chlorinator-gateway
    ├── pychlorinator/
    └── scan.py
```

## BLE Protocol (Viron eQuilibrium)

Service UUID: `45000001-98b7-4e29-a03f-160174643001`

| Characteristic      | UUID suffix | Operation                          |
|---------------------|-------------|------------------------------------|
| Slave Session Key   | `...0002`   | Read — 16-byte random key          |
| Master Auth         | `...0003`   | Write — derived auth key           |
| Chlorinator State   | `...0200`   | Read — 20-byte encrypted state     |
| Chlorinator Action  | `...0203`   | Write — 20-byte encrypted command  |

**Connection sequence**: read session key → derive + write auth key → (optional) write action → read state → disconnect.

## Crypto

Shared secret (all Viron devices): `2b7e151628aed2a6abf7158809cf4f3c`

- **Auth key**: `AES-ECB-encrypt( XOR(session_key[16], access_code[4], zero-pad) )`
- **Encrypt** (writes): XOR data with session key → encrypt bytes 0–15 → encrypt bytes 4–19
- **Decrypt** (reads): decrypt bytes 4–19 → decrypt bytes 0–15 → XOR with session key
- All characteristics are exactly **20 bytes** on the wire

## State Packet (first 11 of 20 decrypted bytes)

`[mode, pump_speed, active_timer, info_message, reserved, flags, ph_raw, chlorine_status, hh, mm, ss]`

pH = `ph_raw / 10.0`

## Action Packet (20 bytes, little-endian)

`uint8 action | int32 period_minutes | 15 bytes padding`

## MQTT Topics

| Topic                                             | Direction        | Payload              |
|---------------------------------------------------|------------------|----------------------|
| `chlorinator/<name>/state`                        | publish          | JSON state object    |
| `chlorinator/<name>/action`                       | subscribe        | integer 0–13         |
| `homeassistant/device/chlorinator/<name>/config`  | publish (retain) | HA discovery JSON    |

`<name>` is `CHLORINATOR_NAME` lowercased (e.g. `POOL01` → `pool01`).
