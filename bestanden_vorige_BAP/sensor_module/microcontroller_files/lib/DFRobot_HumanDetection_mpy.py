# -*- coding: utf-8 -*-
"""
DFRobot Human Detection - MicroPython (minimal build)
Only the parts needed for: presence, movement, heart rate, breathe rate.

Keep the public names so existing user code works:
- Class: DFRobot_HumanDetection
- Constants: SleepMode, eHumanPresence, eHumanMovement

Tested on ESP32-S3 MicroPython 1.22+
"""

from machine import UART, Pin
import utime
import uasyncio as asyncio

# -------- Public enums kept --------
SleepMode = 2          # 1 = FallMode, 2 = SleepMode (module firmware)
eHumanPresence = 1
eHumanMovement = 2
eHumanMovingRange = 3
eHumanDistance = 4

class DFRobot_HumanDetection:
    def __init__(self, uart_id=1, baudrate=115200, tx=16, rx=48, timeout_s=0.05):
        self.uart = UART(uart_id, baudrate=baudrate, tx=tx, rx=Pin(rx, Pin.IN, Pin.PULL_DOWN), timeout=5)
        self.timeout_ms = int(timeout_s * 1000)

    def begin(self):
        """
        Probe the sensor. Returns 0 on success, 1 on failure (DFRobot convention).
        """
        # Allow module to boot after power-up (many boards need several seconds).
        senData = bytearray([0x0f])
        resp = self._getData(0x01, 0x83, 1, senData)
        return 0 if resp and resp[0] != 0xf5 else 1

    def get_workmode(self):
        """
        Read current work mode.
        Returns 1 (FallMode) or 2 (SleepMode). Falls back to 2 if unknown/error.
        """
        senData = bytearray([0x0f])
        resp = self._getData(0x02, 0xA8, 1, senData)
        if resp and resp[0] != 0xf5:
            return resp[6]
        return 2

    def config_work_mode(self, mode):
        """
        Set work mode. Returns 0 on success, 1 on failure.
        """
        # If already in desired mode, nothing to do.
        senData = bytearray([0x0f])
        cur = self._getData(0x02, 0xA8, 1, senData)
        if cur and cur[0] != 0xf5 and cur[6] == (mode & 0xFF):
            return 0

        # Write: con=0x02, cmd=0x08, payload=[mode]
        self._send(0x02, 0x08, bytearray([mode & 0xFF]))
        asyncio.sleep_ms(150)

        # Verify
        cur2 = self._getData(0x02, 0xA8, 1, senData)
        if cur2 and cur2[0] != 0xf5 and cur2[6] == (mode & 0xFF):
            return 0
        return 1

    # -------- Data getters (sleep mode) --------
    def sm_human_data(self, what):
        """
        Sleep-mode human data:
          eHumanPresence -> 0/1
          eHumanMovement -> activity metric (module-defined)
        """
        senData = bytearray([0x0f])
        if what == eHumanPresence:
            resp = self._getData(0x80, 0x81, 1, senData)
        elif what == eHumanMovement:
            resp = self._getData(0x80, 0x82, 1, senData)
        elif what == eHumanMovingRange:
            resp = self._getData(0x80,0x83,1,senData)
        elif what == eHumanDistance:
            resp = self._getData(0x80,0x84,1,senData)            
        else:
            return 0
        if resp and resp[0] != 0xf5:
            return resp[6]
        return 0    

    def get_heart_rate(self):
        """Heart rate in BPM. 0xFF means 'not ready'."""
        senData = bytearray([0x0f])
        resp = self._getData(0x85, 0x82, 1, senData)
        if resp and resp[0] != 0xf5:
            return resp[6]
        return 0xFF

    def get_breathe_state(self):
        """
        Breathe state code: 1=normal, 2=fast, 3=slow, 4=none, 0xFF=unknown/not-ready.
        """
        senData = bytearray([0x0f])
        resp = self._getData(0x81, 0x81, 1, senData)
        if resp and resp[0] != 0xf5:
            return resp[6]
        return 0xFF

    def get_breathe_value(self):
        """Breath rate in RPM. 0xFF means 'not ready'."""
        senData = bytearray([0x0f])
        resp = self._getData(0x81, 0x82, 1, senData)
        if resp and resp[0] != 0xf5:
            return resp[6]
        return 0xFF

    # -------- Low-level helpers --------
    def _send(self, con, cmd, payload):
        length = len(payload)
        buf = bytearray(9 + length)
        buf[0] = 0x53
        buf[1] = 0x59
        buf[2] = con & 0xFF
        buf[3] = cmd & 0xFF
        buf[4] = (length >> 8) & 0xFF
        buf[5] = length & 0xFF
        if length:
            buf[6:6 + length] = payload
        buf[6 + length] = self._sumData(6 + length, buf)
        buf[7 + length] = 0x54
        buf[8 + length] = 0x43
        self._uart_clear()
        self.uart.write(buf)

    def _getData(self, con, cmd, length, senData):
        """
        Send a command and parse one reply frame.
        On timeout, ret[0] == 0xF5.
        """
        self._send(con, cmd, senData[:length] if length else b"")
        start = utime.ticks_ms()
        # State machine
        CMD_WHITE, CMD_HEAD, CMD_CONFIG, CMD_CMD, CMD_LEN_H, CMD_LEN_L, CMD_END_H, CMD_END_L, CMD_DATA = range(9)
        state = CMD_WHITE
        data_len = 0
        count = 0
        ret = bytearray(32)

        while True:
            if self.uart.any():
                b = self.uart.read(1)
                if not b:
                    continue
                ch = b[0]

                if state == CMD_WHITE:
                    if ch == 0x53:
                        ret[0] = ch
                        state = CMD_HEAD
                elif state == CMD_HEAD:
                    if ch == 0x59:
                        ret[1] = ch; state = CMD_CONFIG
                    else:
                        state = CMD_WHITE
                elif state == CMD_CONFIG:
                    if ch == (con & 0xFF):
                        ret[2] = ch; state = CMD_CMD
                    else:
                        state = CMD_WHITE
                elif state == CMD_CMD:
                    if ch == (cmd & 0xFF):
                        ret[3] = ch; state = CMD_LEN_H
                    else:
                        state = CMD_WHITE
                elif state == CMD_LEN_H:
                    ret[4] = ch
                    data_len = ch << 8
                    state = CMD_LEN_L
                elif state == CMD_LEN_L:
                    ret[5] = ch
                    data_len |= ch
                    need = 9 + data_len
                    if len(ret) < need:
                        ret = ret + bytearray(need - len(ret))
                    state = CMD_DATA
                    count = 0
                elif state == CMD_DATA:
                    if count < data_len:
                        ret[6 + count] = ch
                        count += 1
                    else:
                        ret[6 + data_len] = ch
                        state = CMD_END_H
                elif state == CMD_END_H:
                    ret[7 + data_len] = ch
                    state = CMD_END_L
                elif state == CMD_END_L:
                    ret[8 + data_len] = ch
                    asyncio.sleep_ms(20)
                    return ret

            if utime.ticks_diff(utime.ticks_ms(), start) > self.timeout_ms:
                ret[0] = 0xF5
                return ret
            
#-------------- fast presence detection --------------
    """
    The c1001 has a long presence hold time of about 30-60 seconds
    These functions implement presence detection based on movement parameters
    """
    
    

    @staticmethod
    def _sumData(length, buf):
        s = 0
        for i in range(length):
            s = (s + (buf[i] & 0xFF)) & 0xFF
        return s

    def _uart_clear(self):
        asyncio.sleep_ms(2)
        while self.uart.any():
            self.uart.read(self.uart.any())
            asyncio.sleep_ms(1)
