# src/labels/label_generator.py
from __future__ import annotations
import string
from typing import List, Union
import base64
import os
import random
from PIL import Image, ImageDraw, ImageFont
import qrcode
from cryptography.hazmat.primitives.asymmetric import ed25519

from registration_software.common.config import settings
from registration_software.common.crypto import load_private_key_from_file
from registration_software.common.utils import load_json, save_json


class LabelGenerator:
    def __init__(self):
        self.label_registry_path = settings.paths.label_registry
        self.private_key_path = settings.paths.qr_private_key
        self.label_length = settings.qr.qr_version
        self.img_dir = settings.paths.qr_img_dir
        self.label_list = self.load_existing_labels()
        self.private_key = load_private_key_from_file(self.private_key_path)

    def load_existing_labels(self) -> List[dict]:
        """Loads existing labels from the registry JSON file."""
        return load_json(self.label_registry_path)

    def generate(self, amount: int = 1, position: int =  1) -> List[str]:
        """Generates new unique labels, creates QR codes and combined images,
        saves them as PNG files, and updates the label registry JSON file."""
        self.label_list = self.load_existing_labels()
        new_labels = self._unique_strings_generator(amount)
        self.position = position
        
        if not new_labels:
            print("No new labels were generated. Exiting.")
            return
        
        for label in new_labels:
            signed_label = self.sign_label_bytes(label)

            qr_img = self._create_qr_image(signed_label)
            combined_img = self._create_combined_image(label, qr_img)

            png_filename = os.path.join(self.img_dir, f"{label}.png")
            combined_img.save(png_filename)

            # Read the saved PNG, Base64 encode it, and store in list for JSON serialization
            with open(png_filename, "rb") as png_file:
                b64_data = base64.b64encode(png_file.read()).decode("utf-8")
            
            data = {"label": label, "data": b64_data, "filename": png_filename}
            self.label_list.append(data)
            
        # Store the final list as a JSON file
        save_json(self.label_registry_path, self.label_list)
        
        print(f"[LabelGenerator] Successfully created {len(new_labels)} new QR codes.")
        return new_labels

    def _unique_strings_generator(self, amount: int = 1) -> List[str]:
        """Generates N new, unique random strings (labels)."""
        characters = string.ascii_uppercase + string.digits 
        seen = set(entry["label"] for entry in self.label_list)
        labels_to_generate = amount

        unique_labels = []
        while labels_to_generate > 0:
            rand_str = ''.join(random.choices(characters, k=self.label_length))
            if rand_str not in seen:
                seen.add(rand_str)
                labels_to_generate -= 1
                unique_labels.append(rand_str)

        return unique_labels

    def sign_label_bytes(self, label: str) -> bytes:
        if not self.private_key:
             raise RuntimeError("Private key must be loaded before signing.")
             
        enc_label = label.encode("utf-8")
        
        # Check if the key object has the correct signing method
        if isinstance(self.private_key, ed25519.Ed25519PrivateKey):
            signature = self.private_key.sign(enc_label)
        else:
             raise TypeError(f"Unsupported private key type: {type(self.private_key)}")

        return bytes([len(enc_label)]) + enc_label + signature

    # --- QR Code and Image Processing ---

    def _create_qr_image(self, qr_data: Union[str, bytes]) -> Image.Image:
        """Generates the monochrome QR code image."""
        qr = qrcode.QRCode(
            version=None,
            error_correction=qrcode.constants.ERROR_CORRECT_L,
            box_size=10,
            border=4,
        )
        qr.add_data(qr_data)
        qr.make(fit=True)
        return qr.make_image(fill_color="black", back_color="white").convert("1")

    def _create_combined_image(self, label: str, qr_img: Image.Image) -> Image.Image:
        """Creates the text label image and combines it vertically with the QR code."""
        qr_width, qr_height = qr_img.size

        if self.position == 1:
            label_height = int(qr_height * 0.30)
            label_img = Image.new("1", (qr_width, label_height), 1)
            draw = ImageDraw.Draw(label_img)

            try:
                font = ImageFont.truetype("arial.ttf", 60)
            except IOError:
                font = ImageFont.load_default()

            # Dynamic Font Sizing
            final_font = font
            font_size = 10
            max_width = qr_width * 0.6

            while True:
                try:
                    temp_font = final_font.font_variant(size=font_size) if hasattr(final_font, 'font_variant') else ImageFont.load_default()
                    bbox = draw.textbbox((0, 0), label, font=temp_font)
                    text_width = bbox[2] - bbox[0]
                    text_height = bbox[3] - bbox[1]

                    if text_width < max_width and text_height < label_height * 0.8:
                        final_font = temp_font
                        font_size += 5
                    else:
                        break
                except Exception:
                    break

            # Center and Draw Text
            bbox = draw.textbbox((0, 0), label, font=final_font)
            text_width = bbox[2] - bbox[0]
            text_height = bbox[3] - bbox[1]
            text_x = (qr_width - text_width) // 2
            text_y = (label_height - text_height) // 2

            draw.text((text_x, text_y), label, fill=0, font=final_font)

            # Combine Images
            total_height = qr_height + label_height
            combined_img = Image.new("1", (qr_width, total_height), 1)
            combined_img.paste(label_img, (0, 0))
            combined_img.paste(qr_img, (0, label_height))

        elif self.position == 2:
            label_width = int(qr_width * 0.60)
            label_img = Image.new("1", (label_width, qr_height), 1)
            draw = ImageDraw.Draw(label_img)

            try:
                font = ImageFont.truetype("arial.ttf", 60)
            except IOError:
                font = ImageFont.load_default()

            # Dynamic Font Sizing
            final_font = font
            font_size = 10
            max_width = label_width * 0.9
            max_height = qr_height * 0.8

            while True:
                try:
                    temp_font = final_font.font_variant(size=font_size) if hasattr(final_font, 'font_variant') else ImageFont.load_default()
                    bbox = draw.textbbox((0, 0), label, font=temp_font)
                    text_width = bbox[2] - bbox[0]
                    text_height = bbox[3] - bbox[1]

                    if text_width < max_width and text_height < max_height:
                        final_font = temp_font
                        font_size += 5
                    else:
                        break
                except Exception:
                    break

            # Center and Draw Text
            bbox = draw.textbbox((0, 0), label, font=final_font)
            text_width = bbox[2] - bbox[0]
            text_height = bbox[3] - bbox[1]
            text_x = (label_width - text_width) // 2
            text_y = (qr_height - text_height) // 2

            draw.text((text_x, text_y), label, fill=0, font=final_font)

            # Combine Images
            total_width = qr_width + label_width
            combined_img = Image.new("1", (total_width, qr_height), 1)
            combined_img.paste(label_img, (qr_width, 0))
            combined_img.paste(qr_img, (0, 0))

        elif self.position == 3:
            label_width = int(qr_width * 0.60)
            label_img = Image.new("1", (label_width, qr_height), 1)
            draw = ImageDraw.Draw(label_img)

            try:
                font = ImageFont.truetype("arial.ttf", 60)
            except IOError:
                font = ImageFont.load_default()

            # Dynamic Font Sizing
            final_font = font
            font_size = 10
            max_width = label_width * 0.9
            max_height = qr_height * 0.8

            while True:
                try:
                    temp_font = final_font.font_variant(size=font_size) if hasattr(final_font, 'font_variant') else ImageFont.load_default()
                    bbox = draw.textbbox((0, 0), label, font=temp_font)
                    text_width = bbox[2] - bbox[0]
                    text_height = bbox[3] - bbox[1]

                    if text_width < max_width and text_height < max_height:
                        final_font = temp_font
                        font_size += 5
                    else:
                        break
                except Exception:
                    break

            # Center and Draw Text
            bbox = draw.textbbox((0, 0), label, font=final_font)
            text_width = bbox[2] - bbox[0]
            text_height = bbox[3] - bbox[1]
            text_x = (label_width - text_width) // 2
            text_y = (qr_height - text_height) // 2

            draw.text((text_x, text_y), label, fill=0, font=final_font)

            # Combine Images
            total_width = qr_width + label_width
            combined_img = Image.new("1", (total_width, qr_height), 1)
            combined_img.paste(label_img, (0, 0))
            combined_img.paste(qr_img, (label_width, 0))


        else:
            raise ValueError(f"Unsupported label_position: {self.position}")
        
        return combined_img

# gen = LabelGenerator()
# gen.generate(amount=1, position= 2)