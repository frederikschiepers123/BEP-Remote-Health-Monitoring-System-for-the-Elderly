# src/identification/input/streamers.py
from __future__ import annotations
import threading
import queue
import time
import os
import cv2
from typing import Optional, Tuple, Union
from onvif import ONVIFCamera
from wsdiscovery import WSDiscovery, QName
_NAMESPACE = "http://www.onvif.org/ver10/network/wsdl" 
_LOCALNAME = "NetworkVideoTransmitter"


class BaseStreamer(threading.Thread):
    """
    Base class for streamers. Handles frame capturing from various sources using OpenCV.
    Attributes:
        source_uri (Optional[Union[str, int]]): URI or index of the video source.
        total_captured (int): Total number of frames captured.
        total_dropped (int): Total number of frames dropped due to queue overflow.

    Methods:
        run(): Main thread loop to capture frames.
        read(): Reads the latest frame from the queue.
        stop(): Stops the thread safely.
        get_stream_properties(): Fetches and prints stream properties like resolution and FPS.
        record(output_path: str, duration: int): Records the stream to a file for a specified duration.
        get_metrics(): Returns total captured and dropped frame counts.
    """
    def __init__(self, name: str = "BaseStreamer"):
        super().__init__(name=name, daemon=True)
        self.source_uri: Optional[Union[str, int]] = None

        # Threading primitives
        self._frame_queue = queue.Queue(maxsize=1)
        self._stop_event = threading.Event()

        # Metrics
        self.total_captured = 0
        self.total_dropped = 0

    def run(self) -> None:
        """Main thread loop to capture frames."""
        while not self._stop_event.is_set():
            # Connect to stream
            cap = self._connect_opencv()
            
            if cap is None or not cap.isOpened():
                print(f"[{self.name}] Camera not accessible. Retrying in 4s...")
                self._stop_event.wait(timeout=4.0)
                continue
            
            print(f"[{self.name}] Connected to stream.")

            try:
                self._capture_loop(cap)
            except Exception as e:
                print(f"[{self.name}] Error during capture: {e}")
                
        cap.release()
        print(f"[{self.name}] Stream released.")

    def _capture_loop(self, cap: cv2.VideoCapture):
        """Captures frames from the stream."""
        while not self._stop_event.is_set():
            ret, frame = cap.read()
            if not ret:
                print(f"[{self.name}] Stream ended or lost.")
                break

            self.total_captured += 1

            # Non-blocking put with drop policy
            try:
                self._frame_queue.put_nowait(frame)
            except queue.Full:
                self.total_dropped += 1
                try:
                    self._frame_queue.get_nowait()
                    self._frame_queue.put_nowait(frame)
                except queue.Empty:
                    pass

    def read(self) -> Optional[cv2.Mat]:
        """Reads the latest frame from the queue."""
        try:
            return self._frame_queue.get(timeout=1.0) 
        except queue.Empty:
            return None
        
    def stop(self) -> None:
        """Stops the thread safely."""
        self._stop_event.set()
        self.join(timeout=2.0)
        print(f"[{self.name}] Stopped.")

    def get_stream_properties(self) -> Optional[Tuple[int, int, float]]:
        """Fetches and prints stream properties like resolution and FPS."""
        cap = self._connect_opencv()
        if cap is None or not cap.isOpened():
            return
        
        width = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
        height = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
        fps = cap.get(cv2.CAP_PROP_FPS)
        cap.release()
        return width, height, fps
    
    def _connect_opencv(self) -> Optional[cv2.VideoCapture]:
        """Initializes OpenCV with low-latency flags."""
        if not self.source_uri:
            return None

        os.environ["OPENCV_FFMPEG_CAPTURE_OPTIONS"] = "rtsp_transport;udp|fflags;nobuffer|flags;low_delay|probesize;32|analyzeduration;0"
        cap = cv2.VideoCapture(self.source_uri, cv2.CAP_FFMPEG)
        if cap.isOpened():
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        return cap

    def record(self, output_path: str, duration: int):
        """Records the stream to a file for a specified duration."""
        fourcc = cv2.VideoWriter.fourcc(*'mp4v')
        width, height, fps = self.get_stream_properties() or (640, 480, 25.0)

        out = cv2.VideoWriter(output_path, fourcc, fps, (int(width), int(height)))
        
        start_time = time.time()
        print(f"[Streamer] Recording started for {duration} seconds.")
        
        while time.time() - start_time < duration:
            frame = self.read()
            if frame is not None:
                out.write(frame)
                cv2.imshow("Recording", cv2.resize(frame, (0, 0), fx=0.5, fy=0.5))
                cv2.waitKey(1)

        out.release()
        print(f"[Streamer] Recording finished; file path: {output_path}")

    def get_metrics(self):
        return self.total_captured, self.total_dropped
    

