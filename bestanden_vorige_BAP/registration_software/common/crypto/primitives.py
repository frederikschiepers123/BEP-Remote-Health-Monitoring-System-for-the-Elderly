# src/common/crypto/primitives.py
from __future__ import annotations
import os
from typing import Union
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, ed25519

# ed25519 key generation
def generate_ed25519_public_key(private_key: ed25519.Ed25519PrivateKey) -> ed25519.Ed25519PublicKey:
    """Generates the corresponding ed25519 public key from a given private key."""
    return private_key.public_key()

def generate_ed25519_private_key() -> ed25519.Ed25519PrivateKey:
    """Generates a new ed25519 private key."""
    return ed25519.Ed25519PrivateKey.generate()

def generate_ed25519_key_pair() -> tuple[ed25519.Ed25519PrivateKey, ed25519.Ed25519PublicKey]:
    """Generates a new ed25519 key pair."""
    private_key = generate_ed25519_private_key()
    public_key = generate_ed25519_public_key(private_key)
    return private_key, public_key

# ECDSA key generation
def generate_ecdsa_public_key(private_key: ec.EllipticCurvePrivateKey) -> ec.EllipticCurvePublicKey:
    """Generates the corresponding ECDSA public key from a given private key."""
    return private_key.public_key()

def generate_ecdsa_private_key() -> ec.EllipticCurvePrivateKey:
    """Generates a new ECDSA private key using the SECP256R1 curve."""
    return ec.generate_private_key(ec.SECP256R1())

def generate_ecdsa_key_pair() -> tuple[ec.EllipticCurvePrivateKey, ec.EllipticCurvePublicKey]:
    """Generates a new ECDSA key pair using the SECP256R1 curve."""
    private_key = generate_ecdsa_private_key()
    public_key = generate_ecdsa_public_key(private_key)
    return private_key, public_key


def generate_nonce(length: int = 16) -> str:
    """Generate a random nonce of specified length as a hex string."""
    return os.urandom(length).hex()

def verify_signature(public_key: Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey], nonce_bytes: bytes, signature_bytes: bytes) -> bool:
    """Verifies an elliptic curve signature against a nonce using a public key."""
    try:
        if isinstance(public_key, ed25519.Ed25519PublicKey):
            public_key.verify(signature_bytes, nonce_bytes)
        elif isinstance(public_key, ec.EllipticCurvePublicKey):
            public_key.verify(signature_bytes, nonce_bytes, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False
    except Exception as e:
        print(f"[Crypto] Error during signature verification: {e}")
        return False