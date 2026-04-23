# src/mqtt/base_client.py
from __future__ import annotations
# Import packages
import socket
import paho.mqtt.client as mqtt
import ssl
from registration_software.common.config import settings
from registration_software.registration.ca_manager import CAManager

class BaseMQTTClient:
    """
    Abstract base class for MQTT clients/servers.
    Handles connection setup, TLS configuration, and client identity management.
    """
    def __init__(self, ca_manager: CAManager, port: int, client_id: str, tls_required: bool = True, username: str = None, password: str = None):
        self.ca_manager = ca_manager
        self.client_id = client_id
        
        # Ensure the server client has its own certificate signed by the CA
        self.ca_manager.load_or_generate_identity(
            settings.paths.client_cert,
            settings.paths.client_key,
            settings.registration.client_name,
            purpose="client"
        )

        
        # Initialize mqtt client with initial username and password and TLS settings
        self.client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)
        
        if tls_required:
            self.client.tls_set(
                ca_certs=settings.paths.ca_cert, 
                certfile=settings.paths.client_cert, 
                keyfile=settings.paths.client_key, 
                tls_version=ssl.PROTOCOL_TLS_CLIENT, 
                cert_reqs=ssl.CERT_REQUIRED
            )
            self.client.tls_insecure_set(False)
        else:
            self.client.username_pw_set(username, password)
        

        # Set connection and message handlers
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message_router
        
        # Increase MQTT throughput
        try:
            self.client.max_inflight_messages_set(100)
            self.client.max_queued_messages_set(1000)
        except Exception:
            pass # Ignore if version doesn't support
            
        # Connect to the broker
        self.client.connect(settings.mqtt.host, port, 60)

    def start_loop(self):
        """Starts the MQTT network thread."""
        self.client.loop_start()

    def stop_loop(self):
        """Stops the MQTT network thread and disconnects."""
        self.client.loop_stop()
        self.client.disconnect()

    def _on_connect(self, client, userdata, flags, rc):
        """Generic connection handler, calls specialized method."""
        print(f"{self.client_id} connected with result code: {rc}")
        self.on_connect(client, userdata, flags, rc)

    def _on_message_router(self, client, userdata, msg):
        """Routes messages to the specialized handler."""
        self.on_message(client, userdata, msg)

    def advertise_broker_address(self, advertise_port: int) -> None:
        """Sends a UDP broadcast advertisement over the gloal broadcast address. The IP of the sender can be used for MQTT clients to connect to the broker."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # Create a UDP socket.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)  # Enable permission to send broadcast packets.
        #Sends a UDP broadcast advertisement of the MQTT broker's IP and port.
        message_str = f"Hello from MQTT Broker"  # The message to broadcast.
        message_bytes = message_str.encode('utf-8')  # Convert the message to bytes for sending.
        
        try:
            # Send to global broadcast, print any errors
            sock.sendto(message_bytes, (settings.mqtt.advertise_ip, advertise_port))
        except Exception as e:
            print(f"Error sending advertisement: {e}")

    # Abstract methods to be implemented by subclasses
    def on_connect(self, client, userdata, flags, rc):
        """Subclass implementation of on_connect."""
        raise NotImplementedError("Subclasses must implement on_connect")

    def on_message(self, client, userdata, msg):
        """Subclass implementation of on_message."""
        raise NotImplementedError("Subclasses must implement on_message")