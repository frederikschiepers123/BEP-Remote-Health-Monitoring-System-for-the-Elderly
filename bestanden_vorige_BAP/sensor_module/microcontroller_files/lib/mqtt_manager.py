# mqtt_manager.py
#
# Reusable MQTT manager class for MicroPython.
# - Connect / reconnect with retry
# - Publish JSON or raw messages
# - Subscribe + receive via callback
# - check_messages() for non-blocking receive
#
# Use together with your WiFi class and main loop.

import time
try:
    import ujson as json
except ImportError:
    import json

try:
    import ussl as ssl
except ImportError:
    import ssl

from umqtt.simple import MQTTClient


class MQTTManager:
    def __init__(self, broker, keepalive=60, debug=False, msg_callback=None,
        ca_cert_path="/certs/ca.crt",
        client_cert_path="/certs/client.crt",
        client_key_path="/certs/private_key.pem"
    ):
        self.last_ping = 0
        self.ping_interval = keepalive // 2 or 30  # used to set the ping interval
        self.broker = broker          # hostname, e.g. "server"
        self.keepalive = keepalive
        self.debug = debug
        self.client = None
        self.connected = False
        self.msg_callback = msg_callback

        # TLS files
        self.ca_cert_path = ca_cert_path
        self.client_cert_path = client_cert_path
        self.client_key_path = client_key_path

        # Connection state (used for reconnect)
        self.secure = False           # False = 1884 TLS+pass, True = 8883 mTLS
        self.client_id = "MQTTClient"
        self.username = None          # only for 1884
        self.password = None          # only for 1884

    # ---------- internal connect ----------

    def _connect(self, secure=None, client_id=None, username=None, password=None, cert=None, key=None):
        # Use stored state by default
        if secure is None:
            secure = self.secure
            
        if client_id is None:
            client_id = self.client_id
            
        if key is None:
            key = self.client_key_path
            
        if cert is None:
            cert = self.client_cert_path

        self.secure = secure
        self.client_id = client_id
        if username is not None:
            self.username = username
        if password is not None:
            self.password = password

        if self.debug:
            print("[MQTT] Connecting to {} (secure={})".format(self.broker, secure))

        # Load CA certificate
        with open(self.ca_cert_path, "rb") as f:
            ca_cert = f.read()
        if self.debug:
            print("[MQTT] CA cert loaded from", self.ca_cert_path)

        # Decide TLS mode
        if secure:
            # --- mutual TLS on 8883 ---
            if self.debug:
                print("[MQTT] Using mutual TLS (client cert+key) on 8883")
            with open(self.client_cert_path, "rb") as f:
                client_cert = f.read()
            with open(self.client_key_path, "rb") as f:
                client_key = f.read()

            ssl_params = {
                "cert_reqs": ssl.CERT_REQUIRED,
                "cadata": ca_cert,
                "cert": client_cert,
                "key": client_key,
                "server_hostname": "server" #self.broker,  # CN = "server" -- change to self.broker for SBC connection
            }
            port = 8883
            user = None
            password = None
        else:
            # --- TLS + username/password on 1884 ---
            if self.debug:
                print("[MQTT] Using TLS + user/pass on 1884")
            ssl_params = {
                "cert_reqs": ssl.CERT_REQUIRED,
                "cadata": ca_cert,
                "server_hostname": "server"  #self.broker,  # CN = "server"
            }
            port = 1884
            user = self.username
            password = self.password

        # Create MQTT client
        kwargs = {
            "client_id": client_id,
            "server": self.broker,  # << no hard-coded IP anymore
            "port": port,
            "keepalive": self.keepalive,
            "ssl": True,
            "ssl_params": ssl_params,
        }
        if user is not None:
            kwargs["user"] = user
        if password is not None:
            kwargs["password"] = password

        self.client = MQTTClient(**kwargs)
        if self.msg_callback:
            self.client.set_callback(self.msg_callback)

        self.client.connect()
        self.connected = True

        if self.debug:
            print("[MQTT] Connected (port {})".format(port))

    # ---------- public connect with retry ----------

    def connect(
        self,
        secure=False,
        client_id=None,
        username=None,
        password=None,
        cert=None,
        key=None,
        retries=5,
        delay=2,
    ):
        """Connect to the broker, retrying a few times.

        secure=False -> TLS + username/password on port 1884
        secure=True  -> mutual TLS on port 8883

        Returns True if connected, False otherwise.
        """
        if client_id is not None:
            self.client_id = client_id
        if username is not None:
            self.username = username
        if password is not None:
            self.password = password
            
        if cert is not None:
            self.client_cert_path = cert
            
        if key is not None:
            self.client_key_path = key
            
        self.secure = secure

        if self.debug:
            print(
                "[MQTT] connect(): secure={}, client_id={}, user={}".format(
                    self.secure, self.client_id, self.username
                )
            )

        for _ in range(retries):
            print("connecting")
            try:
                self._connect()
                return True
            except OSError as e:
                if self.debug:
                    print("[MQTT] Connect failed:", e)
                time.sleep(delay)

        self.connected = False
        return False

    # ---------- ensure connection / auto reconnect ----------

    def ensure_connection(self):
        if not self.connected:
            return self.connect(secure=self.secure)

        now = time.time()
        # Only ping if enough time has passed
        if now - self.last_ping < self.ping_interval:
            return True

        try:
            if self.debug:
                print("[MQTT] Pinging broker...")
            try:
                # shorten timeout if available
                self.client.sock.settimeout(5)
            except AttributeError:
                pass
            self.client.ping()
            self.last_ping = time.time()
            if self.debug:
                print("[MQTT] Ping OK")
            return True
        except OSError as e:
            if self.debug:
                print("[MQTT] Ping failed with:", e, "– reconnecting...")
            self.connected = False
            return self.connect(
                secure=self.secure,
                client_id=self.client_id,
                username=self.username,
                password=self.password,
            )

    # ---------- subscribe / callback ----------

    def subscribe(self, topic, callback=None):
        """Subscribe to a topic (str or bytes)."""
        if isinstance(topic, str):
            topic = topic.encode("utf-8")

        if callback:
            self.msg_callback = callback
            if self.client:
                self.client.set_callback(callback)

        if not self.ensure_connection():
            return False

        self.client.subscribe(topic)
        if self.debug:
            print("[MQTT] Subscribed to:", topic)
        return True

    # ---------- publish ----------

    def publish(self, topic, payload):
        """Publish a raw payload (str or bytes)."""
        if isinstance(topic, str):
            topic = topic.encode("utf-8")
        if isinstance(payload, str):
            payload = payload.encode("utf-8")

        if not self.ensure_connection():
            return False

        try:
            self.client.publish(topic, payload)
            if self.debug:
                print("[MQTT] Published:", payload)
            return True
        except OSError as e:
            if self.debug:
                print("[MQTT] Publish failed:", e)
            self.connected = False
            return False

    def publish_json(self, topic, data):
        """Publish a dict/list as JSON."""
        return self.publish(topic, json.dumps(data))

    # ---------- incoming messages ----------

    def check_messages(self, ping=True):
        """Non-blocking check for incoming messages.

        Messages are delivered to the callback set via subscribe().
        """
        if ping and not self.ensure_connection():
            return

        try:
            self.client.check_msg()
        except OSError:
            self.connected = False

    def wait_for_message(self):
        """Blocking wait for a single incoming message."""
        if not self.ensure_connection():
            return None
        try:
            return self.client.wait_msg()
        except OSError:
            self.connected = False
            return None

    # ---------- disconnect ----------

    def disconnect(self):
        try:
            if self.client:
                self.client.disconnect()
                self.connected = False
        except OSError:
            pass
        self.connected = False
        if self.debug:
            print("[MQTT] Disconnected")
