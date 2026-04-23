import paho.mqtt.client as mqtt #from paho import mqtt
import json, time, datetime
import threading
import socket
import os
from .config import *


class DepClient:
    """Device client that performs bootstrap enrollment then connects on its assigned topic."""
    def __init__(self):
        # Until enrolled, devices use the placeholder common_name "new_dev"
        HOST_MAP["server"] = "localhost" # Since this script is always run from the SBC where also the mosquitto server is running, use the localhost
        self.read_data()
        self.deregistering_flag = False
        self.port = 8883
        self.connected = False
        self.connected_labels = set()
        self.delays = []
        t = datetime.datetime(2000, 1, 1, 0, 0, 0, 0, tzinfo=datetime.timezone.utc)
        # Seconds since UNIX epoch → convert to nanoseconds
        self.offset = int(t.timestamp() * 1e9)
        self.data_list = {}
        self.connect()
        # Periodic publish control
        self._publish_stop = threading.Event()
        self._publish_thread = None
        self.start()
    
    def connect(self):
        """Connect to the broker with mutual TLS and start MQTT network thread."""
        # Use a unique client_id during bootstrap to avoid clientID collisions across devices
        client_id = self.label
        self.client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
        self.client.tls_set(ca_certs=CA_CERT_PATH, 
                            certfile=self.cert_path, 
                            keyfile=PRIVATE_KEY_PATH, 
                            tls_version=mqtt.ssl.PROTOCOL_TLS_CLIENT, 
                            cert_reqs=mqtt.ssl.CERT_REQUIRED)
        self.client.connect(BROKER, self.port, 60)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        # Start background network thread to process MQTT I/O and callbacks
        self.client.loop_start()
        self.connected = True

    def disconnect(self):
        """Disconnects the MQTT client and stop the publish loop."""
        if hasattr(self, "_publish_stop"):
            self._publish_stop.set()
        if hasattr(self, "_publish_thread") and self._publish_thread and self._publish_thread.is_alive():
            self._publish_thread.join(timeout=1)
        self.client.disconnect()
        self.connected = False


    def start(self):
        """Starts the DepClient by connecting and starting periodic publishing."""
        # Start periodic publishing every 10 minutes
        try:
            print("Starting periodic publish every 10 minutes. Press Ctrl+C to deregister a device.")
            self.start_periodic_publish(interval_sec=600)
            timeout = 0
            while True:
                time.sleep(0.01)
                timeout += 1
                # check every 5 seconds if all allowed devices are connected
                if timeout > 500:
                    allowed_set = set(label_info["label"] for label_info in self.allowed_labels if label_info["active"])
                    if not self.connected_labels == allowed_set:
                        # Ping all devices if a connection is missing
                        try:
                            print("Allowed set:", allowed_set)
                            print("Connected labels:", self.connected_labels)
                            self.connected_labels = set() # Reset connected labels to force re-connection   
                            # Send ping to all devices
                            self.client.publish(
                                "deployment/response",
                                json.dumps({"ping": True}),
                                qos=1
                            )
                            # Advertise the broker address via UDP broadcast
                            self.advertise_broker_address(advertise_port=5005)
                        except Exception as ex:
                            print("Advertisement error:", ex)
                    timeout = 0
        except KeyboardInterrupt:   # Use the keyboard interrupt to trigger deregistration
            print("Periodic publish interrupted by user. Starting deregistration.")
            self.deregister()
        except Exception as e:
            print("Error in DepClient main loop:", e)
        finally:
            cont = input("Do you want to exit the client? Type 'exit' to confirm. Any other key will restart periodic publishing. ")
            if cont.strip().lower() == "exit":
                print("Exiting DepClient.")
                self.stop()
            else:
                self.start()
                
    def read_data(self):
        """Reads existing device status from a file."""
        with open(STATUS_PATH, "r") as f:
            status_data = json.load(f)
        self.label = status_data.get("label", None)
        self.cert_path = status_data.get("certificate", None)
        self.allowed_labels = [{"label": label, "active": True} for label in status_data.get("allowed_labels", [])]

    def on_connect(self, client, userdata, flags, rc):
        """MQTT callback invoked upon successful connection."""
        print("Securely connected with result code ", rc, 'on port', self.port)
        self.client.subscribe(f"deployment/+/data")
        self.connected = True

    def on_message(self, client, userdata, msg):
        """MQTT callback function invoked when a message is received."""
        rec_text = msg.payload.decode()
        topic = msg.topic.split('/')
        while self.deregistering_flag is True:
            time.sleep(0.1) # Wait if deregistering is in progress
        match topic:
            case ["deployment", _, "data"]:
                self.write_data(rec_text, topic[1])
            case _:
                print("Unknown topic format:", msg.topic)
                
    
    def write_data(self, msg, label):
        """Either updates connected devices or writes received data to the device's data file."""
        data = json.loads(msg)
        if label in [label_info["label"] for label_info in self.allowed_labels if label_info["active"]]:
            # Update connected devices set when a device connects
            if data.get("connected") == "True":
                print(f"Device {label} connected., self.connected_labels: {self.connected_labels}")
                self.connected_labels.add(label)
                allowed = set(label_info["label"] for label_info in self.allowed_labels if label_info["active"])
                if self.connected_labels == allowed:
                    print("All allowed devices connected.")
                else:
                    print(f"Still missing connections from: {allowed - self.connected_labels}")
            else:
                # Add timestamp to received data and print it to terminal
                data["SBC_timestamp"] = list(time.gmtime()[:8])
                print(f"Received data from device {label}: {data}")
                # Send data to the database server here
                
        else:
            print(f"Received data for unrecognized label {label}, ignoring.")
        return
        
    

    def start_periodic_publish(self, interval_sec: int = 600):
        """Start a background thread that publishes every interval_sec without blocking."""
        if self._publish_thread and self._publish_thread.is_alive():
            return  # already running

        def _run():
            # First publish immediately, then every interval
            next_time = 0
            while not self._publish_stop.is_set() and self.connected:
                now = time.monotonic()
                if now >= next_time:
                    try:
                        print("Publishing periodic ping to deployment/response")
                        self.client.publish(
                            "deployment/response",
                            json.dumps({"ping": True,
                                        "timestamp": list(time.gmtime()[:8])}),
                            qos=1
                        )
                    except Exception as ex:
                        print("Publish error:", ex)
                    next_time = now + interval_sec
                # Sleep a little, but remain responsive to stop signal
                self._publish_stop.wait(timeout=1)

        self._publish_stop.clear()
        self._publish_thread = threading.Thread(target=_run, name="PeriodicPublish", daemon=True)
        self._publish_thread.start()
    
    def stop(self):
        """Stops the DepClient by publishing goodbye and disconnecting the MQTT client."""
        self.client.publish("deployment/response", json.dumps({"shutdown": True}), qos=1)
        self.disconnect()
        with open(DELAY_PATH, "w") as delay_file:
            for delay in self.delays:
                delay_file.write(f"{delay}\n")
        with open(DATA_PATH, "w") as data_file:
            json.dump(self.data_list, data_file, indent=4)

    def deregister(self):
        """Deregisters the device by deleting its certificate and key files or deactivates it by stopping its data publishing."""
        self.deregistering_flag = True # Set flag to pause message processing during deregistration
        state = input("Do you want to deregister or (de)activate a device? (dr/da/a): ").strip().lower() # Select action to perform
        print("Select your device:") # Select device to deregister or deactivate
        i = 0
        for label in self.allowed_labels:
            # print out possible options
            print(f"{i}: {label}")
            i += 1
        try:
            choice = int(input("Enter the number corresponding to the device, or -1 for the SBC "))
            if choice < -1 or choice >= len(self.allowed_labels): 
                raise ValueError
        except ValueError:
            print("Invalid input. Exiting deregistration.")
            return
        if choice == -1:
            # Deregister the SBC itself
            self.client.publish("deployment/response", json.dumps({"action": "deregister_sbc"}), qos=1)
            os.remove(STATUS_PATH)
            os.remove(PRIVATE_KEY_PATH)
            os.remove(self.cert_path)
            os.remove(SERVER_CERT_PATH)
            os.remove(SERVER_KEY_PATH)
            os.remove(ACL_FILE_PATH)
            # persist deregistration to database server here
            print("SBC deregistered and data deleted.")
            self.stop()
            return 0
        label_info = self.allowed_labels[choice]
        match state:
            case "dr": # deregister
                self.allowed_labels.pop(choice)
                payload = {"action": "deregister"}
                # persist deregistration to database server here
                print(f"Deregistered device {label_info['label']}.")
            case "da": # deactivate
                self.allowed_labels[choice]["active"] = False
                payload = {"action": "deactivate"}
                print(f"Deactivated device {label_info['label']}.")
            case "a": # activate
                self.allowed_labels[choice]["active"] = True
                payload = {"action": "activate"}
                print(f"Activated device {label_info['label']}.")
            case _:
                print("Invalid state. Exiting deregistration.")
        self.client.publish(f"deployment/{label_info['label']}/response", json.dumps(payload), qos=1)
        print(f"Processed {payload['action']} for device {label_info['label']}.")
        self.deregistering_flag = False
        return
    
    def advertise_broker_address(self, advertise_port: int) -> None:
        """Sends a UDP broadcast advertisement over the gloal broadcast address. The IP of the sender can be used for MQTT clients to connect to the broker."""
        broadcast_ip = "255.255.255.255"
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # Create a UDP socket.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)  # Enable permission to send broadcast packets.
        #Sends a UDP broadcast advertisement of the MQTT broker's IP and port.
        message_str = ""
        for label in self.allowed_labels:
            msg = label["label"] if label['active'] else ""
            message_str += "{},".format(msg)
        print(message_str)
        message_bytes = message_str.encode('utf-8')  # Convert the message to bytes for sending.
        
        try:
            # Send to global broadcast, print any errors
            sock.sendto(message_bytes, (broadcast_ip, advertise_port))
        except Exception as e:
            print(f"Error sending advertisement: {e}")
