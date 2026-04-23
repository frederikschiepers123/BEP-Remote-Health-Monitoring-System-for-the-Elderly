# main.py
#
# Main loop based on your flowchart:
#  - Check internet (WiFi class)
#  - Update WiFi LED
#  - Reconnect WiFi if needed
#  - Use MQTTManager to send sensor data when it changes
#  - Receive commands via MQTT (callback)
import uasyncio as asyncio
import ujson as json
from time import sleep, localtime, ticks_add, ticks_ms, ticks_diff
from ntptime import settime
          # reuses CONFIG, WIFI, WIFI_LED_PIN, MQTT
import wifi
import environment
import radar
from mqtt_manager import MQTTManager
from light import GL5516

CONFIG_PATH = "config.json"

stop_data = False
# ---------- sensor registration & init ----------

def init_i2c(i2c_cfg):
    i2c_id = i2c_cfg["i2c_id"]
    scl_pin = i2c_cfg["i2c_scl"]
    sda_pin = i2c_cfg["i2c_sda"]

    i2c = I2C(
        i2c_id,
        scl=Pin(scl_pin),
        sda=Pin(sda_pin),
    )
    print("I2C initialised: id={}, SCL={}, SDA={}".format(i2c_id, scl_pin, sda_pin))
    return i2c


def read_connected_sensors(env, rad):
    """
    Detect which sensors are connected and return a description dict.
    """
    env_kind = env.detected()
    
    radar_kind = rad.detected()

    print("Environment sensor:", env_kind)
    print("Radar sensor      :", radar_kind)

    return {
        "environment": env_kind,
        "radar": radar_kind
    }


def init_environment_sensors(i2c, cfg):
    from environment import Environment  # Environment wrapper

    """
    Create a global environment instance usable from main.py as environment.ENV.
    """
    env = Environment(i2c, cfg["sensors"]["environment"])
    ENV = env  # attach to module for reuse
    return env


def init_radar_sensors(cfg):
    from radar import RadarWrapper       # RadarWrapper wrapper

    uart_cfg = cfg["uart"]
    rad = RadarWrapper(
        tx_pin_df=uart_cfg["C1001_tx"],
        rx_pin_df=uart_cfg["C1001_rx"],
        tx_pin_hm=uart_cfg["hmmd_tx"],
        rx_pin_hm=uart_cfg["hmmd_rx"],
        uart_id=uart_cfg["uart"]
    )
    rad.detect(cfg["sensors"]["radar"])
    RADAR = rad  # attach to module for reuse
    return rad

def init_ldr_sensor(cfg):
    from light import GL5516

    try:
        # Change this to the ADC pin you’re actually using
        ldr = GL5516(cfg["LDR_pin"])
        print("GL5516 light sensor initialised on pin", cfg["LDR_pin"])
    except Exception as e:
        print("Could not init GL5516 light sensor:", e)
        ldr = None
        return

# ---------- queue helpers -----------
class QueueFull(Exception):
    pass

class AsyncQueue:
    def __init__(self, maxsize=0):
        self.maxsize = maxsize
        self._q = []
        self._ev = asyncio.Event()

    def put_nowait(self, item):
        if self.maxsize and len(self._q) >= self.maxsize:
            raise QueueFull()
        self._q.append(item)
        self._ev.set()

    async def get(self):
        while not self._q:
            self._ev.clear()
            await self._ev.wait()
        return self._q.pop(0)

pub_q = AsyncQueue(maxsize=20)
    
    
# ---------- sensor helpers ----------
def build_sensor_payload(env_state, radar_state, light_state, mqtt_cfg):
    """
    Payload without timestamp (used for "data changed?" comparison).
    """
    # If nothing has data, don't send anything
    if not (env_state or radar_state or light_state):
        return None

    base = {"device_id": CONFIG["LABEL"]}

    if env_state:
        base["environment"] = env_state
    if radar_state:
        base["radar"] = radar_state
    if light_state:
        base["light"] = light_state   

    return base

# ---------- MQTT receive callback ----------

async def _handle_mqtt_message(topic_str, payload_str):
    """
    Runs in an asyncio task started from the sync MQTT callback.
    Does JSON parsing and routes to the correct handler.
    """
    try:
        print("MQTT command received on", topic_str, ":", payload_str)

        # Parse JSON if it looks like JSON, else pass raw string
        data = None
        if payload_str:
            if isinstance(payload_str, (bytes, bytearray)):
                try:
                    payload_str = payload_str.decode("utf-8")
                except Exception:
                    payload_str = str(payload_str)

            if payload_str and payload_str[0] in "{[":
                try:
                    data = json.loads(payload_str)
                except Exception:
                    print("Payload is not valid JSON, using raw string")
                    data = payload_str
            else:
                data = payload_str

        device_resp_topic = "deployment/{}/response".format(CONFIG["LABEL"])
        global_resp_topic = "deployment/response"

        if topic_str == device_resp_topic:
            handle_device_response(data)
        elif topic_str == global_resp_topic:
            handle_global_response(data)
        else:
            handle_unknown_topic(topic_str, data)

    except Exception as e:
        print("Error in MQTT message handler:", e)


