# boot.py
#
# Startup sequence based on your boot flowchart:
#  - Load configuration
#  - Run registration (stub)
#  - Detect & register sensors
#  - Initialise connected sensors
#  - Connect to WiFi using WiFi class
#  - Create MQTTManager instance (no network connect yet)
#  - Turn on WiFi LED if connected
#
# main.py / main_mqtt.py then runs the main loop and handles MQTT traffic.
import ujson as json
from time import sleep, localtime, sleep_ms
from machine import I2C, Pin
from gc import collect


CONFIG_PATH = "config.json"

# Globals main.py can reuse
CONFIG = None
WIFI = None          # instance of wifi.WiFi
WIFI_LED_PIN = None
MQTT = None          # instance of MQTTManager


print("Starting in 3s... press Ctrl-C to stop")
for _ in range(30):
    sleep_ms(100)


# ---------- config helpers ----------

def load_config(path=CONFIG_PATH):
    try:
        with open(path, "r") as f:
            return json.load(f)
    except OSError:
        print("No config file found")

def save_config(cfg, path=CONFIG_PATH):
    with open(path, "w") as f:
        json.dump(cfg, f)


# ---------- registration (stub) ----------

def run_registration(cfg):
    global WIFI
    from Client import SMClient

    """
    Stub for device registration (e.g. registering with a backend).
    For now, just flag as registered once.
    """
    print("Running registration stub...")
    SMClient(device_path=f"./device", CONFIG = cfg, wifi = WIFI)



def ensure_registered(cfg):
    if not cfg["registered"]:
        run_registration(cfg)
    else:
        print("Device already registered.")

#---------- get IP ---------
    
def get_ip():
    """
    Get the IP of the server from a UDP broadcast
    Check only if registered if 
    """
    correct_ip = None
    addr = None
    from utils import listen_for_broker_address
    while addr == None:
        try:
            addr, data = listen_for_broker_address()
            print(addr, data)
        except:
            addr = None
            data = None
        
    if CONFIG["registered"]:
        for i in data:
            if i == CONFIG["LABEL"]:
                print("correct SBC detected", addr)
                correct_ip = True
                return
            else:
                correct_ip = False
                
    if not correct_ip and correct_ip is not None:
        from machine import soft_reset
        soft_reset()

    CONFIG["mqtt"]["BROKER"] = addr
    save_config(CONFIG)    
# ---------- wifi ----------

def init_wifi(cfg):
    """
    Initialize the wifi from the config
    """
    import wifi
    from gc import collect
    gc.collect()
    global WIFI
    wifi_cfg = cfg["wifi"]
    print(wifi_cfg)
    
    try:
        WIFI = wifi.WiFi(wifi_pin = wifi_cfg["LED_pin"])  
        ssid = wifi_cfg["ssid"]
        password = wifi_cfg["password"]
        if ssid:
            WIFI.connect(ssid, password)
        else:
            print("WiFi: no SSID in config.json")
    except Exception as e:
        WIFI = None
        print("WiFi init/connect failed:", e)
        
# ---------- TIME ----------
def set_time():
    """
    get the time from the NTP server
    set the time on the microprocessor equall to that of the NTP server
    """
    from ntptime import settime

    for _ in range(5):
        try:
            settime()
            break
        except OSError:
            sleep(1)
    print("Time now:", localtime())


# ---------- MQTT ----------

def init_mqtt(cfg):
    from mqtt_manager import MQTTManager

    """
    Create MQTTManager instance from config and store it in global MQTT.
    Connection itself is handled from main_mqtt.py.
    """
    global MQTT
    mqtt_cfg = cfg["mqtt"]
    broker = mqtt_cfg["BROKER"]
    debug = mqtt_cfg["debug"]
    label = cfg["LABEL"]
    print(label)
    MQTT = MQTTManager(
        broker=broker,
        debug=debug,
        client_cert_path = f"/certs/{label}.crt",
    )
    print("MQTTManager created for broker:", broker)
    return MQTT


# ---------- overall boot sequence ----------

# Load configuration
CONFIG = load_config()
print("Config loaded")

# WiFi
init_wifi(CONFIG)
# get the address from the broker
get_ip()

# Time (To set the right time for the certificate)
set_time() #---- get time from the internet

# MQTT (no network connect here, only object creation/config check)
init_mqtt(CONFIG)

# Run registration (if needed)
ensure_registered(CONFIG)

print("boot.py init complete, ready for main.py")