class FileStreamer(BaseStreamer):
    """
    Streamer for video files.
    Attributes:
        source_uri (str): Path to the video file.
        frame_rate (float): Frame rate of the video file.
    
    Methods:
        run(): Main thread loop to capture frames.
        read(): Reads the latest frame from the queue.
        stop(): Stops the thread safely.
        get_stream_properties(): Fetches and prints stream properties like resolution and FPS.
        get_metrics(): Returns total captured and dropped frame counts.
    """
    def __init__(self, file_path: str, frame_rate: float = 25.0):
        super().__init__(name="FileStreamer")
        self.source_uri = file_path
        self.frame_rate: float = frame_rate

    def run(self):
        """Main thread loop to capture frames."""
        cap = self._connect_opencv()
        if cap is None or not cap.isOpened():
            print(f"[{self.name}] Could not open file: {self.source_uri}.")
            return
        
        print(f"[{self.name}] Opened file: {self.source_uri}.")

        self.frame_rate = cap.get(cv2.CAP_PROP_FPS)
        frame_interval = 1.0 / self.frame_rate

        while not self._stop_event.is_set():
            start_time = time.time()
            ret, frame = cap.read()
            if not ret:
                print(f"[{self.name}] Stream ended or lost.")
                break

            self.total_captured += 1

            # Non-blocking put with drop policy
            try:
                self._frame_queue.put_nowait(frame)
            except queue.Full:
                self.total_dropped += 1
                try:
                    self._frame_queue.get_nowait()
                    self._frame_queue.put_nowait(frame)
                except queue.Empty:
                    pass

            elapsed = time.time() - start_time
            wait_time = frame_interval - elapsed
            if wait_time > 0:
                time.sleep(wait_time)

        cap.release()
        self.stop()


class NetworkStreamer(BaseStreamer):
    """
    Streamer for network (RTSP/IP) cameras.
    Attributes:
        source_uri (str): RTSP URI of the network camera.
        ip (Optional[str]): IP address of the camera.
        port (int): Port number for ONVIF connection.
        user (str): Username for camera authentication.
        password (str): Password for camera authentication.

    Methods:
        run(): Main thread loop to capture frames.
        read(): Reads the latest frame from the queue.
        stop(): Stops the thread safely.
        get_stream_properties(): Fetches and prints stream properties like resolution and FPS.
        record(output_path: str, duration: int): Records the stream to a file for a specified duration.
        get_metrics(): Returns total captured and dropped frame counts.
    """
    def __init__(self, ip: Optional[str] = None, port: int = 80, user: str = None, password: str = None):
        super().__init__(name="NetworkStreamer")
        self.ip = ip
        self.port = port
        self.user = user
        self.password = password
        self.service_index: Optional[int] = None

        if self.ip is None:
            self.ip = self._discover_ip()
        if self.source_uri is None:
            self.source_uri = self._discover_uri()

    def run(self) -> None:
        """Main thread loop to capture frames."""
        while not self._stop_event.is_set():
            if self.ip is None:
                self.ip = self._discover_ip()
                if self.ip is None:
                    print(f"[{self.name}] No valid IP found. Retrying discovery in 4s...")
                    self._stop_event.wait(timeout=4.0)
                    continue

            if self.source_uri is None:
                self.source_uri = self._discover_uri()
                if self.source_uri is None:
                    print(f"[{self.name}] Authentication or URI discovery failed for {self.ip}. Dropping IP and searching for next camera...")
                    self.ip = None 
                    self.source_uri = None
                    continue

            cap = self._connect_opencv()
            if cap is None or not cap.isOpened():
                print(f"[{self.name}] Camera not accessible. Retrying in 4s...")
                self._stop_event.wait(timeout=4.0)
                continue
            
            print(f"[{self.name}] Connected to stream.")

            try:
                self._capture_loop(cap)
            except Exception as e:
                print(f"[{self.name}] Error during capture: {e}")

        cap.release()
        print(f"[{self.name}] Stream released.")

    def _discover_ip(self):
        """Discovers the camera IP using WS-Discovery. Supports cycling through multiple discovered services if password is incorrect."""
        print("[RTSPStreamer] Discovering camera via WS-Discovery...")
        discovery = WSDiscovery()
        discovery.start()
        try:
            services = discovery.searchServices(timeout=5, types=[QName(_NAMESPACE, _LOCALNAME)])
            if not services:
                print(f"[{self.name}] No ONVIF services found.")
                return None
            
            self._get_service_index(len(services))
            print(f"[{self.name}] Found {len(services)} services. Trying index {self.service_index}.")

            service = services[self.service_index]
            xaddrs = service.getXAddrs()
            if xaddrs:
                ip = xaddrs[0].split("//")[1].split("/")[0].split(":")[0]
                print(f"[{self.name}] Discovered IP: {ip}")
                return ip
            
            for service in services:
                xaddrs = service.getXAddrs()
                if xaddrs:
                    ip = xaddrs[0].split("//")[1].split("/")[0].split(":")[0]
                    print(f"[{self.name}] Discovered IP: {ip}")
                    return ip
        except Exception as e:
            print(f"[{self.name}] WS-Discovery Failed: {e}")

        discovery.stop()
        return None
    
    def _get_service_index(self, max_index: int):
        """Loops through available services for discovery."""
        if self.service_index is None:
            self.service_index = 0
        else:
            self.service_index += 1

        if self.service_index >= max_index:
            self.service_index = 0

    def _discover_uri(self):
        """Connects to ONVIF to get the RTSP Stream URI."""
        if not self.ip:
            return None
        
        print(f"[{self.name}] Discovering RTSP URI at {self.ip}...")
        try:
            cam = ONVIFCamera(self.ip, self.port, self.user, self.password)
            media = cam.create_media_service()
            profiles = media.GetProfiles()
            if not profiles:
                raise Exception("No media profiles found.")
            
            token = profiles[0].token
            request = media.create_type('GetStreamUri')
            request.ProfileToken = token
            request.StreamSetup = {'Stream': 'RTP-Unicast', 'Transport': {'Protocol': 'RTSP'}}
            
            response = media.GetStreamUri(request)
            uri = response.Uri

            # Inject credentials if needed
            if self.user and self.password and "@" not in uri:
                protocol, address = uri.split("://", 1)
                return f"{protocol}://{self.user}:{self.password}@{address}"
            return uri

        except Exception as e:
            print(f"[{self.name}] ONVIF Discovery Failed: {e}")
            return None