def on_command(topic, msg):
    """
    Lightweight MQTT callback; just decodes bytes and schedules
    an asyncio task to do the real work.
    """
    try:
        # Normalise to str
        if isinstance(topic, bytes):
            topic = topic.decode("utf-8")

        if isinstance(msg, bytes):
            payload_str = msg.decode("utf-8")
        else:
            payload_str = msg

        # Hand off to asyncio so the callback stays fast
        asyncio.create_task(_handle_mqtt_message(topic, payload_str))
    except Exception as e:
        print("Error in command callback:", e)


def handle_unknown_topic(topic, data):
    print("MQTT: message on unknown topic:", topic, "payload:", data)


def handle_device_response(data):
    """
    Handle responses/commands targeted specifically to this device.
    """
    global stop_data

    if not isinstance(data, dict):
        print("Device response is not a dict:", data)
        return

    ping = data.get("ping")
    action = data.get("action")

    if ping == "True" or ping is True:
        print("server ping received")
        MQTT.publish_json(
            "deployment/{}/data".format(CONFIG["LABEL"]),
            {"connected": "True"},
        )

    if action == "deregister":  # Remove label and disconnect all
        MQTT.publish_json(
            "deployment/{}/data".format(CONFIG["LABEL"]),
            {"deregister": "True"},
        )
        MQTT.disconnect()
        WIFI.disconnect()
        CONFIG["LABEL"] = None
        CONFIG["registered"] = None
        CONFIG["mqtt"]["BROKER"] = None
        save_config(CONFIG)

        # Optional soft reset if available
        try:
            from machine import soft_reset
            soft_reset()
        except ImportError:
            pass

    elif action == "deactivate":  # stop sending data
        print("stop sending data received")
        stop_data = True

    elif action == "activate":  # start sending data
        print("start sending data received")
        stop_data = False

    else:
        print("Unknown device cmd:", data)


def handle_global_response(data):
    """
    Handle messages from the global response channel.
    Could be broadcast config, OTA info, etc.
    """
    if not isinstance(data, dict):
        print("Global response is not a dict:", data)
        return

    ping = data.get("ping")
    timestamp = data.get("timestamp")
    action = data.get("action")  # reserved for later use
    shutdown = data.get("shutdown")

    print("Global response:", data)

    if ping == "True" or ping is True:
        MQTT.publish_json(
            "deployment/{}/data".format(CONFIG["LABEL"]),
            {"connected": "True"},
        )

    if action == "deregister_sbc":
        # -- disconnect mqtt
        MQTT.disconnect()
        # -- disconnect wifi
        WIFI.disconnect()
        # -- delete ipconfig
        CONFIG["mqtt"]["BROKER"] = None
        try:
            from machine import soft_reset
            soft_reset()
        except ImportError:
            pass
    
    if shutdown == "True" or shutdown is True:
        # -- disconnect mqtt
        MQTT.disconnect()
        # -- disconnect wifi
        WIFI.disconnect()
        # -- delete ipconfig
        CONFIG["mqtt"]["BROKER"] = None
        try:
            from machine import soft_reset
            soft_reset()
        except ImportError:
            pass
        
    else:
        print("Unknown global cmd:", data)
        

# ------------- aysnc main loop functions -----------

# ------------- check internet connectivity --------- 
async def wifi_task():
    global WIFI, CONFIG

    while True:
        try:
            if not WIFI.check_connect():
                # Turn off WiFi LED
                WIFI.wifi_LED(WIFI_LED_PIN)  # LED off (or error state)

                # Reconnect internet
                print("WiFi disconnected, trying to reconnect.")
                WIFI.reconnect()
            else:
                # WiFi OK -> update LED (on)
                WIFI.wifi_LED(WIFI_LED_PIN)  # LED on
        except Exception as e:
            print("wifi_task error:", e)

        await asyncio.sleep(2)
        

