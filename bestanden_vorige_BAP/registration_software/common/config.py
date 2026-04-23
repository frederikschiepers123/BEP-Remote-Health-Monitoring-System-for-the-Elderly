# src/common/config.py
from __future__ import annotations
from pathlib import Path
from typing import Optional, Literal
from pydantic import Field, BaseModel
from pydantic_settings import BaseSettings, SettingsConfigDict

# --- PATHS (Static, relative to this file) ---
_CURRENT_FILE = Path(__file__).resolve()
PROJECT_ROOT = _CURRENT_FILE.parent.parent.parent
RESOURCES_DIR = PROJECT_ROOT / "resources"

# Ensure directories exist
(RESOURCES_DIR / "creds").mkdir(parents=True, exist_ok=True)
(RESOURCES_DIR / "labels" / "qr_imgs").mkdir(parents=True, exist_ok=True)

HOST_MAP = {
    "server": "127.0.0.1",
}

#--------- Camera registration settings ---------
# Set USE_Y to True if secondary cameras are placed in a grid formation and False if they are place on a single line.
USE_Y= False

# DISTANCE determines the max distance in meters between a secondary and primary translation vector.
# if this distance is surpassed for a camera this camera will not be given a label.
DISTANCE= 0.08

# Specs for the main camera
#Specs when using foscam R2
PIXELS_WIDTH_1 = 1920
PIXELS_HEIGHT_1 = 1080
HORIZONTAL_FOV_ANGLE_1 = 95
VERTICAL_FOV_ANGLE_1 = 65

# Specs for the secondary cameras
# Specs when using  Hikvision cam as secondary cam
PIXELS_WIDTH_2 = 2688
PIXELS_HEIGHT_2 = 1520
HORIZONTAL_FOV_ANGLE_2 = 98
VERTICAL_FOV_ANGLE_2 = 55

# ONVIF credentials. Leave as None if you don’t have them yet.
USERNAME = "admin"   # e.g. "admin" depends on device fabricator
PASSWORD = "Edisonpro!"   # e.g. "pass" or "admin" depends on device fabricator

# This dictates how long the algorithm will search for onvif devices on the lan.
DISCOVERY_TIMEOUT = 9.0

# This dictates how long the algorithm will wait for a snapshot after requesting one.
SNAPSHOT_TIMEOUT = 1.0

# This dictates how long th algorithm will fetch frames from the live video feed.
RTSP_DETECT_WINDOW = 1.0

# Identify the main (overview) camera by IP/host (recommended).
# If left None, the first discovered device will be used as the overview camera.
MAIN_CAMERA_HOST = "192.168.0.101"  # e.g. "192.168.1.100"

# Size of the QR code measured in meters
# This measurement is taken from identifier to identifier measuring from the outside edges.
# (This implies that  the quiet space/white space is excluded from the measurement).
SIZE_QR = 0.025

REGISTRY_PATH = RESOURCES_DIR / "device_registry.json"


# Sub configs for grouping related settings
class MqttConfig(BaseModel):
    host: str = "server"
    port_secure: int = 8883
    port_non_secure: int = 1884
    advertise_ip: str = "192.168.0.255" # Broadcast address for UDP advertisements, change to match the local network
    advertise_port: int = 5005

class RegistrationConfig(BaseModel):
    client_name: str = "registration_server_user"
    username: str = "registration_server_user"
    password: str = "supersecurepassword123"
    enroll_idle_threshold: int = 3 # seconds
    label_timeout: int = 5 # seconds

class CameraConfig(BaseModel):
    ip: Optional[str] = None
    username: str = "admin"
    password: str = "Edisonpro!"
    http_port: int = 80
    discovery_timeout: float = 4.0

    sample_rate: float = 25.0
    resolution_width: int = 2688
    resolution_height: int = 1520

    focal_length_mm: float = 2.8
    aperture: float = 1.6
    sensor_width_mm: float = 4.8
    sensor_height_mm: float = 3.6

    # From calibration tool
    distance_constant: float = 39800.0
    near_limit_mm: float = 170.0
    far_limit_mm: float = 550.0

class QrConfig(BaseModel):
    qr_size_mm: float = 25.0
    qr_version: int = 4
    ppm_threshold: float = 2.0

class ActiveIDConfig(BaseModel):
    # Background Subtraction
    background_learning_rate: float = 0.04
    noise_floor: float = 50.0       # in pixel intensity units (0-255)
    noise_sigma_factor: float = 5.0
    
    # Signal Extraction
    led_color: str = "all"      # Options: "red", "green", "blue", "all"
    patch_size: int = 4         # in pixels
    signal_timeout: int = 15    # in frames
    
    # Frequency Analysis
    target_frequency: float = 5.0   # in Hz
    min_signal_size: int = 25       # in samples
    buffer_size: int = 50           # in samples
    magnitude_threshold: float = 0.6  # normalized (0-0.707)
    
    # Detection Timing & Limits
    qr_scan_interval: int = 25     # in frames
    device_timeout: float = 2.5    # in seconds
    bbox_padding_percent: float = 50.0
    
class PathsConfig(BaseModel):
    resources: Path = RESOURCES_DIR
    ca_cert: Path = RESOURCES_DIR / "creds" / "ca.crt"
    ca_key: Path = RESOURCES_DIR / "creds" / "ca.key"
    server_cert: Path = RESOURCES_DIR / "creds" / "server.crt"
    server_key: Path = RESOURCES_DIR / "creds" / "server.key"
    client_cert: Path = RESOURCES_DIR / "creds" / "registration_server.crt"
    client_key: Path = RESOURCES_DIR / "creds" / "registration_server.key"
    qr_public_key: Path = RESOURCES_DIR / "creds" / "qr_public.pem"
    qr_private_key: Path = RESOURCES_DIR / "creds" / "qr_private.pem"
    registry: Path = RESOURCES_DIR / "device_registry.json"
    label_registry: Path = RESOURCES_DIR / "labels" / "label_registry.json"
    qr_img_dir: Path = RESOURCES_DIR / "labels" / "qr_imgs"
    registration_camera: Path = RESOURCES_DIR / "registration_camera.json"

# Main settings class
class Settings(BaseSettings):
    """Global application settings."""
    mqtt: MqttConfig = Field(default_factory=MqttConfig)
    registration: RegistrationConfig = Field(default_factory=RegistrationConfig)
    camera: CameraConfig = Field(default_factory=CameraConfig)
    active_id: ActiveIDConfig = Field(default_factory=ActiveIDConfig)
    paths: PathsConfig = Field(default_factory=PathsConfig)
    qr: QrConfig = Field(default_factory=QrConfig)

# Instantiate once. Import 'settings' in other files.
settings = Settings()