# SBC/device/__run_dep_client__.py
import time
import os
import socket
from .DepClient import DepClient
from .config import *
from .led_control import LED


def custom_getaddrinfo(host, port, *args, **kwargs): 
    """Resolve hostname to the HOST_MAP rather than the normal DNS resolution."""
    if host in HOST_MAP:
        host = HOST_MAP[host]   
    return _real_getaddrinfo(host, port, *args, **kwargs)


if __name__ == "__main__":
    work_dir = os.getcwd()
    _real_getaddrinfo = socket.getaddrinfo
    
    socket.getaddrinfo = custom_getaddrinfo # Configure socket to use hosts from HOST_MAP

    c = DepClient() # Run the deployment client 
    while True:
        try:
            time.sleep(1)   
        except KeyboardInterrupt:   # Break clean on KeyboardInterrupt
            c.stop()
            break