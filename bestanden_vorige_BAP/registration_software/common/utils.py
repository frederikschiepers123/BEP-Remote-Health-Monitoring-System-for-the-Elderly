# src/common/utils.py
from __future__ import annotations
import json
from typing import Dict

def load_json(file_path: str) -> Dict:
    """Load and return JSON data from a file."""
    try:
        with open(file_path, "r") as f:
            return json.load(f)
    except FileNotFoundError:
        print(f"File not found: {file_path}")
        return {}
    except Exception as e:
        print(f"Error reading JSON from {file_path}: {e}")
        return {}
    
def save_json(file_path: str, data: Dict) -> None:
    """Save data to a JSON file."""
    try:
        with open(file_path, 'w') as file:
            json.dump(data, file, indent=4)
    except Exception as e:
        print(f"Error writing JSON to {file_path}: {e}")