# -------------- check MQTT connection -------------
async def mqtt_task():
    global MQTT, CONFIG
    subscribed = False
    while True:
        try:
            if not MQTT.ensure_connection():
                print("MQTT not connected, retrying...")
            # Subscribe once to command topic
            else:
                if not subscribed:
                    MQTT.subscribe("deployment/response", callback=on_command)
                    MQTT.subscribe("deployment/" + CONFIG["LABEL"] + "/response", callback=on_command)
                    subscribed = True
        except Exception as e:
            print("mqtt_task error:", e)
            subscribed = False
        
        await asyncio.sleep(3)
        
# ------------- sync on board clock ---------------
async def clock_sync_task():
    """
    Periodically sync RTC via NTP (only when WiFi is up).
    """
    global WIFI
    
    while True:
        try:
            if WIFI.check_connect():
                print("Syncing time with NTP...")
                settime() #---- get time from the SBC
                print("Time now:", localtime())
        except Exception as e:
            print("clock_sync_task error:", e)

        # sync once an hour (adjust as needed)
        await asyncio.sleep(3600)
    
# ------------ send sensor data to SBC ------------
pub_q = AsyncQueue(maxsize=10)

async def sensor_data_task():
    global stop_data

    topic = "deployment/{}/{}".format(CONFIG["LABEL"], CONFIG["mqtt"]["topic"])
    last_payload_no_ts = None
    light_state = None

    period_ms = 100  # adjust as needed

    while True:
        try:
            if not stop_data:
                env_state = env.read(CONFIG["environment"])
                radar_state = rad.read()

                try:
                    light_state = ldr.read(ldr, CONFIG["environment"]["light_res"])
                except Exception:
                    pass

                payload_no_ts = build_sensor_payload(
                    env_state, radar_state, light_state, CONFIG["mqtt"]
                )

                if payload_no_ts and payload_no_ts != last_payload_no_ts:
                    send_payload = dict(payload_no_ts)
                    send_payload["timestamp"] = time.localtime()

                    try:
                        pub_q.put_nowait((topic, send_payload))
                    except QueueFull:
                        # drop newest if overwhelmed
                        pass

                    last_payload_no_ts = payload_no_ts

        except Exception as e:
            print("sensor_task error:", e)

        await asyncio.sleep_ms(period_ms)

async def mqtt_publisher_task():
    global MQTT
    while True:
        topic, payload = await pub_q.get()
        try:
            MQTT.publish_json(topic, payload)
        except Exception as e:
            print("mqtt_publisher_task error:", e)
        await asyncio.sleep_ms(0)  # yield ASAP
        
        
# ------------ check incmonming messages ----------
async def mqtt_receiver_task():
    while True:
        try:
            MQTT.check_messages()
        except Exception as e:
            print("mqtt_receiver_task error:", e)
        await asyncio.sleep_ms(100)
        
        
        
# ------------------------ main loop ----------------------------
async def main():
    global CONFIG, WIFI, MQTT
    # I2C & sensors
    i2c = init_i2c(CONFIG["i2c"])

    env = init_environment_sensors(i2c, CONFIG)

    rad = init_radar_sensors(CONFIG)

    ldr = init_ldr_sensor(CONFIG)
 
    sensors_cfg = read_connected_sensors(env, rad)
    CONFIG["sensors"] = sensors_cfg
    save_config(CONFIG)

'''
from display import Display

display = Display(env, WIFI)
'''
print("boot.py init complete, ready for main.py")

    # --- Initial MQTT connect: decide secure mode from CONFIG["mqtt"]["port"] ---
    mqtt_cfg = CONFIG["mqtt"]
    port = mqtt_cfg["port"]
    use_mtls = (port == 8883)  # 8883 => mutual TLS, else TLS+password

    MQTT.connect(
        secure=use_mtls,
        client_id=CONFIG["LABEL"],
        username=mqtt_cfg["USER"],
        password=mqtt_cfg["PASSWORD"],
    )
    data = {"connected": "True"}
    MQTT.publish_json("deployment/"+CONFIG["LABEL"]+"/"+CONFIG["mqtt"]["topic"], data)
    # --- send connected true to device topic label/data
    # ----- Start background tasks -------
    asyncio.create_task(wifi_task())
    asyncio.create_task(mqtt_task())
    asyncio.create_task(mqtt_receiver_task())
    asyncio.create_task(clock_sync_task())
    asyncio.create_task(sensor_data_task())
    asyncio.create_task(mqtt_publisher_task())

    # Keep main alive (could also wait on an Event)
    while True:
        await asyncio.sleep(3600)


# ---------- entrypoint ----------
# get baseline for cpu usage

try:
    asyncio.run(main())
finally:
    # Micropython recommendation after run()
    asyncio.new_event_loop()