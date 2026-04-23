# wifi.py
#
# Simple WiFi helper class for MicroPython.
# - Connect with SSID/password (optionally from config.py)
# - Check connection status
# - Control a WiFi LED
# - Reconnect if disconnected

import network
from machine import Pin
import utime as time



class WiFi:
    def __init__(self, wifi_pin=None):
        """
        :param wifi_pin: optional LED pin number to indicate WiFi status.
        """
        self.wlan = network.WLAN(network.STA_IF)
        self.wlan.active(True)

        self._led = None
        if wifi_pin is not None:
            self._led = Pin(wifi_pin, Pin.OUT)
            self._led.value(1)

        # store last used credentials for reconnect()
        self._ssid = None
        self._password = None

    # Connection handling
    def connect(self, ssid=None, password=None, max_wait=10):
        """
        Connect to WiFi.
        If ssid/password are None and a config module is present,
        tries config.WIFI_SSID / config.WIFI_PASSWORD.
        """
        # fallback to config.py if available
        if ssid is None and config is not None:
            ssid = getattr(config, "WIFI_SSID", None)
            password = getattr(config, "WIFI_PASSWORD", None)

        self._ssid = ssid
        self._password = password

        if not ssid:
            print("WiFi: no SSID provided")
            return False

        print("WiFi: connecting to", self._ssid)
        self.wlan.active(True)
        self.wlan.connect(self._ssid, self._password)

        while max_wait > 0:
            if self.check_connect():
                break
            max_wait -= 1
            time.sleep(1)

        # update LED once after connect attempt
        self._update_led()

        if self.check_connect():
            print("WiFi: connected, ifconfig:", self.wlan.ifconfig())
            return True
        else:
            print("WiFi: connection failed")
            return False

    def check_connect(self):
        """
        Return True if WLAN is connected.
        """
        return self.wlan.isconnected()
    
    def disconnect(self):
        self.wlan.disconnect()

    # LED handling
    def _update_led(self):
        """
        Internal helper: turn LED on/off according to connection state.
        """
        if self._led is None:
            return
        if self.check_connect():
            self._led.value(1)  # on
        else:
            self._led.value(0)  # off

    def wifi_LED(self, wifi_pin=None):
        """
        Backwards-compatible method name from your original file.
        If wifi_pin is provided and LED not yet initialised, set it.
        Then update LED based on current connection state.
        """
        if wifi_pin is not None and self._led is None:
            self._led = Pin(wifi_pin, Pin.OUT)
        self._update_led()
    
    def blink(self, timer):
        self._led.value(not self._led.value()) # Toggle current state
        
    def led_off(self):
        self._led.value(0)
        
    def led_on(self):
        self._led.value(1)
        
    # Reconnect
    def reconnect(self, max_wait=10):
        """
        Non-blocking reconnect attempt for use in async code.
        Returns True if already connected, False otherwise.
        Does NOT wait in a loop.
        """
        # Already connected?
        if self.check_connect():
            self._update_led()
            return True

        print("WiFi: reconnecting (non-blocking)...")
        self.wlan.active(True)
        self.wlan.connect(ssid, password)

        # wifi_task() will see the state change via check_connect().
        self._update_led()
        return False
    
    # Small helpers
    def ifconfig(self):
        """
        Return interface configuration (IP, mask, gw, DNS).
        """
        return self.wlan.ifconfig()