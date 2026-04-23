# src/identification/interfaces/visualizer.py
from __future__ import annotations
import multiprocessing
import queue
from typing import Dict, List, Tuple
import cv2
import numpy as np

from registration_software.models.enums import RegistrationStatus
from registration_software.common.config import settings

_COLOR_MAP = {
        RegistrationStatus.IDENTIFIED: (255, 255, 0), # Yellow
        RegistrationStatus.SEARCHING: (0, 255, 0),      # Green
        RegistrationStatus.REGISTERED: (255, 255, 255),   # White
        RegistrationStatus.UNAUTHORIZED: (0, 0, 255),   # Red
        RegistrationStatus.NOT_DECODED: (255, 0, 0),    # Blue
        RegistrationStatus.OUT_OF_RANGE: (0, 0, 255)    # Red
    }

class Visualizer(multiprocessing.Process):
    """
    Visualizer process that displays video frames with overlays for active device tracking.

    Features:
        - Limited to 1920x1080 display size to save resources
        - Metrics use a moving average for smoother display
        - Dashboard with system metrics in top left corner:
            - Active devices (count) | Identified devices (count)
            - Average processing time per frame (ms)
            - Duty cycle (%)
            - Performance drop (%)
            - Frame drops (count | %)
            - Average scene brightness
        - for each tracked device:
            - Bounding boxes
            - Labels
            - Status
            - Distance
            - LED markers
            - Signal graphs.
    Attributes:
        window_name (str): Name of the display window.
    Methods:
        update(): Queues a new frame and device data for visualization.
        run(): Main loop that processes queued frames and displays them with overlays.
        stop(): Signals the process to stop and cleans up resources.
    """
    def __init__(self, window_name: str = "Feedback Window") -> None:
        super().__init__(name="Visualizer", daemon=True)
        self.window_name = window_name
        self._queue = multiprocessing.Queue(maxsize=1) 
        self._stop_event = multiprocessing.Event()

        self.metric_factor = 0.1
        self.avg_proc_ms = 0.0
        self.avg_brightness = 0.0

    def update(self, frame: np.ndarray, devices: List, processing_time: float, stream_metrics: Tuple[float, float]) -> None:
        """Update the visualizer with a new frame, system metrics and device data."""
        if self._stop_event.is_set():
            return
        try:
            self._queue.put_nowait((frame, devices, processing_time, stream_metrics))
        except queue.Full:
            pass

    def run(self):
        """Main loop for the visualizer process."""
        print(f"[Visualizer]: Started window '{self.window_name}'")
        while not self._stop_event.is_set():
            try:
                frame, devices, processing_time, stream_metrics = self._queue.get(timeout=0.5)

                self._draw_dashboard(frame, devices, processing_time, stream_metrics)
                self._draw_overlay(frame, devices)

                display_frame = cv2.resize(frame, (1920, 1080))
                cv2.imshow(self.window_name, display_frame)

                if cv2.waitKey(1) & 0xFF == ord('q'):
                    self._stop_event.set()

            except queue.Empty:
                continue
            except Exception as e:
                print(f"[Visualizer]: Error in main loop: {e}")
                    
        cv2.destroyWindow(self.window_name)

    def _draw_overlay(self, frame: np.ndarray, devices: List) -> None:
        """Draws bounding boxes, labels, and status info on the frame."""
        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 0.6
        font_thickness = 2
        
        for device in devices:
            bbox = device['bbox']
            
            status = device['status']
            color = _COLOR_MAP.get(status, (255, 255, 255))

            if status == RegistrationStatus.UNAUTHORIZED or \
                status == RegistrationStatus.OUT_OF_RANGE or \
                status == RegistrationStatus.REGISTERED:
                px, py, pw, ph = bbox
            else:
                px, py, pw, ph = self._get_padded_bbox(bbox)
            
            # Distance range check
            range_info = ""
            if device['distance'] > settings.camera.far_limit_mm:
                range_info = " [TOO FAR]"
            elif device['distance'] < settings.camera.near_limit_mm:
                range_info = " [TOO CLOSE]"
            
            # Create text
            label_text = f"{device['label']} ({status.value})"
            dist_text = f"{device['distance']:.0f}mm{range_info}"
            
            # Text size and positioning
            (label_w, label_h), _ = cv2.getTextSize(label_text, font, font_scale, font_thickness)
            (dist_w, dist_h), _ = cv2.getTextSize(dist_text, font, font_scale, font_thickness)
            
            text_w = max(label_w, dist_w)
            total_text_h = label_h + dist_h + 15
            text_x = px
            text_y = py - 10
            if text_y - total_text_h < 0: 
                text_y = py + total_text_h + 10
            
            # Draw the device box
            cv2.rectangle(frame, (px, py), (px + pw, py + ph), color, 2)
            cv2.rectangle(frame, 
                          (text_x, text_y - label_h - 10), 
                          (text_x + text_w + 10, text_y + dist_h + 10), 
                          (0, 0, 0), -1)
            cv2.putText(frame, label_text, (text_x + 5, text_y - 5), 
                        font, font_scale, color, font_thickness)
            cv2.putText(frame, dist_text, (text_x + 5, text_y + dist_h + 5), 
                        font, font_scale, (255, 255, 255), 1)

            # Draw LED centroid
            if device['led_centroid']:
                lx, ly = device['led_centroid']
                cx, cy = px + lx, py + ly
                cv2.rectangle(frame, (cx - 5, cy - 5), (cx + 5, cy + 5), (0, 0, 255), 2)
                #cv2.drawMarker(frame, (cx, cy), (0, 0, 255), cv2.MARKER_CROSS, 15, 2)
                
            # Draw signal graph
            self._draw_signal(frame, device['buffer'], px, py + ph, pw)

    def _get_padded_bbox(self, bbox: Tuple[int, int, int, int]) -> Tuple[int, int, int, int]:
        """Apply padding to a bounding box."""
        x, y, w, h = bbox
        pad_w = int(w * (settings.active_id.bbox_padding_percent / 100.0))
        pad_h = int(h * (settings.active_id.bbox_padding_percent / 100.0))
        return (max(0, x - pad_w), max(0, y - pad_h), w + 2 * pad_w, h + 2 * pad_h)

    def _draw_signal(self, frame: np.ndarray, signal: List[float], x: int, y: int, w: int):
        """Draws a small plot of the brightness buffer below the device box."""
        if len(signal) < 2:
            return
        
        h, offset = 50, 5
        y = y + offset
        
        # Ensure graph doesn't go off bottom of frame
        # Flip to inside/above bottom edge if needed
        if y + h >= frame.shape[0]:
            y = y - h - offset - 10
            
        cv2.rectangle(frame, (x, y), (x + w, y + h), (20, 20, 20), -1)

        signal_min, signal_max = min(signal), max(signal)
        rng = signal_max - signal_min
        if rng == 0:
            return

        points = []
        for i, value in enumerate(signal):
            px_graph = int(x + (i / len(signal)) * w)
            norm_value = (value - signal_min) / rng
            py_graph = int((y + h) - (norm_value * h))
            points.append((px_graph, py_graph))

        cv2.polylines(frame, [np.array(points)], False, (0, 255, 255), 1)

    def _draw_dashboard(self, frame: np.ndarray, devices: Dict, processing_time: float, stream_metrics: Tuple[float, float]) -> None:
        """Draws the dashboard with system metrics on the frame in the top left corner."""

        # Tracking metrics
        device_count = len(devices)
        verified_count = sum(1 for device in devices if device['status'] == RegistrationStatus.IDENTIFIED)
        proc_ms = processing_time * 1000.0
        self.avg_proc_ms = (self.metric_factor * proc_ms) + ((1 - self.metric_factor) * self.avg_proc_ms)
            
        # Calculate duty cycle
        frame_budget_ms = 1000.0 / settings.camera.sample_rate
        duty_cycle = (self.avg_proc_ms / frame_budget_ms) * 100.0
        perf_drop = max(0.0, ((self.avg_proc_ms - frame_budget_ms) / frame_budget_ms) * 100.0)

        # Stream metrics
        total_cap, total_drop = stream_metrics
        drop_pct = (total_drop / total_cap * 100.0) if total_cap > 0 else 0.0

        # Scene brightness
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        brightness = np.mean(gray)
        self.avg_brightness = (self.metric_factor * brightness) + ((1 - self.metric_factor) * self.avg_brightness)

        # Draw dashboard
        cv2.rectangle(frame, (5, 5), (500, 250), (0, 0, 0), -1)
        info_text = f"Active: {device_count} | Identified: {verified_count}"
        cv2.putText(frame, info_text, (15, 40), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (225, 225, 225), 2)

        performance_metrics = [
            f"Process: {self.avg_proc_ms:.1f} ms",
            f"Duty: {duty_cycle:.1f}%",
            f"Drop (Perf): {perf_drop:.1f}%",
            f"Drop (Frame): {total_drop} ({drop_pct:.1f}%)",
            f"Brightness: {self.avg_brightness:.1f}"
        ]

        for i, text in enumerate(performance_metrics):
            cv2.putText(frame, text, (15, 80 + i * 30), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (200, 200, 200), 1)

    def stop(self):
        self._stop_event.set()
        try:
            while not self._queue.empty():
                self._queue.get_nowait()
        except:
            pass
        print("[Visualizer]: Stopped")