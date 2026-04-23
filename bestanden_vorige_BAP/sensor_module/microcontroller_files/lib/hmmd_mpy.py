# hmmd_mpy.py
#
# MicroPython driver for Waveshare HMMD mmWave Sensor in "report mode".
# Styled similarly to the DFRobot_HumanDetection_mpy driver:
#   - class with begin()
#   - internal UART-based state machine
#
# Protocol (Report Mode, Waveshare docs):
#   Header  : F4 F3 F2 F1
#   Length  : 2 bytes (little-endian)
#   Data:
#       presence  : 1 byte (0x00 = no target, 0x01 = target)
#       distance  : 2 bytes (little-endian, raw units)
#       energies  : 32 bytes = 16 × 2-byte gate energies
#   Tail    : F8 F7 F6 F5
#
# Mode switching command (set to Report Mode):
#   FD FC FB FA  08 00 12 00 00 00 04 00 00 00 04 03 02 01
#
from machine import UART
import time


class HMMD:
    # Command to switch to Report Mode
    _CMD_MODE_REPORT = b"\xFD\xFC\xFB\xFA\x08\x00\x12\x00\x00\x00\x04\x00\x00\x00\x04\x03\x02\x01"

    _HEADER = b"\xF4\xF3\xF2\xF1"
    _TAIL   = b"\xF8\xF7\xF6\xF5"

    def __init__(self, uart_id, baudrate=115200, tx=None, rx=None, timeout_ms=500):
        """
        uart_id   : UART index on your MCU
        baudrate  : should be 115200 for HMMD
        tx, rx    : TX/RX pins connected to the HMMD sensor
        timeout_ms: read timeout for a single frame
        """
        self.uart = UART(uart_id, baudrate=baudrate, tx=tx, rx=rx)
        self.timeout_ms = timeout_ms
        self._clear_uart()

    # ---------- internal helpers ----------

    def _clear_uart(self):
        """Clear pending bytes from UART input buffer."""
        if self.uart.any():
            self.uart.read()

    def _send_cmd(self, cmd_bytes):
        """Send a full binary command to the sensor."""
        self._clear_uart()
        self.uart.write(cmd_bytes)
        time.sleep_ms(200)

    def _read_bytes(self, n, timeout_ms=None):
        """Read exactly n bytes or return None on timeout."""
        if timeout_ms is None:
            timeout_ms = self.timeout_ms

        buf = bytearray()
        start = time.ticks_ms()

        while len(buf) < n and time.ticks_diff(time.ticks_ms(), start) < timeout_ms:
            if self.uart.any():
                b = self.uart.read(1)
                if b:
                    buf += b
            else:
                time.sleep_ms(2)

        return buf if len(buf) == n else None

    def _find_header(self):
        """Search the UART stream for the HMMD report header F4 F3 F2 F1."""
        header = self._HEADER
        idx = 0
        start = time.ticks_ms()

        while time.ticks_diff(time.ticks_ms(), start) < self.timeout_ms:
            if not self.uart.any():
                time.sleep_ms(2)
                continue

            b = self.uart.read(1)
            if not b:
                continue
            b = b[0]

            if b == header[idx]:
                idx += 1
                if idx == len(header):
                    return True
            else:
                idx = 1 if b == header[0] else 0

        return False

    # ---------- public API ----------

    def begin(self):
        """
        Initialize the sensor:
          - switch to Report Mode
          - read at least one valid frame

        Returns:
          True  on success
          False on failure
        """
        # switch to report mode
        self._send_cmd(self._CMD_MODE_REPORT)

        # try a handful of frames
        for _ in range(5):
            frame = self.read_frame()
            if frame is not None and frame.get("presence") in (0, 1):
                return True
        return False

    def read_frame(self):
        """
        Read a single report frame from HMMD.

        Returns (on success) a dict:
          {
            "presence": 0 or 1,
            "distance_raw": int,
            "energies": [16 ints],
          }
        or None on timeout / invalid frame.
        """
        # 1) Wait for header
        if not self._find_header():
            return None

        # 2) Length (2 bytes, little-endian)
        length_bytes = self._read_bytes(2)
        if length_bytes is None:
            return None
        length = length_bytes[0] | (length_bytes[1] << 8)

        # 3) Payload
        payload = self._read_bytes(length)
        if payload is None or len(payload) < 3:
            return None

        # 4) Tail
        tail = self._read_bytes(4)
        if tail is None or tail != self._TAIL:
            return None

        # 5) Parse payload
        presence = payload[0]
        distance_raw = payload[1] | (payload[2] << 8)

        energies = []
        for i in range(16):
            start_i = 3 + 2 * i
            end_i = start_i + 2
            if end_i > len(payload):
                break
            val = payload[start_i] | (payload[start_i + 1] << 8)
            energies.append(val)

        return {
            "presence": presence,
            "distance_raw": distance_raw,
            "energies": energies,
        }

    def distance_to_meters(self, distance_raw, gate_length_m=0.7):
        """
        Roughly convert raw distance into meters.
        This factor is an approximation based on distance gate spacing.
        """
        if distance_raw is None:
            return None
        return distance_raw * gate_length_m
    
    def compute_motion_state(self, frame, lower_threshold=20000, upper_threshold=65000): # ??? tweak these values maybe add some smoothing or based on average energies
        """
        Hysteresis-based motion classifier for HMMD.

        Motion state:
          1 = still
          2 = moving

        Logic:
          - When max_energy >= upper_threshold → state becomes 2 (moving)
          - When max_energy <= lower_threshold → state becomes 1 (still)
          - Between thresholds → keep previous state

        - frame: dict returned by read_frame()
        """
        # Sanity: make sure thresholds are ordered
        if lower_threshold >= upper_threshold:
            raise ValueError("lower_threshold must be < upper_threshold")

        # If we didn't get a frame, just keep the last state
        if frame is None:
            return self.motion_state

        presence = frame.get("presence", 0)
        energies = frame.get("energies") or []

        # No target or no energies → treat as still
        if presence == 0 or not energies:
            self.motion_state = 1
            return self.motion_state

        max_energy = max(energies)

        # Hysteresis:
        if max_energy >= upper_threshold:
            # Strong motion → switch to moving
            self.motion_state = 2
        elif max_energy <= lower_threshold:
            # Very low energy → switch to still
            self.motion_state = 1
        else:
            # Between lower & upper → keep previous state
            pass

        return self.motion_state