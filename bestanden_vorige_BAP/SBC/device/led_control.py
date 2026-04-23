import time
import threading

class LED():
    def __init__(self, name="work", mode="builtin"):
        self.work_dir = f"/sys/class/leds/{name}/"
        self._brightness = '0'
        self._state = "off"
        self.mode = mode  # "builtin" for SBC usage or "laptop" for testing on laptop
    
    @property
    def brightness(self):
        return self._brightness
    
    @brightness.setter
    def brightness(self, new_brightness):
        nb = int(new_brightness)
        if nb > 255:
            self._brightness = '255'
        elif nb < 0:
            self._brightness = '0'
        else:
            self._brightness = str(nb)

    @property
    def state(self):
        return self._state
    
    @state.setter
    def state(self, new_state: bool):   # setter, to make sure the new state is applied
        self._state = new_state
        if self.state == "blink":
            self.blink()
        elif self.state == "on":
            self.led_on()
        else:
            self.led_off()

    def led_on(self):
        self._led_control(self.brightness)

    def led_off(self):      
        self._led_control('0')

    def blink(self, speed=5):
        """Start non-blocking blink at speed (Hz)."""
        def _run():
            interval = 1/(2*speed)
            next_time = time.monotonic()    # Use monotonic time to avoid issues with system time changes
            led = True
            while self._state == "blink":   # Stop blinking when state changes
                next_time += interval   # Calculate next toggle time
                if led is True:
                    self.brightness= '255'
                    self.led_on()
                else:
                    self.led_off()
                led = not led
                sleep = next_time - time.monotonic()    # Calculate sleep time
                if sleep > 0:
                    time.sleep(sleep)
                else:
                    print("Warning: loop too slow")

        self._blink_thread = threading.Thread(target=_run, daemon=True) # Daemon thread to not block exit
        self._blink_thread.start()
    
    def _led_control(self, brightness='0'):
        if self.mode=="builtin":    # Builtin LED control, only control when using the builtin SBC LED
            with open(self.work_dir + 'brightness', "w") as f:
                f.write(brightness)
        return