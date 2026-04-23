from requests.auth import HTTPDigestAuth, HTTPBasicAuth
from urllib.parse import urlparse, urlunparse
import requests, cv2, time, os, json, numpy as np
from onvif import ONVIFCamera
from getmac import get_mac_address
from wsdiscovery.discovery import ThreadedWSDiscovery as WSDiscovery
from wsdiscovery import QName

from registration_software.common.config import *
_NAMESPACE = "http://www.onvif.org/ver10/network/wsdl"
_LOCALNAME = "NetworkVideoTransmitter"


class Class_onvif:
    """
    Used to connect to ONVIF camera and get frames and device UUID, MAC
    """
    def __init__(self,):
        self.devices = []
        self.timeout = DISCOVERY_TIMEOUT
        self.user = USERNAME
        self.password = PASSWORD
        self.detect_window = RTSP_DETECT_WINDOW



    def discover_onvif_devices(self):
        """Discover ONVIF devices via WS-Discovery."""
        wsd = WSDiscovery()
        wsd.start()
        print(f'Looking for ONVIF devices on the network...')
        services = wsd.searchServices(timeout= self.timeout, types=[QName(_NAMESPACE, _LOCALNAME)])
        wsd.stop()

        devices = {}
        n = 0
        for s in services:
            xaddrs = s.getXAddrs() or []
            # Get the endpoint reference (EPR) – usually contains a stable UUID
            try:
                epr = s.getEPR()  # e.g. "urn:uuid:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
            except Exception:
                epr = None  # could not get EPR

            # Prefer typical ONVIF device_service URL if present
            xaddr = next((u for u in xaddrs if "device_service" in u or "/onvif" in u),
                         (xaddrs[0] if xaddrs else None))
            if not xaddr:
                continue

            p = urlparse(xaddr)
            host = p.hostname
            port = p.port or (443 if p.scheme == "https" else 80)
            key = (host, port)

            # MAC address (no admin rights needed with getmac)
            mac = get_mac_address(ip=host) or ""

            snapshot_url = self.onvif_snapshot_uri(host, port)
            if snapshot_url:
                # Fix host if camera returns a wrong hostname in the URI
                snapshot_url = self.fix_uri_host(snapshot_url, host, fallback_port=port)

            rtsp = self.discover_stream_uri(host, port)  # Or onvif_stream_uri()

            if key not in devices:
                devices[key] = {
                    "host": host,
                    "port": port,
                    "xaddr": xaddr,
                    "uuid": epr or None,
                    "mac": mac,
                    "snapshot_url": snapshot_url or "",
                    "rtsp_url": rtsp or "",
                }
                print(f"Device {n}:")
                print(json.dumps(devices[key], indent=2, ensure_ascii=False))
                n = n + 1

        self.devices = list(devices.values())
        return self.devices


    def onvif_snapshot_uri(self, host, port):
        """Get official ONVIF snapshot URI (requires credentials)."""
        try:
            from onvif import ONVIFCamera
            cam = ONVIFCamera(host, port, self.user, self.password)
            media = cam.create_media_service()
            profiles = media.GetProfiles()
            if not profiles:
                return None
            # Pick profile with the largest width (fallback: first)
            best, best_w = None, -1
            for p in profiles:
                try:
                    w = getattr(p.VideoEncoderConfiguration.Resolution, "Width", -1) or -1
                    if w > best_w:
                        best, best_w = p, w
                except Exception:
                    if best is None:
                        best = p
            snap = media.GetSnapshotUri({"ProfileToken": best.token})
            return snap.Uri
        except Exception:
            return None

    def discover_stream_uri(self, host, port):
        """Connects to ONVIF to get the RTSP Stream URI."""
        try:
            cam = ONVIFCamera(host, port, self.user, self.password)
            media = cam.create_media_service()
            profile = media.GetProfiles()[0]

            req = media.create_type('GetStreamUri')
            req.ProfileToken = profile.token
            req.StreamSetup = {'Stream': 'RTP-Unicast', 'Transport': {'Protocol': 'RTSP'}}

            resp = media.GetStreamUri(req)
            raw_uri = resp.Uri

            # Inject credentials securely
            if self.user and self.password:
                # Handle cases where URI might already have auth or different format
                proto, address = raw_uri.split("://", 1)
                uri = f"{proto}://{self.user}:{self.password}@{address}"
            else:
                uri = raw_uri
            return uri

        except Exception as e:
            print(f"ONVIF Discovery Failed: {e}")
            raise e

    def fix_uri_host(self, uri, host, fallback_port=None):
        """Some cameras return URIs with a wrong hostname. Force the discovered IP."""
        try:
            pu = urlparse(uri)
            port = pu.port or fallback_port
            netloc = f"{host}:{port}" if port else host
            return urlunparse(pu._replace(netloc=netloc))
        except Exception:
            return uri

    def get_frame(self, snapshot_url):# self.user="admin", password="admin"
        auth = HTTPBasicAuth(self.user, self.password) if self.user and self.password else None

        resp = requests.get(snapshot_url, auth=auth, timeout=SNAPSHOT_TIMEOUT, verify=False)
        resp.raise_for_status()

        jpg_bytes = np.frombuffer(resp.content, dtype=np.uint8)
        frame = cv2.imdecode(jpg_bytes, cv2.IMREAD_COLOR)  # BGR image
        return frame

    """
    Open the RTSP stream with OpenCV, read frames for x seconds,
    and save them as individual images.
    """
    def capture_x_second(self, rtsp_url):
        cap = cv2.VideoCapture(rtsp_url)

        if not cap.isOpened():
            print(f"  [frames] Could not open stream: {rtsp_url}")
            return

        start = time.time()
        frame_idx = 0
        frames = []
        print(f"  [frames] Capturing ~{self.detect_window}s of frames")
        while True:
            ok, frame = cap.read()
            if not ok:
                print("  [frames] Stream ended or read failed.")
                break
            frame_idx += 1
            frames.append(frame)

            # Stop after ~x second
            if time.time() - start >= self.detect_window:
                break

        cap.release()
        print(f"  [frames] Captured {frame_idx} frames\n")
        if len(frames) == 0:
            return self.capture_x_second(rtsp_url)  # try again
        return frames

    """
    This function can be called to change the password of a onvif camera.
    However not all camera allow this. So please check if this function works for your camera first.
    """
    def change_user(self,
        host: str,
        port: int,
        new_password: str,
        ):
        try:
            # Use the *current* creds (USERNAME/PASSWORD from your settings) to log in
            cam = ONVIFCamera(host, port, self.user, self.password)
            dev = cam.create_devicemgmt_service()  # or cam.devicemgmt

            users = dev.GetUsers()  # list of User objects

            # Find the admin user you want to update
            target_user = None
            for u in users:
                # u.Username is usually a string, e.g. "admin"
                if u.Username == self.user:
                    target_user = u
                    break

            if target_user is None:
                raise RuntimeError(f"Admin user '{self.user}' not found on device")

            # Change only what you need: the password (and keep it Administrator)
            target_user.Password = new_password
            target_user.UserLevel = 'Administrator'  # just to be explicit

            # Push the change back to the camera
            # Depending on the library, both of these usually work:
            dev.SetUser({'User': [target_user]})
            # or:
            # dev.SetUser(User=[target_user])

            print(f"Password for '{self.user}' updated successfully")
            return True
        except Exception as e:
            print(f"[ONVIF] Failed to update users on {host}:{port}: {e}")
            return False

    def see_network_interfaces(self, host, port):
        cam = ONVIFCamera(host, port, self.user, self.password)
        dev = cam.create_devicemgmt_service()  # or cam.devicemgmt
        interfaces = dev.GetNetworkInterfaces()
        print(interfaces)
        return interfaces


