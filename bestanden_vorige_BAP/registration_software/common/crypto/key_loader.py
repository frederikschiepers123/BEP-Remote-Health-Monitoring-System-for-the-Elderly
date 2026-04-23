# src/common/crypto/key_loader.py
from __future__ import annotations
from typing import Union
from cryptography.hazmat.primitives.asymmetric import ec, ed25519
from cryptography.hazmat.primitives import serialization

def public_key_to_pem(public_key: Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey]) -> str:
    """Converts a public key object to a PEM string."""
    pem_bytes = public_key.public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo
    )
    return pem_bytes.decode('utf-8')

def private_key_to_pem(private_key: Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey]) -> str:
    """Converts a private key object to a PEM string."""
    pem_bytes = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()
    )
    return pem_bytes.decode('utf-8')

def key_pair_to_pem(private_key: Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey],
                    public_key: Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey]) -> tuple[str, str]:
    """Converts a key pair to PEM formatted strings."""
    private_pem = private_key_to_pem(private_key)
    public_pem = public_key_to_pem(public_key)
    return private_pem, public_pem

def pem_to_public_key(pem_bytes: bytes) -> Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey]:
    """Convertes PEM formatted bytes to a public key object."""
    try:
        return serialization.load_pem_public_key(pem_bytes)
    except Exception as e:
        raise ValueError(f"Failed to load public key from PEM: {e}")
    
def pem_to_private_key(pem_bytes: bytes) -> Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey]:
    """Converts PEM formatted bytes to a private key object."""
    try:
        return serialization.load_pem_private_key(pem_bytes, password=None)
    except Exception as e:
        raise ValueError(f"Failed to load private key from PEM: {e}")
    
def pem_to_key_pair(private_pem_bytes: bytes, public_pem_bytes: bytes) -> tuple[Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey], Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey]]:
    """Converts PEM formatted bytes to a key pair."""
    private_key = pem_to_private_key(private_pem_bytes)
    public_key = pem_to_public_key(public_pem_bytes)
    return private_key, public_key

def load_public_key_from_file(filename: str) -> Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey]:
    """Loads a public key from a PEM file."""
    try:
        with open(filename, "rb") as f:
            pem_bytes = f.read()
        return serialization.load_pem_public_key(pem_bytes)
    except Exception as e:
        raise ValueError(f"Failed to load public key from file {filename}: {e}")
    
def load_private_key_from_file(filename: str) -> Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey]:
    """Loads a private key from a PEM file."""
    try:
        with open(filename, "rb") as f:
            pem_bytes = f.read()
        return serialization.load_pem_private_key(pem_bytes, password=None)
    except Exception as e:
        raise ValueError(f"Failed to load private key from file {filename}: {e}")
    
def load_key_pair_from_files(private_filename: str, public_filename: str) -> tuple[Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey], Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey]]:
    """Loads a key pair from PEM files."""
    private_key = load_private_key_from_file(private_filename)
    public_key = load_public_key_from_file(public_filename)
    return private_key, public_key

def save_public_key(key_obj: Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey], filename: str) -> None:
    """Saves a public key object to a PEM file."""
    pem_bytes = key_obj.public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo
    )
    with open(filename, "wb") as f:
        f.write(pem_bytes)

def save_private_key(key_obj: Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey], filename: str) -> None:
    """Saves a private key object to a PEM file."""
    pem_bytes = key_obj.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()
    )
    with open(filename, "wb") as f:
        f.write(pem_bytes)

def save_key_pair(private_key_obj: Union[ed25519.Ed25519PrivateKey, ec.EllipticCurvePrivateKey],
                  public_key_obj: Union[ed25519.Ed25519PublicKey, ec.EllipticCurvePublicKey],
                  private_filename: str,
                  public_filename: str) -> None:
    """Saves a key pair to PEM files."""
    save_private_key(private_key_obj, private_filename)
    save_public_key(public_key_obj, public_filename)