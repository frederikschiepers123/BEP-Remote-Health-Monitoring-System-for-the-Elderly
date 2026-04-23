# src/identification/__init__.py
from __future__ import annotations

from registration_software.common.config import settings
from registration_software.interface.streamers import create_streamer
from registration_software.interface.visualizer import Visualizer
from registration_software.identification.processing.qr_detector import QrCodeDetector
from registration_software.identification.processing.led_detector import LEDDetector
from registration_software.identification.active_identification import ActiveIdentificationTracker

def get_tracker(manual_source: str | int = None) -> ActiveIdentificationTracker:
    """
    Helper function to create the ActiveIdentificationTracker.
    
    Args:
        manual_source: Optional override for the video source (file path, IP, or int).
                       If None, uses settings.camera.main_host or auto-discovery.
    Returns:
        An instance of ActiveIdentificationTracker.
    """
    # Determine source
    source = manual_source
    if source is None:
        source = settings.camera.ip
    
    # Create Components
    streamer = create_streamer(
        source=source, 
        user=settings.camera.username, 
        password=settings.camera.password,
        port=settings.camera.http_port
    )
    qr_detector = QrCodeDetector()
    led_detector = LEDDetector()
    visualizer = Visualizer()
    
    tracker = ActiveIdentificationTracker(
        video_source=streamer,
        qr_detector=qr_detector,
        led_detector=led_detector,
        visualizer=visualizer
    )
    
    print(f"Tracker created with source: {source}")
    return tracker