# src/labels/__init__.py
import os
import json
import base64
from .label_generator import LabelGenerator


def recreate_png_from_json(file_path: str, qr_img_dir: str):
    """
    Function to turn .json file back into .png files (QR codes).
    """
    try:
        with open(file_path, "r") as f:
            json_data = json.load(f)
    except Exception as e:
        print(f"Error loading JSON file {file_path}: {e}")
        return

    for entry in json_data:
        label = entry.get("label", "unknown")
        img_data_b64 = entry.get("data")
        
        if not img_data_b64:
            print(f"Warning: Skipping entry for label '{label}' - missing data.")
            continue

        try:
            img_data = base64.b64decode(img_data_b64) 
            
            output_filename = os.path.join(qr_img_dir, f"{label}.png")
            with open(output_filename, "wb") as f:
                f.write(img_data)
        except Exception as e:
            print(f"Error processing label '{label}': {e}")