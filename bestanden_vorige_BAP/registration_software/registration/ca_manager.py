# core/crypto/ca_manager.py
from __future__ import annotations

# Import required packages
from cryptography import x509
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
import datetime
import os

# Import configuration and helper functions
from registration_software.common.config import settings
from registration_software.common.crypto.primitives import generate_ecdsa_private_key

class CAManager:
    """
    Manages the Root Certificate Authority (CA). 
    Responsible for CA generation, loading, and signing client certificates.
    """
    def __init__(self, ca_cert_path: str = settings.paths.ca_cert, ca_key_path: str = settings.paths.ca_key):
        self.ca_cert_path = ca_cert_path
        self.ca_key_path = ca_key_path
        self.ca_cert = None
        self.ca_key = None
        
        self.load_or_generate_ca()
        self.load_or_generate_identity(cert_path=settings.paths.server_cert, key_path=settings.paths.server_key, common_name="server", purpose="server")

    def load_or_generate_ca(self):
        """Tries to load CA files; if missing, generates and saves a new CA."""
        try:
            with open(self.ca_cert_path, 'rb') as f:
                self.ca_cert = x509.load_pem_x509_certificate(f.read())
            with open(self.ca_key_path, 'rb') as f:
                self.ca_key = serialization.load_pem_private_key(f.read(), password=None)
            print(f"Loaded existing CA certificate: {self.ca_cert_path}")
        except FileNotFoundError:
            print("CA files missing. Generating new CA...")
            ca_key = generate_ecdsa_private_key()   # Use the helper function to generate ECDSA key
            ca_cert = self.generate_ca_certificate(ca_key)
            self.write_ca_files(ca_cert, ca_key)
            self.ca_cert = ca_cert
            self.ca_key = ca_key
        
    def generate_ca_certificate(self, ca_key: ec.EllipticCurvePrivateKey) -> x509.Certificate:
        """Creates a new self-signed CA certificate."""
        subject = issuer = x509.Name([
            x509.NameAttribute(NameOID.COUNTRY_NAME, u"NL"),
            x509.NameAttribute(NameOID.STATE_OR_PROVINCE_NAME, u"Zuid Holland"),
            x509.NameAttribute(NameOID.LOCALITY_NAME, u"Delft"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, u"TU Delft"),
            x509.NameAttribute(NameOID.COMMON_NAME, u"BAP_CA"),
        ])

        # Get UTC time zone for the certificates and then build the CA certificate
        now = datetime.datetime.now(datetime.timezone.utc)
        cert = x509.CertificateBuilder().subject_name(
            subject
        ).issuer_name(
            issuer
        ).public_key(
            ca_key.public_key()
        ).serial_number(
            x509.random_serial_number()
        ).not_valid_before(
            now - datetime.timedelta(days=1) # Extensive validaty for testing purposes
        ).not_valid_after(
            now + datetime.timedelta(days=365)  # 1 year
        ).add_extension(
            x509.BasicConstraints(ca=True, path_length=None),
            critical=True,
        ).add_extension(
            x509.KeyUsage(
                digital_signature=True,
                key_encipherment=False,
                content_commitment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        ).sign(ca_key, hashes.SHA256())

        return cert


    def write_ca_files(self, ca_cert: x509.Certificate, ca_key: ec.EllipticCurvePrivateKey):
        """Writes the CA key and certificate to disk."""
        os.makedirs(os.path.dirname(self.ca_cert_path), exist_ok=True)
        os.makedirs(os.path.dirname(self.ca_key_path), exist_ok=True)
        
        with open(self.ca_cert_path, 'wb') as f:
            f.write(ca_cert.public_bytes(serialization.Encoding.PEM))
        with open(self.ca_key_path, 'wb') as f:
            f.write(ca_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.PKCS8,
                encryption_algorithm=serialization.NoEncryption()
            ))

    def sign_certificate(self, client_public_key, common_name: str, validity_days: int = 365, purpose="client") -> str:
        """Signs a certificate with the CA's private key."""
        if not self.ca_key:
            raise RuntimeError("CA key not loaded. Cannot sign certificate.")

        subject = x509.Name([
            x509.NameAttribute(NameOID.COMMON_NAME, common_name),
        ])
        now = datetime.datetime.now(datetime.timezone.utc)
        
        builder = (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(self.ca_cert.subject)
            .public_key(client_public_key)
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=validity_days))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(common_name)]), critical=False)
        )

        # Add KeyUsage and ExtendedKeyUsage
        if purpose == "client":
            builder = builder.add_extension(
                x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH]),
                critical=False,
            )
        elif purpose == "server":
            builder = builder.add_extension(
                x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
                critical=False,
            )

        cert = builder.sign(private_key=self.ca_key, algorithm=hashes.SHA256())
        return cert.public_bytes(serialization.Encoding.PEM).decode()

    def load_or_generate_identity(self, cert_path: str, key_path: str, common_name: str, purpose: str):
        """ Ensures the certificate and key exist and generate them if they do not. """
        
        if os.path.exists(cert_path) and os.path.exists(key_path):
            print(f"Loaded existing identity certificate: {cert_path}")
            return # Identity files already exist
        
        identity_key = generate_ecdsa_private_key()
        cert_pem = self.sign_certificate(
            identity_key.public_key(), 
            common_name=common_name,
            purpose=purpose
        )

        os.makedirs(os.path.dirname(cert_path), exist_ok=True)
        with open(cert_path, 'wb') as f:
            f.write(cert_pem.encode())
            
        with open(key_path, 'wb') as f:
            f.write(identity_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.PKCS8,
                encryption_algorithm=serialization.NoEncryption()
            ))
            
        print(f"Generated and stored new identity at {cert_path}")