from machine import Pin

# --- Radar drivers ---
from hmmd_mpy import HMMD
from DFRobot_HumanDetection_mpy import (
    DFRobot_HumanDetection, SleepMode, eHumanPresence, eHumanMovement, eHumanMovingRange, eHumanDistance
)
import time

class RadarWrapper:
    def __init__(self, tx_pin_df, rx_pin_df, tx_pin_hm, rx_pin_hm, uart_id):
        self.tx_pin_df = tx_pin_df
        self.rx_pin_df = rx_pin_df
        self.tx_pin_hm = tx_pin_hm
        self.rx_pin_hm = rx_pin_hm
        self.uart_id = uart_id
        self.kind = None          # "DFRobot" or "HMMD"
        self.dev = None
        self._df_poll = 0
        self._df_cache = {}
    #-------- generic --------
        self.presence = None
        self.movement = None
    #-------- C1001 ----------    
        self.movement_range = None
        self.heart = None
        self.breath = None
    #--------- HMMD ----------
        self.motion = None
        self.distance_m = None
        self.distance_raw = None
        self.existance_energy = None
        
        
    def detect(self, sensor):
        self.kind = sensor

        if self.kind == "HMMD":
            self.probe_HMMD()
        if self.kind == "DFRobot":
            self.probe_dfrobot()
        if self.kind == None:
            self.probe_HMMD()
        if self.kind == None:
            self.probe_dfrobot()
            

    def detected(self):
        return self.kind
    
    def probe_dfrobot(self):
        try:
            df = DFRobot_HumanDetection(
                uart_id=self.uart_id,
                baudrate=115200,
                tx=Pin(self.tx_pin_df),
                rx=Pin(self.rx_pin_df),
                timeout_s=0.15
            )
            print("Probing DFRobot...")
            ok = df.begin()   # 0 = success
            if ok == 0:
                df.config_work_mode(SleepMode)
                self.kind = "DFRobot"
                self.dev = df
                print("Detected DFRobot")
                return True
            else:
                self.kind = None
        except Exception as e:
            print("DFRobot probe error:", e)


    def probe_HMMD(self):
        try:
            hm = HMMD(
                uart_id=self.uart_id,
                baudrate=115200,
                tx=Pin(self.tx_pin_hm),
                rx=Pin(self.rx_pin_hm),
                timeout_ms=200
            )
            print("Probing HMMD...")
            if hm.begin():
                self.kind = "HMMD"
                self.dev = hm
                print("Detected HMMD")
                return True
            else:
                self.kind = None
        except Exception as e:
            print("HMMD probe error:", e)

    def read(self):
        out = {
            "source": self.kind,
        }
        # ---------- HMMD branch ----------
        if self.kind == "HMMD":
            frame = self.dev.read_frame()
            if not frame:
                return None
            
            presence  = frame.get("presence")
            energies = frame.get("energies") or []
            existance_energy = max(energies) if energies else None
            motion_energy = int(sum(energies) / len(energies)) if energies else None

            distance_raw = frame.get("distance_raw")
            distance_m = self.dev.distance_to_meters(distance_raw)

            movement = self.dev.compute_motion_state(frame)
            
            self.update_field("presence", presence, out, "presence")
            self.update_field("movement", movement, out, "movement")
            self.update_field("motion", motion_energy, out, "motion")
            self.update_field("distance_m", distance_m, out, "distance_m")
            self.update_field("distance_raw", distance_raw, out, "distance_raw")
            self.update_field("existance_energy", existance_energy, out, "existance_energy")

        # ---------- DFRobot branch ----------
        elif self.kind == "DFRobot":
            self._df_poll = (self._df_poll + 1) & 0x7fffffff

            # fast signals (every call)
            presence = self.dev.sm_human_data(eHumanPresence)
            movement = self.dev.sm_human_data(eHumanMovement)

            # slow (every 20 calls ~2s at 10Hz)
            if (self._df_poll % 20) == 0:
                self._df_cache["heart_bpm"] = self.dev.get_heart_rate()
                self._df_cache["breath_rpm"] = self.dev.get_breathe_value()

            # use cached values (may be missing at start)
            heart = self._df_cache.get("heart_bpm")
            breath = self._df_cache.get("breath_rpm")

            self.update_field("presence", presence, out, "presence")
            self.update_field("movement", movement, out, "movement")
            if heart is not None:
                self.update_field("heart", heart, out, "heart_bpm")
            if breath is not None:
                self.update_field("breath", breath, out, "breath_rpm")
        if len(out) > 1:
            return out
        return None

    def close(self):
        # Nothing special needed for DFRobot or HMMD here,
        # but keep the method for compatibility with main.py.
        pass
# ---------- helper -----------    
    def update_field(self, field_name, new_value, out_dict, key_name):
        """
        Updates self.<field_name> with new_value.
        Adds {key_name: new_value} to out_dict ONLY if changed.
        """
        old_value = getattr(self, field_name)

        if old_value != new_value:
            # store change flag or logging HERE if needed
            out_dict[key_name] = new_value
            setattr(self, field_name, new_value)  # store new value

            return True  # changed
        return True  # unchanged
