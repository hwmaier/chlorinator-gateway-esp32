"""
PlatformIO pre-build script – injects .env credentials as compiler defines.

  1. Copy .env.example to .env and fill in your values.
  2. .env is gitignored and never committed.
  3. Build normally: pio run -t upload
"""
Import("env")
import os, sys

REQUIRED = [
    "WIFI_SSID", "WIFI_PASSWORD",
    "MQTT_BROKER", "MQTT_USERNAME", "MQTT_PASSWORD",
    "CHLORINATOR_MAC", "CHLORINATOR_NAME", "CHLORINATOR_CODE",
]

def parse_env_file(path):
    values = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            values[key.strip()] = val.strip()
    return values

env_path = os.path.join(os.getcwd(), ".env")

if not os.path.exists(env_path):
    sys.stderr.write(
        "\n[load_env] ERROR: .env not found.\n"
        "           Copy esp32/.env.example to esp32/.env and fill in your credentials.\n\n"
    )
    env.Exit(1)

values = parse_env_file(env_path)

missing = [k for k in REQUIRED if not values.get(k)]
if missing:
    sys.stderr.write(
        "\n[load_env] ERROR: missing required .env entries: {}\n\n".format(", ".join(missing))
    )
    env.Exit(1)

for key, val in values.items():
    if val.lstrip("-").isdigit():
        env.Append(CPPDEFINES=[(key, val)])
    else:
        env.Append(CPPDEFINES=[(key, env.StringifyMacro(val))])

print("[load_env] loaded {} values from .env".format(len(values)))
