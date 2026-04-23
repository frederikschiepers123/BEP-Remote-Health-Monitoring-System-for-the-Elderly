import ujson as json
from machine import Timer, unique_id, soft_reset
import time
import uuid
from wifi import *
from utils import *
from mqtt_manager import MQTTManager


class SMClient:
    """Device client that performs bootstrap enrollment then connects on its assigned topic."""
    def __init__(self, device_path: str = "./", CONFIG = None, wifi=None):
        # Until enrolled, devices use the placeholder common_name "new_dev"
        #listen_for_broker_address()
        self.device_path = device_path
        self.cert_path = None
        self.CONFIG = CONFIG
        cfg = CONFIG["mqtt"]
        self.status_path = "config.json" # ----- merge with config.json
        self.ca_path = "/certs/ca.crt"
        self.tls_pub = "/certs/public_key.pem"
        self.tls_priv = "/certs/private_key.pem"
        self.client = MQTTManager(broker=cfg["BROKER"], debug=True, msg_callback=self.rec_message)
        self.common_name = "new_dev"
        self.username = cfg["USER"]
        self.password = cfg["PASSWORD"]
        self.LED_state = False
        self.uuid_val = str(uuid.uuid4())
        self.wifi = wifi
        self.wifi.led_off()
        self.timer = Timer(0)
        self.port = 1884
        self.label = None
        self.connected = False
        self.finished = False
        #self.priv_key = generate_ed25519_private_key()
        #self.pub_key = derive_ed25519_public_key(self.priv_key)
        print(f"Generated ed25519 keypair for {self.common_name}")
        # Initial bootstrap connection subscribes to shared enrollment response topic
        self.connect()
        #time.sleep(0.2) # brief pause to ensure connection setup
        self.enroll_sent = False  # guard to prevent repeated enroll publishes
        self.loop()

    def loop(self):
        """Main loop to process incoming MQTT messages."""
        ping_count = 0
        while self.finished is False:
            if ping_count > 100:
                ping_count = 0
                ping = True
            else:
                ping_count += 1
                ping = False
            self.client.check_messages(ping=ping)
            time.sleep(0.1)
            

    def connect(self, secure=False):
        """MQTT callback invoked upon successful connection."""
        # Use a unique client_id during bootstrap to avoid clientID collisions across devices
        if self.port == 1884 and self.common_name == "new_dev":
            client_id = f"{self.common_name}-{uuid.uuid4().hex[:8]}"
        elif self.label is not None:
            client_id = self.label
        else:
            client_id = self.common_name
        print(client_id)
        if secure:
            self.port = 8883
            cert = self.cert_path
            key = self.tls_priv
            print(f"Connecting to broker at {self.client.broker}:{self.port} as {client_id} with username {self.username} using TLS with client cert/key")
        else:
            print(f"Connecting to broker at {self.client.broker}:{self.port} as {client_id} with username {self.username} using TLS without client cert/key")   
            self.port = 1884
            cert = None
            key = None
        self.client.connect(client_id=client_id, username=self.username, password=self.password, secure=secure, cert=cert, key=key)
        while self.client.connected is False:
            pass  # wait until connected
        print(f"{self.common_name} connected to broker on port {self.port}")
        # Subscribe to appropriate response topic based on current phase
        if self.port == 1884:
            if self.common_name == "new_dev":
                self.client.subscribe("enroll/response")
            else:
                self.client.subscribe(f"enroll/{self.common_name}/response")
        elif self.username == "reg_dev":
           self.client.subscribe(f"verification/{self.label}/response")
           self.client.subscribe(f"verification/response")
        return None
       
    def rec_message(self, topic, msg):
        """MQTT callback for all subscribed topics during both phases.

        During enrollment, forward messages to enroll_handler. After enrollment,
        handle per-device messages (currently printed).
        """
        print("Message received on topic:", topic, "with payload:", msg)
        rec_text = msg.decode()
        topic = topic.decode().split('/')
        if topic == ['enroll', 'response']:
            self.enroll_handler(rec_text)
        elif topic[0] == "enroll" and topic[2] == "response":
            self.verify_handler(rec_text)
        elif topic[0] == "verification" and topic[2] == "response": 
            self.finish_handler(rec_text)
        else:
            print(f"Received message on unknown topic {msg.topic}: {rec_text}")
        
        #self.loop()

    def enroll_handler(self, msg):
        """Process bootstrap response and signal completion when it matches our public key."""
        data = json.loads(msg)
        if data.get("request") is True and self.common_name == "new_dev" and not self.enroll_sent:
            self.enroll()
            self.enroll_sent = True
        if (data.get("dev_key") == self.uuid_val) and self.common_name == "new_dev":
            self.common_name = data.get("common_name")
            self.client.disconnect()
            print(f"{self.common_name} disconnected after enrollment")
            # signal enrollment completion
            self.connect()
            self.client.publish(f"enroll/{self.common_name}/request", json.dumps({"checkup": True, "common_name": self.common_name}))
            #self.enroll_sent = False  # reset for potential future cycles (unlikely)

    def verify_handler(self, msg):
        """Handle per-device messages after enrollment (currently just print them)."""
        data = json.loads(msg)
        key_data = None
        try:
            nonce = data.get("nonce")
        except:
            nonce = None
        try:
            label = data.get("label")
        except:
            label = None
            
        if nonce:
            nonce_bytes = bytes.fromhex(nonce)
            # Sign and publish the signature (hex-encoded for JSON)
            # sig = sign_message_ed25519(nonce_bytes, self.priv_key, self.pub_key)
            sig = nonce
            print(sig)
            self.LED_state = True # Turn LED on
            self.timer.init(freq=10, mode=Timer.PERIODIC, callback=self.wifi.blink)
            try:
                with open(self.tls_pub, "rb") as f:
                    key_data = f.read()
            except Exception as e:
                print("could not read public key:", e)
            print(key_data)
            print("gotten uuid value")
            mac = str(unique_id())
            payload = {
                "signature": sig,
                "LED": self.LED_state, 
                "uuid": self.uuid_val,
                "mac": mac,
                "add_key": key_data

            }
            print(payload)
            self.client.publish(f"enroll/{self.common_name}/request", json.dumps(payload))
        if label:
            self.timer.deinit()
            self.label = label
            self.certificate = data.get("certificate")
            self.cert_path = f"/certs/{self.label}.crt"
            with open(self.cert_path, "w") as f:
                f.write(self.certificate)
            self.LED_state = False # Turn LED off
            self.wifi.led_off()
            self.client.disconnect()
            self.username = "reg_dev"
            self.connect(secure=True)
            self.client.publish(f"verification/{self.label}/request", json.dumps({"checkup": True, "common_name": self.common_name}))
    
    def finish_handler(self, msg):
        self.wifi.led_off()
        data = json.loads(msg)
        if data.get("verify") is True:
            self.LED_state = True
            self.wifi.led_on()
        if data.get("shutdown") is True:
            self.client.disconnect()
            self.write_out()
            print(f"{self.common_name} finished and disconnected.")
        
  
    def enroll(self):
        """Perform bootstrap enrollment using an event-driven publish/wait loop.

        Publishes our public key to the shared enroll/request topic and waits for a
        matching response that contains the assigned common_name. The MQTT network
        thread signals completion by setting self._enrolled_evt.
        """
        payload = {
            "dev_key": self.uuid_val,
            "device_type": "sensor_module",
        }
        # Single publish per request cycle
        self.client.publish("enroll/request", json.dumps(payload))
        
    def write_out(self):
        """Output device status to a file."""
        self.LED_state = False
        self.wifi.led_off()
        
        self.CONFIG["LABEL"] = self.label
        self.CONFIG["UUID"] = self.uuid_val
        self.CONFIG["registered"] = True

        with open(self.status_path, "w") as f:
            json.dump(self.CONFIG, f)
        self.finished = True
        print(f"Status written to {self.status_path}")
        soft_reset()
        