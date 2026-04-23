# src/registration/registration_client.py
from __future__ import annotations

# Import packages
import json
import threading
import secrets
import string
import time

from registration_software.common.config import settings
from registration_software.common.crypto import verify_signature, generate_nonce, pem_to_public_key
from registration_software.mqtt.base_client import BaseMQTTClient
from registration_software.registration.ca_manager import CAManager
from registration_software.registration.registry import DeviceRegistry
from registration_software.identification import ActiveIdentificationTracker
from registration_software.models.enums import IdentificationEvent

class RegistrationClient(BaseMQTTClient):
    """
    Registration enrollment coordinator. Subclasses BaseMQTTClient for network functionality.
    Manages the three-phase enrollment process: device enrollment, identification and verification.
    1. Enrollment Phase:
        - Broadcast enrollment requests.
        - Receives device public keys and assigns temporary names.
    2. Identification Phase:
        - Waits for checkup messages from enrolled devices, re-issues temporary name for devices left behind.
        - Verifies device ownership via nonce-signature challenge.
        - Assigns unique alphanumeric labels (manual, auto, or via tracker).
        - Assigns client (and server) certificates.
    3. Verification Phase:
        - Waits for checkup messages from enrolled device, re-issues nonce challenges for devices that were left behind.
        - Publishes CA-signed client certificates to verified devices.
        - Persists device records in the DeviceRegistry.
    """
    def __init__(self, ca_manager: CAManager, data_registry: DeviceRegistry,
                 identifier: ActiveIdentificationTracker=None, manual: bool = False, auto: bool = False):
        super().__init__(ca_manager=ca_manager, port=settings.mqtt.port_secure, client_id=settings.registration.client_name, tls_required=True)
        # Initialize dependencies
        self.ca_manager = ca_manager
        self.registry = data_registry
        self.identifier = identifier
        self.manual = manual
        self.auto = auto
        self.enrolling_devices = {}
        
        # Initialize internal flags and threading events
        self._verify_evt = threading.Event()
        self._lock = threading.Lock()
        self.break_verify = False
        self.finished = False
        self.checkup_flag = False
        
    # --- BaseMQTTClient Abstract Methods Implementation ---
    def on_connect(self, client, userdata, flags, rc):
        """Subscribe to registration topics upon connection."""
        print(f"{self.client_id} securely connected with result code: {rc}")
        self.client.subscribe("enroll/request", qos=1)
        self.client.subscribe("enroll/+/request", qos=1)
        self.client.subscribe("verification/+/request", qos=1)
        
    def on_message(self, client, userdata, msg):
        """Route incoming enroll messages to the proper handler. Check for any errors."""
        try:
            rec_text = msg.payload.decode()
            data = json.loads(rec_text)
            topic = msg.topic.split("/")
            match topic:
                case ["enroll", "request"]:  # New device enrollment request
                    self.new_dev_handler(data)
                case ["enroll", _, "request"]:   # Handles either the nonce response or checkup message
                    if data.get("checkup") is True:
                        self.verify_handler(data, topic[1])
                    else:
                        self.dev_rsp_handler(data, topic[1])
                case ["verification", _, "request"]:    # This topic only receives checkup messages
                    self.verify_handler(data, topic[1])
                case _:
                    print(f"Unknown topic format: {msg.topic}")
        except json.JSONDecodeError as e:
            print(f"Error decoding JSON payload: {e}")
        except Exception as e:
            print(f"Error handling message: {e}, {msg.topic}, {msg.payload.decode()}")
            
    # --- Enrollment Lifecycle Methods ---
    
    def start(self):
        """ Run the registration pipeline: start -> enroll -> verify -> finish. """
        self.start_loop()
        self.verify_enroll()
        self.verify_label()
        self.finish()
     
    def verify_enroll(self, old_count=0):
        """Publish enrollment requests and wait for device responses. Continues when no clients respond for a set time."""
        idle_ticks = 0
        while idle_ticks < settings.registration.enroll_idle_threshold or len(self.enrolling_devices) == 0: # Do not continue if no device have tried to register
            self.request()
            if self._verify_evt.wait(timeout=1.0): # Wait a second for a new device, then publish a new request 
                idle_ticks = 0
                self._verify_evt.clear()
            else:
                idle_ticks += 1
        self.retry("enroll", old_count)    # Retry any devices that did not respond
    
    def verify_label(self, old_count=0):
        """Publish label verification requests and wait for device responses. Continues when no clients respond for a set time."""
        labels_to_collect = list(self.enrolling_devices.values())
        for device in labels_to_collect:
            self.get_label(device)
            if self._verify_evt.wait(timeout=settings.registration.label_timeout):    # Wait for device label verification
                self._verify_evt.clear()
            else:
                print("Label wait timeout reached. continuing without label")
        self.retry("label", old_count)  # Retry any devices that did not respond

    def request(self):
        """Broadcast enrollment request to all devices."""
        print("Broadcasting enrollment request...")
        self.advertise_broker_address(settings.mqtt.advertise_port)
        self.client.publish("enroll/response", json.dumps({"request": True}), qos=0)

    def _check_enroll(self, devices_to_check):
        """Per-device checkup and targeted mapping republish."""
        print("Performing checkup...")
        for d in devices_to_check:
            dev_key, common_name = d.get("dev_key"), d.get("common_name")
            payload = {"dev_key": dev_key, "common_name": common_name}
            self.client.publish("enroll/response", json.dumps(payload), qos=1)
            print(f"Sent another response for {common_name}")
        self.verify_all("enroll", len(devices_to_check))
        
    def _check_label(self, devices_to_check):
        """Per-device checkup and targeted label requests."""
        print("Performing label checkup...")
        for d in devices_to_check:
                if d.get("checkup") is False:
                    if d.get("label") is None:
                        self._get_label(d)
                    else:
                        self._present_certificate(d)
        self.verify_all("label_again")
        
    def _checkup_reset(self):
        """Reset all checkup events and flags."""
        for d in self.enrolling_devices.values():
            d["checkup"] = False
        self.checkup_flag = False

    def get_label(self, device_data):
        """Send a nonce to the device and wait for signature + label assignment."""
        nonce = generate_nonce()
        if not device_data: return
        device_data["nonce"] = nonce 
        payload = {
            "nonce": nonce,
            "message": "LED on" # Request device action for physical verification
        }
        self.client.publish(f"enroll/{device_data['common_name']}/response", json.dumps(payload), qos=1)
   
    def retry(self, state, old_count):
        """Find devices that have not responded and check for progress in re-issuing requests."""
        devices_to_check = [dev for dev in self.enrolling_devices.values() if dev.get("checkup") is False] 
        if len(devices_to_check) == old_count: # If the no retried device responded, exit
            print(f"No progress in {state} phase; moving on.")
            self.checkup_reset()
            return
        if state == "enroll":
            self.check_enroll(devices_to_check)
        elif state == "label":
            self.check_label(devices_to_check)

    def check_enroll(self, devices_to_check):
        """Republish enrollment responses to devices that have not yet responded."""
        for d in devices_to_check:
            dev_key, common_name = d.get("dev_key"), d.get("common_name")   # Republish previous enrollment response
            payload = {"dev_key": dev_key, "common_name": common_name}
            self.client.publish("enroll/response", json.dumps(payload), qos=1)
        self.verify_enroll(len(devices_to_check)) # Verify again for non-responding devices
        
    def check_label(self, devices_to_check):
        """Republish either nonce challenge or the certificates to devices that have not yet responded."""
        for d in devices_to_check:
                if d.get("checkup") is False:
                    if d.get("label") is None: # Republish nonce challenge
                        self.get_label(d)
                    else:
                        self.present_certificate(d) # Republish certificates
        self.verify_label(len(devices_to_check)) # Verify again for non-responding devices
        
    def checkup_reset(self):
        """Reset all checkup events and flags for the next phase."""
        for d in self.enrolling_devices.values():
            d["checkup"] = False
        self.checkup_flag = False    
        
    def new_dev_handler(self, data: dict) -> None:
        """Handles new device enrollment requests."""
        dev_key = data.get("dev_key")
        device_type = data.get("device_type")
        if dev_key and not any(dev_key == dev["dev_key"] for dev in self.enrolling_devices.values()): # check if the device is actually new
            with self._lock:
                new_id = len(self.enrolling_devices) # Assign new common name based on sequential ID
                common = f"device_{new_id}"
                
                payload = {"dev_key": dev_key, "common_name": common}
                self.client.publish("enroll/response", json.dumps(payload), qos=1) # Respond with assigned common name
                
                # Initialize defaults for new device record in internal *temporary* state
                self.enrolling_devices[common] = {
                    "common_name": common,
                    "dev_key": dev_key,
                    "device_type": device_type,
                    "label": None,
                    "mac_address": None,
                    "uuid": None,
                    "nonce": None,
                    "checkup": False
                }
                
    def verify_handler(self, data, common_name=None):
        """Handles the verification 'checkup' messages from devices."""
        new_name = data.get("common_name")
        if new_name: # The common name can not always be derived from the topic, so take it from the message if provided
            common_name = new_name
        if data.get("checkup") is True:
            print(f"Received checkup from {common_name}")
            try:
                with self._lock:
                    self.enrolling_devices[common_name]["checkup"] = True
            except Exception as e:
                print(f"Error updating checkup for {common_name}: {e}")
            self._verify_evt.set()  # Notify waiting threads that a device has responded
            

    def dev_rsp_handler(self, data, common_name):
        """Handle device responses to nonce challenges and verify signatures."""
        try: # Handle device response to nonce challenge
            sig = data.get("signature")
            if sig:
                device_data = self.enrolling_devices.get(common_name) # Retrieve device record, return if not found
                if device_data is None:
                    print(f"No record for {common_name}; cannot verify signature.")
                    self.break_verify = True
                    return
                nonce = device_data["nonce"] # Get the issued nonce for this device
                if nonce is None:
                    raise RuntimeError(f"No nonce issued for {common_name}; cannot verify signature.")
                if device_data["device_type"] == "sensor_module":   # Simple equality check for sensor modules
                    verified = sig == nonce
                else:
                    nonce_bytes = bytes.fromhex(nonce)
                    sig_bytes = bytes.fromhex(sig)
                    public_key = pem_to_public_key(device_data.get("dev_key").encode())
                    verified = verify_signature(public_key, nonce_bytes, sig_bytes)
                if verified:
                    # Signature verified successfully, store device info and proceed to label assignment
                    uuid = data.get("uuid") 
                    mac_address = data.get("mac")
                    add_key = data.get("add_key")
                    device_data["mac_address"] = mac_address
                    device_data["uuid"] = uuid
                    if add_key:
                        device_data["add_key"] = add_key
                    device_data["nonce"] = None # Nonce used and cleared
                    if self.identifier is None:
                        if self.manual: # Manual label entry
                            label = ""
                            while label == "":
                                label = input(f"Enter label for device {common_name} if led is on").strip()
                            device_data["label"] = label
                        else:   
                            device_data["label"] = self.generate_unique_label() # Auto-generate unique label
                    else:   # Use active identification tracker to assign label
                        found = False
                        timeout = 0
                        while found == False and timeout < settings.registration.label_timeout * 100:
                            event = self.identifier.get_events()
                            if event:
                                if event["type"] == IdentificationEvent.DEVICE_FOUND:   # Device found by tracker
                                    pass
                                elif event["type"] == IdentificationEvent.IDENTIFIED:   # Device LED identified by tracker
                                    device_data["label"] = event['label']
                                    found = True
                            time.sleep(0.01)
                            timeout += 1
                        if found == False:
                            raise RuntimeError("Device identification via tracker timed out.")
                    payload = self.present_certificate(device_data)     # Publish certificates and label
                    self.client.publish(f"enroll/{common_name}/response", json.dumps(payload), qos=1)
                else:
                    raise RuntimeError("Signature verification failed.")
        except Exception as e: # Any error in handling the device response causes verification to fail
            print(f"Error handling device response from {common_name}: {e}")   
            verified = False
            with self._lock:
                self.enrolling_devices.pop(common_name, None)
                
    def generate_unique_label(self) -> str:
        """
        Generate and ensure a unique 4-character alphanumeric label.
        Checks against both current enrollment devices and the persistent registry.
        """
        while True:
            label_code = ''.join(secrets.choice(string.ascii_letters + string.digits) for _ in range(4))
            
            # Check against currently enrolling devices
            is_in_current = any(d.get("label") == label_code for d in self.enrolling_devices.values())
            
            # Check against persistent registry
            if label_code not in self.registry.get_labels() and not is_in_current:
                return label_code
            
    def present_certificate(self, device: dict):
        """Generate and publish CA-signed client certificates to each labeled device and persist the record."""
        label = device["label"]
        if device["device_type"] == "sensor_module":
            public_key = pem_to_public_key(device.get("add_key").encode()) # Get the public key for sensor modules
            # Use CAManager to sign the client certificate
            cert_pem = self.ca_manager.sign_certificate(
                client_public_key=public_key, 
                common_name=label, 
                validity_days=365
            )
            # Payload with certificate
            payload = {
                "label": label,
                "certificate": cert_pem
            }
        else:
            dev_key = pem_to_public_key(device.get("dev_key").encode()) # Get the device public key
            server_key = pem_to_public_key(device.get("add_key").encode()) # Get the server public key
            # Use CAManager to sign the client and server certificates
            cert_pem = self.ca_manager.sign_certificate(
                client_public_key=dev_key, 
                common_name=label, 
                validity_days=365
            )
            serv_pem = self.ca_manager.sign_certificate(
                client_public_key=server_key, 
                common_name="server", 
                validity_days=365,
                purpose="server"
            )
            
            # Payload with certificates
            payload = {
                "label": label,
                "certificate": cert_pem,
                "server_certificate": serv_pem
            }
        return payload

    def finish(self):
        """Finish the registration process by publishing verification messages and persisting device records."""
        print("Publishing verify message")
        SM_labels = []
        allowed_labels = []
        for label, device_type in [(dev["label"], dev["device_type"]) for dev in self.enrolling_devices.values()]:
            if device_type == "sensor_module": # Collect sensor module labels for SBC assignment
                SM_labels.append(label) 
            self.client.publish(f"verification/{label}/response", json.dumps({"verify": True}), qos=1) # Initial verify message to turn on LEDs
        print(SM_labels)
        if self.auto is False: # Wait for user confirmation before sending shutdown signals
            input("Press Enter to send shutdown signal to all devices...") 
        else:
            time.sleep(0.5)  # brief pause before shutdown in auto mode
        for device in self.enrolling_devices.values():
            if device["device_type"] == "SBC":  # For SBC devices, assign allowed sensor module labels
                if self.auto is False:
                    for label in SM_labels:
                        answer = input(f"Do you want to assign label {label} to SBC device {device['label']}(y/n)? ") # ask user what sensor modules to assign to which SBCs
                        if answer.lower() == 'y':
                            allowed_labels.append(label)
                    
                    payload = {
                        "allowed_labels": allowed_labels,
                        "shutdown": True
                    }
                    for label in allowed_labels:
                        SM_labels.remove(label)  # remove assigned labels from the pool
                    allowed_labels = []  # reset for next SBC
                else:
                    payload = {
                        "allowed_labels": [],
                        "shutdown": True
                    }
            else:
                payload = {
                    "shutdown": True
                }
            self.client.publish(f"verification/{device['label']}/response", json.dumps(payload), qos=1)
            mac = device["mac_address"]
            self.registry.add_registered_device( # persist device record in registry
                mac_address=mac, 
                uuid=device["uuid"],
                device_type=device["device_type"], 
                label=device["label"]
            )
        self.registry.persist_to_json() # Save registry to disk
        self.stop_loop()
        print("Registration client finished and disconnected.")