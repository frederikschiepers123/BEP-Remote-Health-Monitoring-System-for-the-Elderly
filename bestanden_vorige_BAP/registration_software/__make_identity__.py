# src/__run_ca__.py
from __future__ import annotations

import os
import sys

from registration_software.registration.ca_manager import CAManager
from registration_software.common.config import settings

def clear_cached_files(force: bool) -> None:
    """Removes cached certificate and key files to force regeneration. If force is True, also removes the CA files."""
    if force:
        paths = [
            settings.paths.ca_cert, settings.paths.ca_key,
            settings.paths.server_cert, settings.paths.server_key,
            settings.paths.client_cert, settings.paths.client_key,
            settings.paths.registry
        ]
    else:
        paths = [
            settings.paths.server_cert, settings.paths.server_key,
            settings.paths.client_cert, settings.paths.client_key,
            settings.paths.registry
        ]
    for path in paths:
        try:
            os.remove(path)
            print(f"Removed cached file: {path}")
        except FileNotFoundError:
            pass

if __name__ == "__main__":
    # Force regeneration of CA and server certificates
    if sys.argv[1].lower() == "true":
        clear_cached_files(True)
    else:
        clear_cached_files(False)
    ca_manager = CAManager()
    
    