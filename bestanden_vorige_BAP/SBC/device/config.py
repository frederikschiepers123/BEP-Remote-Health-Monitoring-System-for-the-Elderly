# SBC/device/config.py
from __future__ import annotations
from pathlib import Path

_CURRENT_FILE = Path(__file__).resolve()
PROJECT_ROOT = _CURRENT_FILE.parent.parent
DATA_DIR = PROJECT_ROOT / "data"
BASE_DIR = PROJECT_ROOT / "base"
# Ensure data and base directories exist
DATA_DIR.mkdir(parents=True, exist_ok=True)
BASE_DIR.mkdir(parents=True, exist_ok=True)

# ------ Credential file paths -----
CA_CERT_PATH =  BASE_DIR / "ca.crt"
SERVER_CERT_PATH = BASE_DIR / "server.crt"
SERVER_KEY_PATH = BASE_DIR / "server.key"
PRIVATE_KEY_PATH = BASE_DIR / "device.key"
ACL_FILE_PATH = BASE_DIR / "aclfile"
STATUS_PATH = DATA_DIR / "status.json"
DATA_PATH = DATA_DIR / "device_data.json"
DELAY_PATH = DATA_DIR / "delay.txt"
# -----------------------------------


# ------ MQTT/TLS Settings -----
# MQTT/TLS configuration for bootstrap broker
BROKER = "server"                 # Broker hostname or IP
USERNAME = "new_dev"              # Bootstrap username
PASSWORD = "new123"               # Bootstrap password

# Server host name configuration
HOST_MAP  = {
        "server": "None"  # Placeholder, will be set at runtime
        }


# Listening port for UDP broker advertisements
ADVERTISE_PORT = 5005
# -----------------------------------
