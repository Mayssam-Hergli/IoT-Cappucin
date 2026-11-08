# EMBER shared config (server, fake_esp32, attack_test). NEVER show this file in slides.
try:
    from ember_secrets import EMBER_KEY, ADMIN_PASSWORD, VIEWER_PASSWORD   # local file, NOT in git
except ImportError:
    raise SystemExit("Missing ember_secrets.py: copy ember_secrets.example.py to ember_secrets.py and fill it in.")
USE_TLS = True                                    # True -> https :5443, False -> http :5000
SERVER_IP = "127.0.0.1"                           # clients on THIS laptop can use 127.0.0.1; ESP32 uses 172.20.10.3

PORT = 5443 if USE_TLS else 5000
BASE_URL = f"{'https' if USE_TLS else 'http'}://{SERVER_IP}:{PORT}"

# Dashboard users (passwords are hashed at server startup)
USERS = {
    "admin":  {"password": ADMIN_PASSWORD,  "role": "admin"},
    "viewer": {"password": VIEWER_PASSWORD, "role": "viewer"},
}

# Simple threshold rule (comparison box). Update with Mayssam's baseline thresholds.
RULE_GAS_DELTA = 300.0      # gas raw rise over baseline
RULE_TEMP_SLOPE = 0.2       # degC per second (a warm hand on the DHT11 gives ~0.22 -> rule would alarm)
RULE_LIGHT_DELTA = 800.0    # abs change in LDR reading
HEAT_DETECTOR_C = 57.0      # classic fixed-temperature heat detector
