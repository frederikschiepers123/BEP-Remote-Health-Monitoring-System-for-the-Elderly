# environment.py — wrapper around user's existing sensor drivers
# Uses:
#   - BME280 (BME280.py)  -> temp, humidity, pressure
#   - ENS160 (ens160.py) + AHT20 (ahtx0.py) -> AQI, TVOC, eCO2 + temp/humidity
#
# API:
#   env = Environment(i2c)
#   env.detected() -> "BME280" | "ENS160_AHT20" | None
#   env.read() -> dict:
#     {
#       "source": "...",
#       "temp_c": float|None,
#       "rh_pct": float|None,
#       "pressure_hpa": float|None,
#       "aqi": int|None,
#       "tvoc_ppb": int|None,
#       "eco2_ppm": int|None
#     }

from machine import I2C
import utime

# environment drivers
from ahtx0 import AHT20           # AHT20.read_data() -> (temp_c, rh_pct)
from BME280 import BME280         # has .temperature_raw, .humidity_raw, .pressure_raw
from ens160 import ENS160         # get_aqi(), get_tvoc(), get_eco2()

_ADDR_AHT20  = 0x38
_ADDR_ENS160 = 0x53
_ADDR_BME280_A = 0x76
_ADDR_BME280_B = 0x77
    

class Environment:
    def __init__(self, i2c: I2C, sensor):
        if not isinstance(i2c, I2C):
            raise ValueError("Environment expects a machine.I2C instance")
        self.i2c = i2c
        self.mode = sensor
        self.sensor = None   # either BME280 instance or tuple (ens160, aht20)
        self.last = None
        self.temp = 0
        self.hum = 0
        self.co2 = 0
        self.tvoc = 0
        self.pres = 0
        self.aqi = 0
                        
        if self.mode == "BME280":
            self.probe_BME()
        
        if self.mode == "ENS160_AHT20":
            self.probe_ENS()
        
        if self.mode == None:
            self.scan(self.i2c)

    def probe_BME(self, bme_addr = 118):
        print("probing BME...")
        try:
            bme = BME280(i2c=self.i2c, address=bme_addr)
            # quick read to confirm
            _ = bme.temperature_raw
            self.mode = "BME280"
            self.sensor = bme
        except Exception:
            # fall back to ENS160 + AHT20 if available
            self.mode = None
            self.sensor = None
                
    def probe_ENS(self):
        print("probing ENS...")
        try:
            ens = ENS160(self.i2c)
            aht = AHT20(self.i2c)
            utime.sleep_ms(20)
            self.mode = "ENS160_AHT20"
            self.sensor = (ens, aht)
        except Exception:
            self.mode = None
            self.sensor = None
    
    def scan(self, i2c):
        # scan bus
        try:
            addrs = i2c.scan()
        except Exception:
            addrs = []

        # Prefer BME280 if present (0x76/0x77)
        bme_addr = None
        if _ADDR_BME280_A in addrs:
            bme_addr = _ADDR_BME280_A
        elif _ADDR_BME280_B in addrs:
            bme_addr = _ADDR_BME280_B

        if bme_addr is not None:
            self.probe_BME(bme_addr)
        if self.mode is None and (_ADDR_ENS160 in addrs) and (_ADDR_AHT20 in addrs):
            self.probe_ENS()
                
    def detected(self):
        return self.mode

    def read(self, cfg_env):
        out = {
            "source": self.mode
        }

        if self.mode == "BME280":
            bme = self.sensor
            try:
                temp = self.round_step(float(bme.temperature_raw), cfg_env["temp_res"]) 
                self.update_field("temp", temp, out, "temp_c")
            except Exception:
                temp = None
            try:
                hum = self.round_step(float(bme.humidity_raw), cfg_env["hum_res"])
                self.update_field("hum", hum, out, "rh_pct")
            except Exception:
                hum = None
            try:
                pres = self.round_step(float(bme.pressure_raw), cfg_env["pres_res"])
                self.update_field("pres", pres, out, "pressure_hpa")
            except Exception:
                pres = None

        elif self.mode == "ENS160_AHT20":
            ens, aht = self.sensor
            # Read AHT20 first, then read ENS160 (user ENS160 driver doesn't support compensation API)
            try:
                t, rh = aht.read_data()
                t = self.round_step(t, cfg_env["temp_res"])
                rh = self.round_step(rh, cfg_env["hum_res"])
                
            except Exception:
                t, rh = (None, None)
                
            self.update_field("temp", t, out, "temp_c")
            self.update_field("hum", rh, out, "rh_pct")
            
            try:
                aqi = int(ens.get_aqi())
                self.update_field("aqi", aqi, out, "aqi")
            except Exception:
                pass
            try:
                tvoc = int(ens.get_tvoc())
                self.update_field("tvoc", tvoc, out, "tvoc_ppb")
            except Exception:
                pass
            try:
                co2 = self.round_step(int(ens.get_eco2()), cfg_env["co2_res"])
                self.update_field("co2", co2, out, "co2_ppm")
            except Exception:
                pass
            
        if out != {"source": self.mode}:
            return out
        else:
            return None
    
# ----------- helpers ---------------    
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
        return False  # unchanged

    def round_step(self, value, step):
        """Round a numeric value to the nearest step (e.g., 0.5 or 5)."""
        if value is None:
            return None
        try:
            return round(value / step) * step
        except Exception:
            return None