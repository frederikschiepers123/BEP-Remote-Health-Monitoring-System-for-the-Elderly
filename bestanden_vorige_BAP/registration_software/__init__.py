# src/__init__.py
from __future__ import annotations
import socket
import os
import time 

from registration_software.common.config import settings, HOST_MAP
from registration_software.identification import get_tracker
from registration_software.registration import CAManager, DeviceRegistry, RegistrationClient
from registration_software.tools.calibrate import run_camera_calibration_tool
from registration_software.identification.passive_identification import Cam_register
import sys


def camera_registration(manual):
    """Initializes dependencies and runs the """

    while True:
        cameras = input(f'Are all the secondary cameras on a straight line facing the main camera?(y/n)')
        if cameras.lower() == "y":
            break
        elif cameras.lower() == "n":
            print(f'Please place all the secondary cameras on a straight line facing the main camera')
            continue
        else:
            print(f'Please answer using only y or n.')
            continue

    det = Cam_register(save_results=manual)
    det.start()

def peripheral_registration(manual: bool = False, auto: bool = False, camera: bool = True) -> None:
    """Initializes dependencies and runs the two-phase registration process."""
    print("\nStarting peripheral registration process...\n")

    ca_manager = CAManager(ca_cert_path=settings.paths.ca_cert, ca_key_path=settings.paths.ca_key)
    data_registry = DeviceRegistry(registry_path=settings.paths.registry)
    if camera is True:
        tracker = get_tracker()
        tracker.start()
        while tracker.visualizer.is_alive() is False:
            time.sleep(0.1)

        input("Press enter to continue with peripheral registration...\n")
        # Initialize and run the Enrollment Server
        registration_client = RegistrationClient(
            ca_manager=ca_manager, 
            data_registry=data_registry, 
            identifier=tracker, 
            manual=False,
            auto=False
        )
    else:
        # Initialize and run the Enrollment Server
        registration_client = RegistrationClient(
            ca_manager=ca_manager, 
            data_registry=data_registry, 
            identifier=None, 
            manual=manual,
            auto=auto
        )

    # Blocking call to run the full enrollment pipeline
    enrolled_labels = registration_client.start()

    if not enrolled_labels:
        return
    

def custom_getaddrinfo(host, port, *args, **kwargs):
    """Resolve hostname to the HOST_MAP first"""
    if host in HOST_MAP:
        host = HOST_MAP[host]   
    return _real_getaddrinfo(host, port, *args, **kwargs)

    
if __name__ == "__main__":

    _real_getaddrinfo = socket.getaddrinfo
    socket.getaddrinfo = custom_getaddrinfo
    try:
        type = sys.argv[1].lower()
    except IndexError:
        raise ValueError("Please provide registration type: 's' for peripheral, 'c' for camera, 'calibration' for camera calibration.")
    try:
        manual = True if sys.argv[2].lower() == "true" else False
    except IndexError:
        manual = False
    try:
        camera = True if sys.argv[3].lower() == "true" else False
    except IndexError:
        camera = True
    try:
        auto = True if sys.argv[4].lower() == "true" else False
    except IndexError:
        auto = False
    if type == 'c':
        camera_registration(manual=manual)
    elif type == 's':
        peripheral_registration(camera=camera, manual=manual, auto=auto)
    elif type == 'calibration':
        run_camera_calibration_tool()
    elif type == 'tracker':
        tracker = get_tracker()
        tracker.start()
        while tracker.visualizer.is_alive() is False:
            time.sleep(0.1)
        input("Press enter to exit...\n")
    else:
        raise ValueError("Invalid registration type. Use 's' for peripheral, 'c' for camera, 'calibration' for camera calibration.")