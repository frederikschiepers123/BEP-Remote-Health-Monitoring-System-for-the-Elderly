from machine import ADC, Pin
import math

class GL5516:
    """
    GL5516 LDR driver for ESP32-S3

    """

    def __init__(self, pin, r_fixed=10_000, v_supply=3.25, adc_vref=3.3):
        adc_pin = Pin(pin)
        self.adc = ADC(adc_pin)

        # ESP32-S3 ADC range setting — REQUIRED
        self.adc.atten(ADC.ATTN_11DB)   # ~0–3.3V
        self.adc.width(ADC.WIDTH_12BIT)

        # microPython returns scaled-to-12-bit values
        self.adc_max = 4095

        self.r_fixed = r_fixed
        self.v_supply = v_supply
        self.adc_vref = adc_vref

    def read_raw(self):
        return self.adc.read()

    def read_voltage(self):
        raw = self.read_raw()
        return (raw / self.adc_max) * self.adc_vref

    def get_resistance(self):
        """
        Using:
           Vout = Vsupply * (R_fixed / (R_LDR + R_fixed))
        Solve:
           R_LDR = R_fixed * (Vout / (Vsupply - Vout))
        """
        v_out = self.read_voltage()

        if v_out <= 0:
            return float("inf")

        if v_out >= self.adc_vref:
            return None  # ADC saturated

        return self.r_fixed * ((self.v_supply - v_out)/v_out)

    def get_lux(self, r10_ohm=10000, gamma=0.5):
        """
        Approximate lux from GL5516 resistance.

        r10_ohm: resistance at 10 lux (ohms)
        gamma:   datasheet gamma (~0.5 for GL5516)
        """
        r = self.get_resistance()
        if r is None or r <= 0:
            return None  # or float("inf"), depending on how you want to handle it

        return 10 * (r10_ohm / r) ** (1.0 / gamma)
        
    def read(self, ldr, light_cfg):
        """
        Read GL5516 and return a small dict or None if it fails.
        """
        if ldr is None:
            return None

        try:
            lux = self.get_lux()
            # Guard against weird values like inf / None
            if not isinstance(lux, (int, float)):
                return None
            if lux < 0 or lux > 1_000_000:
                return None

            # You can round if you want less noise
            lux = round(lux, 1)
            lux = self.round_step(lux, self.light_cfg)
            return {"lux": lux}
        except Exception as e:
            #print("Light sensor read error:", e)
            return None
        
    def round_step(self, value, step):
        """Round a numeric value to the nearest step (e.g., 0.5 or 5)."""
        if value is None:
            return None
        try:
            return round(value / step) * step
        except Exception:
            return None
