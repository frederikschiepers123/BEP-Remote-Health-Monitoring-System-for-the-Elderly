#SBC/device/__run_client__.py
import time
import socket
import os
from .Client import SBCClient
from .config import *
from .led_control import LED
import sys


def custom_getaddrinfo(host, port, *args, **kwargs): 
    """Resolve hostname to the HOST_MAP rather than the normal DNS resolution."""
    if host in HOST_MAP:
        host = HOST_MAP[host]   
    return _real_getaddrinfo(host, port, *args, **kwargs)


if __name__ == "__main__":
    work_dir = os.getcwd()
    _real_getaddrinfo = socket.getaddrinfo
    
    socket.getaddrinfo = custom_getaddrinfo # Configure socket to use hosts from HOST_MAP

    try:
        device_uuid = sys.argv[1]  # device UUID passed as argument
    except IndexError:
        device_uuid = None
    try:
        led_type = sys.argv[2]  # LED type passed as argument
    except IndexError:
        led_type = "builtin"
    
    c = SBCClient(led=LED("work", led_type), dev_uuid=device_uuid) # Use the specified LED type
    while c.finished is False: # Keep running until finished
        time.sleep(1)