# src/models/device.py
from __future__ import annotations
import time
import numpy as np
from collections import deque
from dataclasses import dataclass, field
from typing import Tuple, Deque, Optional
from registration_software.models.enums import RegistrationStatus
from registration_software.common.config import settings

@dataclass
class DeviceState:
    """
    Represents a physical device being tracked in the camera FOV.
    Attributes:
        label: Human-readable label for the device.
        bbox: Bounding box (x, y, width, height) of the devices QR code in the frame.
        status: Registration status of the device.
        distance: Estimated distance from the camera to the device.
        last_seen: Timestamp of the last time the device was seen.
        missed_frames: Count of consecutive frames where the device was not detected.
        background_model: Background model for LED signal extraction.
        led_centroid: Current centroid of the detected LED signal.
        signal_buffer: Buffer storing recent signal values for processing.
    """
    label: str
    bbox: Tuple[int, int, int, int]
    status: RegistrationStatus
    distance: float

    # Metadata
    last_seen: float = field(default_factory=time.time)
    
    # Tracking reliability
    missed_frames: int = 0
    
    # Signal Processing
    background_model: Optional[np.ndarray] = None
    led_centroid: Optional[Tuple[int, int]] = None
    signal_buffer: Deque[float] = field(default_factory=lambda: deque(maxlen=settings.active_id.buffer_size))

    def is_expired(self) -> bool:
        """Check if device hasn't been seen for too long."""
        return (time.time() - self.last_seen) > settings.active_id.device_timeout

    def update_position(self, bbox: Tuple[int, int, int, int], distance: float) -> None:
        """Update device position and distance. Only update if changed significantly to avoid jitter."""
        x, y, w, h = self.bbox
        nx, ny, nw, nh = bbox
        dist = np.sqrt(((x+w/2) - (nx+nw/2))**2 + ((y+h/2) - (ny+nh/2))**2)
        
        # Threshold to avoid jitter updates (5% of bbox size)
        if dist > min(w, h) * 0.05:
            self.bbox = bbox
            self.distance = distance
        
        self.last_seen = time.time()

    def handle_miss(self) -> None:
        """Handle a frame where no change was detected. If missed 15 frames, reset LED tracking."""
        self.missed_frames += 1
        if self.missed_frames > settings.active_id.signal_timeout:
            self.led_centroid = None
            self.signal_buffer.clear()