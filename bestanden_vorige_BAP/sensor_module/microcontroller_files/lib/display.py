"""
File Name: sensor/display.py
Purpose  : Render data to LilyGO T-QT Pro display.
Version  : 1.2
"""

import time
import font
from machine import Pin

DISPLAY_AVAILABLE = False
try:
    import tft_config
    DISPLAY_AVAILABLE = True
except Exception as e:
    print("Display: tft_config not available:", e)


class Display:
    """
    Pages:
      0 - Environment (temp, hum, pressure, etc.)
      1 - Info (location, id, wifi RSSI, firmware version)
      2 - Time (RTC set via boot.set_time())
      3 - Radar (presence / motion / distance)
    """

    def __init__(self, env, wifi=None, radar=None):
        """
        env   : environment wrapper (must have .temp .hum .pres)
        wifi  : WiFi helper (optional, for RSSI)
        radar : radar wrapper (optional)
        """
        self.env = env
        self.wifi = wifi
        self.radar = radar

        self._tft = None
        self._current_page = 0
        self._page_functions = [
            self.page_env,
            self.page_info,
            self.page_time,
            self.page_radar,
        ]

        if not DISPLAY_AVAILABLE:
            return

        try:
            self._tft = tft_config.config(tft_config.WIDE)
            self._tft.init()

            # Page navigation buttons
            self._next_pin = Pin(47, Pin.IN, Pin.PULL_UP)
            self._prev_pin = Pin(0, Pin.IN, Pin.PULL_UP)
            self._next_pin.irq(trigger=Pin.IRQ_FALLING, handler=self.next_page)
            self._prev_pin.irq(trigger=Pin.IRQ_FALLING, handler=self.prev_page)

            # Draw initial page
            self._render_current_page()
        except Exception as e:
            print("Display init error:", e)
            self._tft = None

    def __del__(self):
        if DISPLAY_AVAILABLE and self._tft:
            try:
                tft_config.deinit(self._tft, True)
            except Exception as e:
                print("Display deinit error:", e)

    # ---------- helpers ----------

    def _ptsw(self, prefix, suffix):
        """Pad a string to 11 characters (screen width in chars)."""
        width = 11
        prefix = str(prefix)
        suffix = str(suffix)
        space = width - len(prefix) - len(suffix)
        if space <= 0:
            return prefix + suffix
        return prefix + " " * space + suffix

    def _render_current_page(self):
        """Clear screen and render the current page."""
        if not self._tft:
            return
        tft_config.backlight_on()
        self._tft.fill(0)
        self._page_functions[self._current_page]()
        self._tft.show()

    def print_line(self, text, row):
        """Print a single line at row 0..N."""
        if not self._tft:
            return
        text = str(text)
        column = 0
        for char in text:
            try:
                idx = font.MAP.index(char)
            except ValueError:
                idx = 0  # unknown char -> first glyph
            self._tft.bitmap(font, column, row * font.HEIGHT, idx)
            column += font.WIDTH
            if column >= self._tft.width() - font.WIDTH:
                break

    # ---------- pages ----------

    def page_env(self):
        """Environment page."""
        if not self._tft:
            return
        co2 = 400  # placeholder until you wire in real values
        aud = 35   # placeholder

        temp = getattr(self.env, "temp", None)
        hum = getattr(self.env, "hum", None)
        pres = getattr(self.env, "pres", None)

        self.print_line(self._ptsw("Tmp:", f"{temp}C"), 0)
        self.print_line(self._ptsw("Hum:", f"{hum}%"), 1)
        self.print_line(self._ptsw("CO2:", f"{co2}ppm"), 2)
        self.print_line(self._ptsw("Prs:", f"{int(pres)}hPa"), 3)
        self.print_line(self.__ptsw("aqi:", f"{self.env.aqi}"), 4)
        
    def page_info(self):
        """Sensor info + WiFi."""
        if not self._tft:
            return

        wifi_txt = "nowifi"
        if self.wifi is not None:
            try:
                wlan = getattr(self.wifi, "wlan", None)
                if wlan and wlan.isconnected():
                    try:
                        wifi_txt = str(wlan.status("rssi"))
                    except Exception:
                        wifi_txt = "OK"
            except Exception as e:
                print("Display WiFi RSSI error:", e)

        self.print_line(self._ptsw(config.location, ""), 0)
        self.print_line(self._ptsw("", ""), 1)
        self.print_line(self._ptsw("ID:", str(config.sensor_id)), 2)
        self.print_line(self._ptsw("Wifi:", wifi_txt), 3)
        self.print_line(self._ptsw("V:", config.version), 4)

    def page_time(self):
        """Local date & time (from RTC set in boot.set_time())."""
        if not self._tft:
            return

        y, m, d, hh, mm, ss, wd, yd = time.localtime()
        date_str = f"{d:02d}-{m:02d}"
        year_str = f"{y % 100:02d}"
        time_str = f"{hh:02d}:{mm:02d}"
        weekday_names = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
        day_str = weekday_names[wd] if 0 <= wd < 7 else str(wd)

        self.print_line(self._ptsw("Date:", date_str), 0)
        self.print_line(self._ptsw("Year:", year_str), 1)
        self.print_line(self._ptsw("Time:", time_str), 2)
        self.print_line(self._ptsw("Day:", day_str), 3)
        self.print_line(self._ptsw("", ""), 4)

    def page_radar(self):
        """Radar status page."""
        if not self._tft:
            return

        if self.radar is None:
            self.print_line(self._ptsw("Radar:", "none"), 0)
            return

        state = None

        # Preferred: radar.read() -> dict or value
        if hasattr(self.radar, "read"):
            try:
                state = self.radar.read()
            except Exception as e:
                print("Display radar read error:", e)

        # Fallback: build dict from common attribute names
        if state is None:
            state = {}
            for name in (
                "presence",
                "human",
                "target",
                "move",
                "motion",
                "distance",
                "distance_cm",
                "range",
            ):
                if hasattr(self.radar, name):
                    state[name] = getattr(self.radar, name)

        if not isinstance(state, dict):
            self.print_line(self._ptsw("Radar:", str(state)), 0)
            return

        presence = (
            state.get("presence")
            or state.get("human")
            or state.get("target")
            or state.get("state")
        )
        motion = state.get("motion") or state.get("move") or state.get("activity")
        distance = (
            state.get("distance")
            or state.get("distance_cm")
            or state.get("range")
        )

        self.print_line(self._ptsw("Radar:", ""), 0)
        self.print_line(self._ptsw("Pres:", str(presence)), 1)
        self.print_line(self._ptsw("Move:", str(motion)), 2)
        self.print_line(self._ptsw("Dist:", str(distance)), 3)
        self.print_line(self._ptsw("", ""), 4)

    # ---------- navigation (button IRQs) ----------

    def next_page(self, pin):
        """IRQ handler: go to next page."""
        if not self._tft or pin.value() != 0:
            return
        if self._current_page >= len(self._page_functions) - 1:
            self._current_page = 0
        else:
            self._current_page += 1
        self._render_current_page()

    def prev_page(self, pin):
        """IRQ handler: go to previous page."""
        if not self._tft or pin.value() != 0:
            return
        if self._current_page <= 0:
            self._current_page = len(self._page_functions) - 1
        else:
            self._current_page -= 1
        self._render_current_page()

