# src/identification/active_identification.py
from __future__ import annotations
import time
import threading
import queue
import numpy as np
from typing import Dict, Optional, Tuple
from collections import deque

from registration_software.common.config import settings
from registration_software.models.device import DeviceState
from registration_software.models.enums import RegistrationStatus, IdentificationEvent, QrDetectorEvent
from registration_software.identification.processing.qr_detector import QrCodeDetector
from registration_software.identification.processing.led_detector import LEDDetector
from registration_software.interface.streamers import BaseStreamer
from registration_software.interface.visualizer import Visualizer

class ActiveIdentificationTracker(threading.Thread):
    """
    Service that tracks devices with QR codes and blinking LEDs.

    Attributes:
        video_source: BaseStreamer instance for video input
        qr_detector: QrCodeDetector instance for QR code detection
        led_detector: LEDDetector instance for LED signal processing
        visualizer: Optional Visualizer instance for real-time display
    """
    def __init__(self,
                 video_source: BaseStreamer,
                 qr_detector: QrCodeDetector,
                 led_detector: LEDDetector,
                 visualizer: Optional[Visualizer] = None) -> None:
        super().__init__(name="ActiveIdentificationTracker", daemon=True)
        self.video_source = video_source
        self.qr_detector = qr_detector
        self.led_detector = led_detector
        self.visualizer = visualizer
        
        # Event management
        self._stop_event = threading.Event()
        self._output_queue: queue.Queue = queue.Queue()

        # Device management
        self.active_devices: Dict[str, DeviceState] = {}
        self.registered_labels: set[str] = set()

    def run(self) -> None:
        print("[ActiveIdentificationTracker] Started")
        self._start_subservices()

        frame_index = 0
        try:
            while not self._stop_event.is_set():
                frame = self.video_source.read()
                if frame is None:
                    time.sleep(0.005)
                    continue

                frame_index += 1
                start_time = time.time()
                
                # Periodic qr detection
                if frame_index % settings.active_id.qr_scan_interval == 0:
                    self.qr_detector.push_frame(frame)
                self._handle_qr_events()

                # Update the devices and run LED tracking
                self._update_device_state(frame)

                # Visualization
                if self.visualizer:
                    self._update_visualizer(frame, start_time)

        except KeyboardInterrupt:
            print("[ActiveIdentificationTracker] KeyboardInterrupt received. Stopping...")
            self.stop()
        except Exception as e:
            print(f"[ActiveIdentificationTracker] Error in main loop: {e}")
            
        self.stop()

    def stop(self) -> None:
        if self._stop_event.is_set():
            return
        self._stop_event.set()
        self.qr_detector.stop()
        self.video_source.stop()
        if self.visualizer:
            self.visualizer.stop()
        print("[ActiveIdentificationTracker] Stopped")

    def get_events(self, block: bool = False, timeout: Optional[float] = None) -> Optional[Dict]:
        try:
            return self._output_queue.get(block=block, timeout=timeout)
        except queue.Empty:
            return None
        
    def _start_subservices(self) -> None:
        """Checks and starts internal threads/processes if they aren't active."""
        if not self.video_source.is_alive():
            self.video_source.start()
        if not self.qr_detector.is_alive():
            self.qr_detector.start()
        if self.visualizer and not self.visualizer.is_alive():
            self.visualizer.start()

    def add_registered_labels(self, labels: set) -> None:
        """Add labels to the set of registered devices."""
        self.registered_labels.update(labels)

    def _handle_qr_events(self) -> None:
        """Get results from the QR detector and update the tracking list."""
        qr_events = []
        while True:
            qr_event = self.qr_detector.get_events(block=False)
            if not qr_event:
                break
            qr_events.append(qr_event)

        for qr_event in qr_events:
            event_type = qr_event.get('type')
            label = qr_event.get('label')
            bbox = qr_event.get('bbox')
            authenticity = qr_event.get('authenticity', False)
            distance = self.estimate_distance(bbox)

            # Determine device status
            if event_type == QrDetectorEvent.QR_DETECTED and authenticity:
                initial_status = RegistrationStatus.SEARCHING
            elif event_type == QrDetectorEvent.QR_UNDECODED:
                initial_status = RegistrationStatus.NOT_DECODED
            elif label in self.registered_labels:
                initial_status = RegistrationStatus.REGISTERED
            else:
                initial_status = RegistrationStatus.UNAUTHORIZED
            if (distance < settings.camera.near_limit_mm or distance > settings.camera.far_limit_mm):
                initial_status = RegistrationStatus.OUT_OF_RANGE

            # Add or update device
            if label in self.active_devices:
                self._update_device_position(label, bbox, distance)
            else:
                self._add_new_device(label, bbox, initial_status, distance)
    
    def _update_device_position(self, label: str, bbox: Tuple[int, int, int, int], distance: float) -> None:
        """Update the position and distance of an existing device."""
        device = self.active_devices[label]
        device.update_position(bbox, distance)

    def _add_new_device(self, label: str, bbox: Tuple[int, int, int, int], status: RegistrationStatus, distance: float) -> None:
        """Add a new device to tracking dict."""
        if self._is_overlapping(bbox):
            return
            
        new_device = DeviceState(
            label=label,
            bbox=bbox,
            status=status,
            distance=distance,
            signal_buffer=deque(maxlen=settings.active_id.buffer_size),
            last_seen=time.time()
        )
        self.active_devices[label] = new_device
        self._output_queue.put({
            "type": IdentificationEvent.DEVICE_FOUND,
            "label": label
        })

    def estimate_distance(self, bbox: Tuple[int, int, int, int]) -> float:
        """Estimate distance to device based on bbox size."""
        _, _, w, _ = bbox
        if w <= 0:
            return 0.0
        distance_mm = settings.camera.distance_constant / w
        return distance_mm

    def _is_overlapping(self, bbox: Tuple[int, int, int, int]) -> bool:
        """Check if the given bbox overlaps with any existing device bboxes."""
        x2, y2, w2, h2 = bbox
        for device in self.active_devices.values():
            x1, y1, w1, h1 = device.bbox
            if (x1 < x2 + w2 and x1 + w1 > x2 and
                y1 < y2 + h2 and y1 + h1 > y2):
                return True
            cx2, cy2 = x2 + w2 // 2, y2 + h2 // 2
            cx1, cy1 = x1 + w1 // 2, y1 + h1 // 2
            distance = np.sqrt((cx1 - cx2) ** 2 + (cy1 - cy2) ** 2)
            if distance < max(w1, h1, w2, h2) * 0.5:
                return True
        return False

    def _update_device_state(self, frame: np.ndarray):
        """Update all devices in the tracking dict and run LED identification."""
        devices = list(self.active_devices.values())
        for device in devices:

            # Remove stale devices
            if device.is_expired() and device.status != RegistrationStatus.IDENTIFIED:
                del self.active_devices[device.label]
                continue
            
            # Only process searching devices
            if device.status != RegistrationStatus.SEARCHING:
                continue

            verified = self.led_detector.process_frame(device, frame)
            if verified:
                self._output_queue.put({
                    "type": IdentificationEvent.IDENTIFIED, 
                    "label": device.label
                })

    def _update_visualizer(self, frame: np.ndarray, start_time: float) -> None:
        """Pushes data to the visualizer."""
        processing_time = time.time() - start_time
        stream_metrics = self.video_source.get_metrics()

        device_data = []
        for device in self.active_devices.values():
            device_data.append({
                "bbox": device.bbox,
                "label": device.label,
                "status": device.status,
                "distance": device.distance,
                "buffer": list(device.signal_buffer),
                "led_centroid": device.led_centroid
            })
        self.visualizer.update(frame, device_data, processing_time, stream_metrics)