def create_streamer(source: Union[str, int], user: str, password: str, port: int) -> BaseStreamer:
    """
    Helper function to detect source type and return the correct Streamer object.
    Args:
        source (Union[str, int]): Source identifier (file path, IP address, or camera index).
        user (str): Username for network camera authentication.
        password (str): Password for network camera authentication.
        port (int): Port number for ONVIF connection.
    """
    # Case 1: USB Camera (input is an integer)
    if isinstance(source, int):
        print(f"[System] Detected USB Camera Index: {source}")
        # Assuming FileStreamer wraps cv2.VideoCapture, it handles ints automatically
        return BaseStreamer(source)

    # Case 2: RTSP/IP Camera
    is_ip_address = isinstance(source, str) and sum(c.isdigit() for c in source) > 6 and "." in source
    is_rtsp_url = isinstance(source, str) and source.lower().startswith("rtsp://")

    if is_ip_address or is_rtsp_url or (user and password):
        print(f"[System] Detected Network Stream: {source}")
        
        clean_ip = source
        if source is not None and "://" in source:
             try:
                 clean_ip = source.split("@")[-1].split(":")[0].split("/")[0]
             except:
                 pass

        return NetworkStreamer(
            ip=clean_ip,
            port=port,
            user=user,
            password=password
        )

    # Case 3: Video File
    print(f"[System] Detected Video File: {source}")
    return FileStreamer(source)
        
class RTSP_streamer(BaseStreamer):
    def __init__(self, ip: Optional[str] = None, port: int = 80, user: str = None, password: str = None):
        super().__init__(name="NetworkStreamer")
        self.ip = ip
        self.port = port
        self.user = user
        self.password = password

        if not self.ip:
            self.ip = self._discover_ip()

        if not self.source_uri:
            self.source_uri = self._discover_uri()

    def _discover_ip(self):
        """Discovers the camera IP using WS-Discovery."""
        print("[RTSPStreamer] Discovering camera via WS-Discovery...")
        discovery = WSDiscovery()
        discovery.start()
        try:
            services = discovery.searchServices(timeout=5, types=["dn:NetworkVideoTransmitter"])
            for service in services:
                xaddrs = service.getXAddrs()
                if xaddrs:
                    ip = xaddrs[0].split("//")[1].split("/")[0].split(":")[0]
                    print(f"[{self.name}] Discovered IP: {ip}")
                    return ip
        except Exception as e:
            print(f"[{self.name}] WS-Discovery Failed: {e}")

        discovery.stop()
        return None

    def _discover_uri(self):
        """Connects to ONVIF to get the RTSP Stream URI."""
        if not self.ip:
            return None

        print(f"[{self.name}] Discovering RTSP URI at {self.ip}...")
        try:
            cam = ONVIFCamera(self.ip, self.port, self.user, self.password)
            media = cam.create_media_service()
            profiles = media.GetProfiles()
            if not profiles:
                raise Exception("No media profiles found.")

            token = profiles[0].token
            request = media.create_type('GetStreamUri')
            request.ProfileToken = token
            request.StreamSetup = {'Stream': 'RTP-Unicast', 'Transport': {'Protocol': 'RTSP'}}

            response = media.GetStreamUri(request)
            uri = response.Uri

            # Inject credentials if needed
            if self.user and self.password and "@" not in uri:
                protocol, address = uri.split("://", 1)
                return f"{protocol}://{self.user}:{self.password}@{address}"
            return uri

        except Exception as e:
            print(f"[{self.name}] ONVIF Discovery Failed: {e}")
            return None