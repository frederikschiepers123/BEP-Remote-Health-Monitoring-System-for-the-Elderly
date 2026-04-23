# src/tools/calibrate.py
from __future__ import annotations

import sys
import os
import cv2
import json
import time
import numpy as np
from pathlib import Path
from typing import Dict

# Add project root to path to allow imports
sys.path.append(str(Path(__file__).resolve().parents[2]))

from registration_software.interface.streamers import create_streamer
from registration_software.identification.processing.qr_detector import QrCodeDetector
from registration_software.common.config import settings


def calibrate_camera(focal_length: float, aperture: float,
                     sensor_width: float, sensor_height: float,
                     image_width: int, image_height: int,
                     qr_version: int, qr_size_mm: float,
                     ppm_threshold: float,
                     observed_minimum: float, qr_width: int) -> tuple[float, float]:
    """
    Calibrates camera parameters based on observed minimum working distance and QR code size.

    Args:
        name (str): Name of the camera model.
        focal_length (float): Focal length in millimeters.
        sensor_width (float): Sensor width in millimeters.
        sensor_height (float): Sensor height in millimeters.
        image_width (int): Image width in pixels.
        image_height (int): Image height in pixels.
        observed_minimum (float): Observed minimum working distance in millimeters.
        qr_width (int): Width of the QR code in pixels at observed minimum distance.

    Returns:
        Tuple[float, float]: Calculated far limit in millimeters and distance constant k.
    """
    sensor_diagonal = (sensor_width**2 + sensor_height**2) ** 0.5
    image_diagonal = (image_width**2 + image_height**2) ** 0.5
    pixel_pitch = sensor_diagonal / image_diagonal

    module_count = 21 + (qr_version - 1) * 4
    far_limit_mm = (focal_length * qr_size_mm) / (ppm_threshold * module_count * pixel_pitch)
    distance_constant_k = observed_minimum * qr_width

    return far_limit_mm, distance_constant_k

def run_camera_calibration_tool():
    """
    Runs an interactive camera calibration tool using QR codes.
    it is used to determine camera parameters:
        - near and far limits
        - The distance constant k.
    All other camera parameters should be pre-configured in the settings file.
    """
    print(f"\n-------------------------------\n"
          f"--- Camera Calibration Tool ---"
          f"\n-------------------------------\n")
    
    # Initialize streamer and QR detector
    streamer = create_streamer(
        source=settings.camera.ip,
        user=settings.camera.username,
        password=settings.camera.password,
        port=settings.camera.http_port
    )
    detector = QrCodeDetector()
    
    streamer.start()
    detector.start()

    print("\nInstructions:")
    print("1. Place the QR code at the closest distance where it is reliably detected.")
    print("2. Verify that the green bounding box appears around the QR code.")
    print("3. Press 'C' if the device is in the correct position and you want to calibrate.")
    print("4. Press 'Q' to quit.\n")

    calibrated_data = None
    try:
        while True:
            frame = streamer.read()
            if frame is None:
                time.sleep(0.01)
                continue

            # Detection
            display_frame = frame.copy()
            results = detector.detect_single_frame(frame)
            
            # Visualization
            for r in results:
                x, y, w, h = r["bbox"]
                color = (0, 255, 0) if r["authenticity"] else (0, 0, 255)
                cv2.rectangle(display_frame, (x, y), (x + w, y + h), color, 2)
                cv2.putText(display_frame, f"{r['label']}", (x, y-10), 
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)

            cv2.imshow("Calibration View", cv2.resize(display_frame, (1280, 720)))
            
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                break
            elif key == ord('c'):
                if not results:
                    print("No QR code detected! Cannot calibrate.")
                    continue
                
                valid_qr = next((r for r in results if r['authenticity']), None)
                if not valid_qr:
                    print("No authentic QR code detected! Cannot calibrate.")
                    continue

                print("\n[!] Frame Captured.")
                width, height, fps = streamer.get_stream_properties()
                streamer.stop()
                cv2.destroyAllWindows()
                
                # Get width of the QR in pixels from the bounding box
                _, _, w_px, _ = valid_qr['bbox']

                measured_dist = None
                while measured_dist is None:
                    user_input = input("Enter the measured distance from lens to QR code in millimeters: ")
                    try:
                        # Attempt to convert the string input to a float
                        measured_dist = float(user_input)
                    except ValueError:
                        print("Invalid input. Please enter a number (e.g., 100 or 100.5).")
                
#                try:
#                    measured_dist = float(input(f"Enter the measured distance from lens to QR code in millimeters: "))
#                except ValueError:
#                    print("Invalid input. Exiting.")
#                    break

                # Perform calibration
                calibrated_data = calibrate_camera(
                    focal_length=settings.camera.focal_length_mm,
                    aperture=settings.camera.aperture,
                    sensor_width=settings.camera.sensor_width_mm,
                    sensor_height=settings.camera.sensor_height_mm,
                    image_width=width,
                    image_height=height,
                    observed_minimum=measured_dist,
                    qr_width=w_px,
                    qr_version=settings.qr.qr_version,
                    qr_size_mm=settings.qr.qr_size_mm,
                    ppm_threshold=settings.qr.ppm_threshold
                )

                far_limit, distance_constant = calibrated_data
                calibrated_data = {
                    "observed qr width (px)": w_px,
                    "near limit (mm)": measured_dist,
                    "far limit (mm)": far_limit,
                    "distance constant": distance_constant,
                    "focal length (mm)": settings.camera.focal_length_mm,
                    "sensor width (mm)": settings.camera.sensor_width_mm,
                    "sensor height (mm)": settings.camera.sensor_height_mm,
                    "image width (px)": width,
                    "image height (px)": height,
                    "fps": fps
                }
                
                print("\n--- Calibration Results ---")
                print(json.dumps(calibrated_data, indent=4))
                break

    except KeyboardInterrupt:
        pass
    finally:
        streamer.stop()
        detector.stop() # Stop the process
        cv2.destroyAllWindows()