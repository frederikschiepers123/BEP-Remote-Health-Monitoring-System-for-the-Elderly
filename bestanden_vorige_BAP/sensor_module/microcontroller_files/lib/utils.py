import random
import os
import hashlib
from ed25519 import *
import usocket as socket

def generate_ed25519_private_key() -> bytes:
    """Generate a 32-byte private key."""
    return os.urandom(32)

def derive_ed25519_public_key(private_key: bytes) -> bytes:
    """Derive 32-byte public key from private key."""
    return publickey_unsafe(private_key)

def sign_message_ed25519(message: bytes, private_key: bytes, public_key: bytes | None = None) -> bytes:
    """Sign message with private key. Provide public_key for faster signing if already derived."""
    if public_key is None:
        public_key = derive_ed25519_public_key(private_key)
    return signature_unsafe(message, private_key, public_key)

def verify_message_ed25519(message: bytes, signature: bytes, public_key: bytes) -> bool:
    """Verify signature using public key."""
    try:
        checkvalid(signature, message, public_key)
        return True
    except Exception:
        return False


def listen_for_broker_address() -> int| None:
        """
        Listen for a UDP broadcast advertisement from the broker.

        Expected payload format: "IP_ADDRESS:MQTT_PORT" (e.g., "192.168.1.10:1884").
        On success, updates HOST_MAP["server"] with the advertised IP and returns (ip, port).
        Returns None on failure/timeout.
        """
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Not strictly required for receiving, but harmless and clarifies intent
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        except OSError:
            pass

        # Bind to all interfaces on the given port
        sock.bind(("", 5005))
        print(f"Listening for broker on UDP port 5005 (broadcast) ...")
        while True:
            try:
                data, addr = sock.recvfrom(1024)
                data = data.decode("utf-8")
                data = data.split(",")
                #print(f"Received advertisement: {message_str} from {addr[0]}")
                # Expected format: "IP_ADDRESS:MQTT_PORT"
                #if message_str == "Hello from MQTT Broker":
                    #print(f"Set broker address to {addr[0]}")
                #else:
                    #print(f"Unexpected advertisement format: {message_str}, retrying...")
                    #return
                return addr[0], data
            except Exception as e:
                print(f"UDP listen error: {e}")
                return
            finally:
                try:
                    sock.close()
                except Exception:
                    pass