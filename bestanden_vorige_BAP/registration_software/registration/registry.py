# src/registration/core/data_registry.py
from __future__ import annotations
import threading
from registration_software.common import load_json, save_json
from registration_software.common.config import settings

class DeviceRegistry:
    """
    Manages the persistent registry of enrolled devices and their credentials.
    Separates DataFrame and file I/O logic from the MQTT phases.
    """
    def __init__(self, registry_path: str):
        self.registry_path = registry_path
        self.current_data = load_json(registry_path)

        # threading
        self._lock = threading.Lock()

    def add_registered_device(self, mac_address: str, uuid: str, device_type: str, label: str) -> bool:
        """Adds a new registered device to the registry."""
        with self._lock:
            if label not in self.current_data:
                new_data = {
                        "mac_address": mac_address,
                        "uuid": uuid,
                        "device_type": device_type,
                        "label": label,
                        "registered_flag": "True",
                        "device_check_flag": "False"
                    }
                self.current_data[label] = new_data
                return True
            elif self.current_data[label]["device_type"] != device_type:
                new_data = {
                        "mac_address": mac_address,
                        "uuid": uuid,
                        "device_type": device_type,
                        "label": label,
                        "registered_flag": "True",
                        "device_check_flag": "False"
                    }
                self.current_data[label] = new_data
                return True
            return False
        
    def persist_to_json(self):
        """Persists the collected credentials to a JSON registry file."""
        with self._lock:
            save_json(self.registry_path, self.current_data)

    def get_labels(self) -> list:
        """Returns the list of currently registered labels."""
        self.current_data = load_json(self.registry_path)
        with self._lock:
            return [key for key in self.current_data]

    def is_complete(self, expected_labels: list) -> bool:
        """Checks if all expected labels have been registered."""
        self.current_data = load_json(self.registry_path)
        with self._lock:
            return len(self.current_data) >= len(expected_labels)