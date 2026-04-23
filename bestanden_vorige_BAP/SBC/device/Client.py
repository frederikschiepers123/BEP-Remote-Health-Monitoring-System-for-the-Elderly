# SBC/device/Client.py
from __future__ import annotations

# Import required packages
import paho.mqtt.client as mqtt
import json
import uuid

# Import other python files
from .config import *
from .utils import *
from .led_control import LED


class SBCClient:
    """Device client that performs enrollment then connects on its assigned topic."""
    def __init__(self, led: LED=LED(name="work", mode="builtin"), dev_uuid:str = None, finished: dict | bool = False):
        self.led = led
        self.led.state = "off"  # Make sure the LED is off at start
        while listen_for_broker_address() != 0: # First listen for broker advertisement
            pass
        # Specify initial parameters
        self.common_name = USERNAME
        self.port = 1884
        self.label = None
        self.connected = False
        self.finished = finished
        self.uuid = dev_uuid if dev_uuid is not None else str(uuid.uuid4())

        # Generate keys immediately so public_key is available for enroll
        self.client_key, self.client_public_key = generate_ecdsa_keys(PRIVATE_KEY_PATH)
        self.server_key, self.server_public_key = generate_ecdsa_keys(SERVER_KEY_PATH)
        # Connect to broker and start MQTT network thread
        self.connect()
    
    def connect(self):
        """Connect to the broker with given port and credentials. Sets up MQTT client and starts network thread.
        """
        if self.port == 1884 and self.common_name == "new_dev":
            client_id = f"{self.common_name}-{self.uuid.encode().hex()[:8]}" # Unique client ID during initial enrollment
        elif self.port == 8883: # Use assigned label as client ID after enrollment
            client_id = self.label
        else: # Use common_name as client ID for other cases
            client_id = self.common_name
        self.client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311) # Initialize MQTT client
        if self.port == 1884:   # Use either username/password or certs based on phase
            self.client.username_pw_set(USERNAME, PASSWORD)
            certfile = None
            keyfile = None
        else:
            certfile = self.cert_path
            keyfile = PRIVATE_KEY_PATH
        self.client.tls_set(ca_certs=CA_CERT_PATH, # Always use TLS, mutual when certs are provided
                            certfile=certfile, 
                            keyfile=keyfile, 
                            tls_version=mqtt.ssl.PROTOCOL_TLS_CLIENT, 
                            cert_reqs=mqtt.ssl.CERT_REQUIRED) 
        # Connect and map callback functions
        self.client.connect(BROKER, self.port, 60)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.loop_start()
    
    def disconnect(self):
        """Disconnects the MQTT client."""
        self.client.disconnect()
        self.connected = False
    
    def on_connect(self, client, userdata, flags, rc):
        """MQTT callback invoked upon successful connection. Connects to topics and optionally publishes checkup based on current phase."""
        print("Securely connected with result code ", rc, "port:", self.port)
        if self.common_name == "new_dev":
            self.client.subscribe("enroll/response", qos=1) # only subscribe to enrollment response during initial phase
            self.connected = True
            return
        if self.port == 1884:
            topic = f"enroll/{self.common_name}" # During enrollment phase, subscribe to own enroll topic
        else:
            topic = f"verification/{self.label}" # After enrollment, subscribe to verification topic
        self.client.subscribe(topic + "/response", qos=1)
        self.client.publish(topic + "/request", json.dumps({"checkup": True, "common_name": self.common_name}), qos=1)    # Handle checkup message centrally on reconnect
        self.connected = True
        return

    def on_message(self, client, userdata, msg):
        """MQTT callback for all subscribed topics during both phases. Dispatches to appropriate handler based on topic structure."""
        rec_text = msg.payload.decode()
        data = json.loads(rec_text)
        topic = msg.topic.split('/')
        match topic:
            case ["enroll", "response"]:
                self.enroll_handler(data)
            case ["enroll", _, "response"]:
                self.verify_handler(data)
            case ["verification", _, "response"]:
               self.finish_handler(data)
            case _:
                print("Unknown topic format:", msg.topic)
                return
            
    def enroll_handler(self, data):
        """Process enrollment request and common_name assignment."""
        if data.get("request") is True and self.common_name == "new_dev": # Try enrolling when the server requests it
            self.enroll()
        if (data.get("dev_key") == self.client_public_key) and self.common_name == "new_dev": # Reconnect with assigned common_name
            self.common_name = data.get("common_name")
            self.client.loop_stop()
            self.disconnect()
            while self.connected is not False:
                pass    # Wait for client to finish disconnecting
            self.connect()

    def enroll(self):
        """Send enrollment request with public key and device type."""
        payload = {
            "dev_key": self.client_public_key,
            "device_type": "SBC"
        }
        self.client.publish("enroll/request", json.dumps(payload), qos=1)
     
    def verify_handler(self, data):
        """Handle the nonce and LED verification messages during registration"""
        nonce = data.get("nonce")
        label = data.get("label")
        certificate = data.get("certificate")
        server_certificate = data.get("server_certificate")
        if nonce: 
            nonce_bytes = bytes.fromhex(nonce)
            sig = sign(self.client_key, nonce_bytes) # Sign the nonce to prove possession of private key
            self.led.state = "blink" # Start LED blinking
            payload = { # return signature and device credentials
                "signature": sig.hex(),
                "uuid": self.uuid,
                "mac": uuid.getnode().to_bytes(6, 'big').hex(),  # Send as integer
                "add_key": self.server_public_key
            }
            self.client.publish(f"enroll/{self.common_name}/request", json.dumps(payload), qos=1)
        if label and certificate: # Received certificate and label, finish enrollment
            self.disconnect()
            self.label = label
            self.led.state = "off" # Turn LED off
            # Save both certificates to files
            cert_str = certificate.strip()
            serv_cert_str = server_certificate.strip()
            pem = cert_str if cert_str.endswith("\n") else cert_str + "\n" # If it's PEM, write as-is; otherwise, assume base64 DER and wrap into PEM
            serv_pem = serv_cert_str if serv_cert_str.endswith("\n") else serv_cert_str + "\n"
            self.cert_path = BASE_DIR / f"{self.label}.crt"
            with open(SERVER_CERT_PATH, "w") as f:
                f.write(serv_pem)
            with open(self.cert_path, "w") as f:
                f.write(pem)
            self.port = 8883    # Switch to secure port
            while self.connected is not False:
                pass    # Wait for client to finish disconnecting
            self.connect()  # Reconnect with new credentials

    def finish_handler(self, data):
        """Handle credential response messages after receiving certificate."""
        verify = data.get("verify")
        shutdown = data.get("shutdown")
        if verify:
            self.led.state = "on" # Turn LED on for verification
        if shutdown:    # Shutdown command received, finalize and exit
            self.allowed_labels = data.get("allowed_labels")
            self.led.state = "off" # Turn LED off
            self.disconnect()
            self.write_out()

       
    def write_out(self):
        """Output device status to a file. Also create an ACL file based on allowed labels."""
        self.led.state = "off"
        write_out = {
            "label": self.label,
            "certificate": str(self.cert_path),
            "allowed_labels": self.allowed_labels 
        }
        with open(STATUS_PATH, "w") as f:   # Write status JSON file
            json.dump(write_out, f, indent=4)
        with open(ACL_FILE_PATH, "w") as f:   # Write ACL file
            f.write("# ACLFILE\n\n")
            f.write(f'user {self.label}\n')
            f.write(f'topic readwrite deployment/#\n\n')
            for label in self.allowed_labels:
                f.write(f"user {label}\n")
                f.write(f"topic write deployment/{label}/data\n")
                f.write(f"topic read deployment/{label}/response\n")
                f.write("topic read deployment/response\n\n")    
        self.finished = True # Signal that the client has finished
