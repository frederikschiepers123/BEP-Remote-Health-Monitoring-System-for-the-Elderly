# src/identification/processing/qr_detector.py
from __future__ import annotations
import queue
import multiprocessing
from typing import Optional, Tuple, Dict, Any, List
import numpy as np
import cv2
from qreader import QReader
import warnings

from registration_software.common.config import settings
from registration_software.models.enums import QrDetectorEvent
from registration_software.common.crypto.key_loader import load_public_key_from_file

warnings.filterwarnings("ignore", category=UserWarning, message=".*Double decoding failed.*")


class QrCodeDetector(multiprocessing.Process):
    """
    QR code detector used to detect and verify QR codes in video frames.

    Can operate in two modes:
    1. Background Processing Mode:
        - Runs as a separate process.
        - Accepts frames via a non-blocking queue.
        - Outputs detected QR code events to an output queue.
    2. Synchronous Single Frame Mode:
        - Processes a single frame on demand.
        - Returns detected QR code events directly.

    Attributes:
        public_key: The public key used for verifying QR code signatures.
        detector: The QReader instance for QR code detection.
    """
    def __init__(self):
        super().__init__(name="QrCodeDetector", daemon=True)
        self.public_key = None
        self.detector: Optional[QReader] = None
        
        # Queues and Flags
        self._output_queue: multiprocessing.Queue = multiprocessing.Queue()
        self._input_queue: multiprocessing.Queue = multiprocessing.Queue(maxsize=1)
        self._stop_event = multiprocessing.Event()

    def _initialize(self):
        if self.detector is not None:
            return

        self.public_key = load_public_key_from_file(settings.paths.qr_public_key)
        self.detector = QReader(model_size='n')

    def push_frame(self, frame: np.ndarray) -> None:
        """Non-blocking function to submit a frame for processing."""
        if self._stop_event.is_set():
            return
        try:
            self._input_queue.put_nowait(frame)
        except queue.Full:
            pass

    def run(self):
        """Main processing loop for background mode."""
        print("[QrCodeDetector] Started")
        try:
            self._initialize()
        except Exception as e:
            print(f"[QrCodeDetector] Initialization failed: {e}")
            return
        
        while not self._stop_event.is_set():
            try:
                frame = self._input_queue.get(timeout=0.5)
                events = self._detect(frame)
                for event in events:
                    self._output_queue.put(event)
            except queue.Empty:
                continue
            except Exception as e:
                print(f"[QrCodeDetector] Error during detection loop: {e}")

    def detect_single_frame(self, frame: np.ndarray) -> List[Dict]:
        """Synchronous detection for a single frame."""
        try:
            self._initialize()
            return self._detect(frame)
        except Exception as e:
            print(f"[QrCodeDetector] Detection failed: {e}")
            return []

    def _detect(self, frame: np.ndarray) -> List[Dict]:
        """Runs detection on a single frame and returns a list of events."""
        events = []
        try:
            payloads, detections = self.detector.detect_and_decode(image=frame, return_detections=True)
        except Exception:
            return events

        if not detections:
            return events

        # Normalize containers
        if not isinstance(detections, (list, tuple)):
            detections = [detections]
        if not isinstance(payloads, (list, tuple)):
            payloads = [payloads]

        for detection, payload in zip(detections, payloads):
            if detection is None:
                continue

            bbox, quad = self._get_bbox(detection)
            if not bbox:
                continue

            if payload is None:
                events.append({
                    "type": QrDetectorEvent.QR_UNDECODED,
                    "label": "NOT_DECODED",
                    "bbox": bbox,
                    "authenticity": False,
                    "quad": quad
                })
                continue

            authenticity, label = self._check_authenticity_bytes(payload)
            events.append({
                "type": QrDetectorEvent.QR_DETECTED,
                "label": label,
                "bbox": bbox,
                "authenticity": authenticity,
                "quad": quad
            })
        
        return events
    
    def _get_bbox(self, detection: Any) -> Optional[Tuple[float, float, float, float]]:
        try:
            if isinstance(detection, dict):
                if "quad" in detection and detection["quad"] is not None:
                    points = detection.get("quad")  # shape ~ (4,2)
                elif "polygon" in detection and detection["polygon"] is not None:
                    points = detection.get("polygon")  # shape ~ (N,2)
                elif "bbox_xyxy" in detection and detection["bbox_xyxy"] is not None:
                    points = detection.get("bbox_xyxy")
                    x1, y1, x2, y2 = detection["bbox_xyxy"]
                    q_pts = np.array([[x1, y1], [x2, y1], [x2, y2], [x1, y2]],
                                     dtype=np.float32)  # To ensure correct number of points for "quad"
                else:
                    points = None
            else:
                points = detection

            pts = np.array(points, dtype=np.float32).reshape(-1, 2)
            if pts is None:
                return None, None

            x, y, w, h = cv2.boundingRect(pts.astype(np.int32))
            if q_pts is not None:
                q_pts = np.array(q_pts, dtype=np.float32).reshape(-1, 2)
                quad = q_pts.tolist()
            else:
                quad = pts.tolist()

            return (int(x), int(y), int(w), int(h)), quad
        except Exception:
            return None, None

    def _check_authenticity_bytes(self, qr_data: str) -> Tuple[bool, str]:
        """Verifies the signature of the QR code data."""
        if not qr_data:
            return False, ""

        try:
            # Attempt to interpret as latin1 to recover bytes, or expect Base64 here
            data_bytes = qr_data.encode("latin1")

            if len(data_bytes) < 66:  # 1 (len) + 1 (min label) + 64 (sig)
                return False, ""
        
            # --- Protocol Logic (Length + Label + Signature) ---
            label_len = data_bytes[0]
            label_bytes = data_bytes[1: 1 + label_len]
            signature = data_bytes[1 + label_len: 1 + label_len + 64]

            self.public_key.verify(signature, label_bytes)
            return True, label_bytes.decode("utf-8")
        except Exception:
            try:
                return False, label_bytes.decode("utf-8")
            except:
                return False, ""
            
    def get_events(self, block: bool = False, timeout: Optional[float] = None) -> Optional[Dict]:
        try:
            return self._output_queue.get(block=block, timeout=timeout)
        except queue.Empty:
            return None
    
    def stop(self) -> None:
        """Signals the thread to stop."""
        self._stop_event.set()
        print("[QrCodeDetector] Stopped")