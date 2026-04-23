# src/identification/processing/led_detector.py
from __future__ import annotations
from typing import Tuple, Optional
import cv2
import numpy as np
from registration_software.models.device import DeviceState
from registration_software.models.enums import RegistrationStatus
from registration_software.common.config import settings

# Map LED color names to the corresponding BGR channel
_CHANNEL_MAP = { "blue": 0, "green": 1, "red": 2,}

class LEDDetector:
    """
    Core class for executing the LED identification algorithm.
    The main function is `process_frame`, which updates the device state.

    Attributes:
        blur_kernel: Kernel size for Gaussian blur.
        ref_signal_cos: Reference cosine signal for frequency detection.
        ref_signal_sin: Reference sine signal for frequency detection.
    """
    def __init__(self) -> None:
        self.blur_kernel = (5, 5)
        
        # Frequency detector
        t = np.arange(settings.active_id.min_signal_size) / settings.camera.sample_rate
        self.ref_signal_cos = np.cos(2 * np.pi * settings.active_id.target_frequency * t)
        self.ref_signal_sin = np.sin(2 * np.pi * settings.active_id.target_frequency * t)

    def process_frame(self, device: DeviceState, full_frame: np.ndarray) -> bool:
        """
        Updates the device state based on the current frame. Returns `True` if a blinking LED signal was verified.
        The processing steps as described in the documentation given in the code using comments.
        """
        # ROI extraction
        padded_bbox = self._get_padded_bbox(device.bbox)
        roi = self._get_roi(full_frame, padded_bbox)
        if roi.size == 0:
            device.handle_miss()
            return False

        # Channel selection and normalization
        gray = self._preprocess_roi(roi)

        # Background model update and difference calculation
        binary_mask, difference = self._update_background_model(device, gray)

        # Signal extraction
        led_centroid = self._find_centroid(binary_mask, difference)
        if led_centroid:
            device.led_centroid = led_centroid
            device.missed_frames = 0

            cx, cy = led_centroid
            intensity = self._get_patch_mean(gray, cx, cy)
            device.signal_buffer.append(intensity)
        elif device.led_centroid is not None:
            cx, cy = device.led_centroid
            intensity = self._get_patch_mean(gray, cx, cy)
            device.signal_buffer.append(intensity)
            device.handle_miss()
        else:
            intensity = 0.0
            device.handle_miss()

        # Frequency analysis
        return self._verify_frequency(device)
    
    def _get_padded_bbox(self, bbox: Tuple[int, int, int, int]) -> Tuple[int, int, int, int]:
        """Apply padding to the QR code bounding box."""
        x, y, w, h = bbox
        pad_w = int(w * (settings.active_id.bbox_padding_percent / 100.0))
        pad_h = int(h * (settings.active_id.bbox_padding_percent / 100.0))
        return (max(0, x - pad_w), max(0, y - pad_h), w + 2 * pad_w, h + 2 * pad_h)
    
    def _get_roi(self, frame: np.ndarray, bbox: Tuple[int, int, int, int]) -> np.ndarray:
        """Extracts the ROI from the frame based on the bounding box."""
        x, y, w, h = bbox
        fh, fw = frame.shape[:2]
        
        # Clamp to frame boundaries
        x, y = max(0, x), max(0, y)
        w = min(w, fw - x)
        h = min(h, fh - y)
        
        if w <= 0 or h <= 0:
            return np.array([])
        return frame[y:y+h, x:x+w]
    
    def _preprocess_roi(self, roi: np.ndarray) -> np.ndarray:
        """Applies channel selection and normalization to the ROI."""
        color_idx = _CHANNEL_MAP.get(settings.active_id.led_color, None)
        if color_idx is not None:
            gray = roi[:, :, color_idx]
        else:
            gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)

        gray = cv2.normalize(gray, None, 0, 255, cv2.NORM_MINMAX)
        return gray
    
    def _update_background_model(self, device: DeviceState, gray: np.ndarray) -> np.ndarray:
        """Updates the background model and returns the difference mask."""

        # Initialize background model if not present
        if device.background_model is None:
             device.background_model = gray.astype(np.float32)
             return np.zeros_like(gray), (0, 0)
        
        # Handle size jitter from device movement
        if gray.shape != device.background_model.shape:
            device.background_model = cv2.resize(device.background_model, (gray.shape[1], gray.shape[0]))
        cv2.accumulateWeighted(gray, device.background_model, settings.active_id.background_learning_rate)
        
        # Background subtraction
        difference = cv2.absdiff(gray, device.background_model.astype(np.uint8))
        difference = cv2.GaussianBlur(difference, self.blur_kernel, 0)

        # Adaptive thresholding for binary mask
        mean_val, std_val = cv2.meanStdDev(difference)
        thresh_val = mean_val[0][0] + (settings.active_id.noise_sigma_factor * std_val[0][0])
        _, binary_mask = cv2.threshold(difference, max(settings.active_id.noise_floor, thresh_val), 255, cv2.THRESH_BINARY)
        return binary_mask, difference
    
    def _find_centroid(self, binary_mask: np.ndarray, difference: np.ndarray) -> Optional[Tuple[float, float]]:
        """Finds the centroid using the binary mask and difference image."""

        # Find contours from binary mask
        contours, _ = cv2.findContours(binary_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if not contours:
            return None
        
        largest_contour = max(contours, key=cv2.contourArea)
        if cv2.contourArea(largest_contour) < 2.0:
            return None

        # Isolate largest contour
        blob_mask = np.zeros_like(binary_mask)
        cv2.drawContours(blob_mask, [largest_contour], -1, 255, -1)
        masked_intensity = cv2.bitwise_and(difference, difference, mask=blob_mask)
        
        M = cv2.moments(masked_intensity, binaryImage=False)
        if M["m00"] != 0:
            return int(M["m10"] / M["m00"]), int(M["m01"] / M["m00"])

        x, y, w, h = cv2.boundingRect(largest_contour)
        return int(x + w // 2), int(y + h // 2)
    
    def _get_patch_mean(self, frame: np.ndarray, x: int, y: int) -> float:
        """Calculates average value of a square patch around x, y"""
        h, w = frame.shape
        r = settings.active_id.patch_size // 2
        x1, x2 = max(0, x - r), min(w, x + r + 1)
        y1, y2 = max(0, y - r), min(h, y + r + 1)
        patch = frame[y1:y2, x1:x2]
        if patch.size > 0: 
            return float(np.mean(patch))
        return 0.0

    def _verify_frequency(self, device: DeviceState) -> bool:
        """Performs frequency analysis to verify the LED blinking signal."""
        if device.status == RegistrationStatus.IDENTIFIED:
            return True
        if len(device.signal_buffer) < settings.active_id.min_signal_size:
            return False

        signal = np.array(device.signal_buffer)
        magnitude = self._calculate_magnitude(signal)
        
        if magnitude > settings.active_id.magnitude_threshold:
            device.status = RegistrationStatus.IDENTIFIED
            return True
        return False
    
    def _calculate_magnitude(self, signal: np.ndarray) -> float:
        """Single-bin DFT magnitude calculation."""
        n = len(signal)
        if n < 2: 
            return 0.0
        
        if n > len(self.ref_signal_cos):
             n = len(self.ref_signal_cos)
             signal = signal[-n:]

        cos = self.ref_signal_cos[:n]
        sin = self.ref_signal_sin[:n]

        signal_std = np.std(signal)
        if signal_std < 1e-6:
            return 0.0
        norm_signal = (signal - np.mean(signal)) / signal_std

        real = np.dot(norm_signal, cos)
        imag = np.dot(norm_signal, sin)
        return np.sqrt(real**2 + imag**2) / n