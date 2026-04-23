# SBC/device/utils.py
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization, hashes
import socket
from .config import *

def generate_ecdsa_keys(private_key_path: str = "./private.key") -> tuple[ec.EllipticCurvePrivateKey , ec.EllipticCurvePublicKey]:
    """Generates an ECDSA key pair and saves the private key to a PEM file. Returns the private and public key objects."""
    private_key = ec.generate_private_key(ec.SECP256R1())
    public_key = private_key.public_key().public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo
        ).decode()
    return _serialize_and_save(private_key, private_key_path), public_key

def _serialize_and_save(private_key_obj: ec.EllipticCurvePrivateKey, private_key_path: str) -> ec.EllipticCurvePrivateKey | str:
    """Internal method to serialize keys and save them directly as PEM files. Return them as well so they can be cached."""
    private_bytes = private_key_obj.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()
    )
    with open(private_key_path, "wb") as f:
        f.write(private_bytes)
    return private_key_obj


def listen_for_broker_address() -> int| None:
        """
        Listen for a UDP broadcast advertisement from the broker.

        Expected payload format: "IP_ADDRESS:MQTT_PORT" (e.g., "192.168.1.10:1884").
        On success, updates HOST_MAP["server"] with the advertised IP and returns (ip, port).
        Returns None on failure/timeout.
        """
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Allow reuse so multiple listeners/tests can bind and to avoid TIME_WAIT issues
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except OSError:
            pass
        # Some platforms support SO_REUSEPORT for multiple receivers
        try:
            sock.setsockopt(socket.SOL_SOCKET, getattr(socket, "SO_REUSEPORT", 15), 1)
        except (AttributeError, OSError):
            pass
        # Bind to all interfaces on the given port
        sock.bind(("", ADVERTISE_PORT))
        while True:
            try:    # Receive a message
                data, addr = sock.recvfrom(1024)
                message_str = data.decode("utf-8", errors="replace").strip()
                print(f"Received advertisement: {message_str} from {addr[0]}")
                if message_str == "Hello from MQTT Broker": # Map the sender's IP to "server"
                    HOST_MAP["server"] = addr[0]
                else:
                    print(f"Unexpected advertisement format: {message_str}, retrying...")
                    return 1
                return 0
            except Exception as e:
                print(f"UDP listen error: {e}")
                return 1
            finally:
                try:
                    sock.close()
                except Exception:
                    pass

def sign(private_key: ec.EllipticCurvePrivateKey, message: bytes) -> bytes:
    """Signs a message using the provided ECDSA private key and returns the signature."""
    signature = private_key.sign(message, ec.ECDSA(hashes.SHA256()))
    return signature