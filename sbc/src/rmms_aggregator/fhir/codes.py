"""LOINC / SNOMED CT / UCUM coding tables (CLAUDE.md §8.2).

This is the single source of truth for sensor-field → code mapping. New sensor
fields are added HERE (plus a unit test), nowhere else. Pure module — stdlib
only, so the mapping is unit-testable without the FHIR libraries.

Requirement split (firmware repo's loss-tolerance/idempotency requirements):
  - **Medical/vital fields use LOINC**, strictly, in Observation.code.
  - **Ambient fields use LOINC where a code exists, else SNOMED CT** (or a
    documented custom `urn:rmms:*` system as an explicit placeholder until a
    real code is assigned — never an invented LOINC).

`confirmed=False` marks a code that needs clinical-advisor sign-off before any
deployment beyond project review (we flag, we do not invent — §20).
"""
from __future__ import annotations

from dataclasses import dataclass

LOINC = "http://loinc.org"
SNOMED = "http://snomed.info/sct"
UCUM = "http://unitsofmeasure.org"
# Explicit placeholder system for fields with no agreed LOINC/SNOMED yet.
RMMS_PLACEHOLDER = "urn:rmms:obs-code"

VALUE_QUANTITY = "quantity"
VALUE_INTEGER = "integer"
VALUE_BOOLEAN = "boolean"

# FHIR observation-category codes (http://terminology.hl7.org/CodeSystem/observation-category)
CAT_VITAL = "vital-signs"
CAT_SURVEY = "survey"
CAT_ACTIVITY = "activity"


@dataclass(frozen=True, slots=True)
class CodeSpec:
    obs_key: str          # identifier discriminator, e.g. "heart" (see identifiers.py)
    code_system: str      # LOINC / SNOMED / placeholder
    code: str
    display: str
    value_kind: str       # VALUE_QUANTITY | VALUE_INTEGER | VALUE_BOOLEAN
    unit: str | None      # human unit label (None for boolean)
    ucum_code: str | None # UCUM code if standard, else None (annotated unit only)
    category: str
    klass: str            # "medical" | "ambient" | "device"
    confirmed: bool       # False → needs clinical sign-off before deployment
    scale: float = 1.0    # multiply the raw value (e.g. 0.1 for hPa → kPa)


# Keyed by the sensor-sample FIELD name (matches domain.sample attribute names).
CODES: dict[str, CodeSpec] = {
    # ── Medical / vital signs → strictly LOINC ──────────────────────────────
    "heart_bpm": CodeSpec(
        "heart", LOINC, "8867-4", "Heart rate",
        VALUE_QUANTITY, "/min", "/min", CAT_VITAL, "medical", confirmed=True),
    "breath_bpm": CodeSpec(
        "breath", LOINC, "9279-1", "Respiratory rate",
        VALUE_QUANTITY, "/min", "/min", CAT_VITAL, "medical", confirmed=True),
    "presence": CodeSpec(
        # NO verified LOINC for person-presence/occupancy. (LOINC 76689-9 in the
        # old §8.2 table is "Sex assigned at birth" — a real-but-WRONG code; never
        # ship that.) Placeholder until a clinician assigns a correct LOINC/SNOMED.
        "presence", RMMS_PLACEHOLDER, "presence", "Person presence detected",
        VALUE_BOOLEAN, None, None, CAT_ACTIVITY, "medical", confirmed=False),

    # ── Ambient (environment) → LOINC where a VERIFIED one exists, else flagged ─
    "temp_c": CodeSpec(
        # Ambient ROOM temperature. LOINC 8310-5 is *body* temperature — coding
        # room temp with it would misrepresent it as a vital. Placeholder until a
        # verified environmental-temperature code is chosen.
        "temp", RMMS_PLACEHOLDER, "ambient_temp_c", "Ambient temperature",
        VALUE_QUANTITY, "Cel", "Cel", CAT_SURVEY, "ambient", confirmed=False),
    "hum_pct": CodeSpec(
        "humidity", LOINC, "19736-7", "Relative humidity in environment",
        VALUE_QUANTITY, "%", "%", CAT_SURVEY, "ambient", confirmed=True),
    "pres_hpa": CodeSpec(
        # LOINC 3140-1 in the old §8.2 table is "Body surface area" (m²) — WRONG.
        # Placeholder reporting raw hPa until a verified atmospheric-pressure code
        # (and its canonical unit/scale) is assigned.
        "pressure", RMMS_PLACEHOLDER, "atmospheric_pressure_hpa", "Atmospheric pressure",
        VALUE_QUANTITY, "hPa", "hPa", CAT_SURVEY, "ambient", confirmed=False),

    # ── Ambient air quality → no widely-adopted LOINC: placeholder, flagged ──
    # The Radxa team must assign a LOINC or SNOMED CT code (with clinical
    # sign-off) before deployment. The URN makes the placeholder explicit; it is
    # NOT a fake LOINC.
    "co2_ppm": CodeSpec(
        # UCUM unit is coded ([ppm]) even though the Observation.code is still a
        # placeholder — the measurement unit is independent of the code system.
        "co2", RMMS_PLACEHOLDER, "co2_ppm", "Indoor equivalent CO2",
        VALUE_INTEGER, "ppm", "[ppm]", CAT_SURVEY, "ambient", confirmed=False),
    "tvoc_ppb": CodeSpec(
        "tvoc", RMMS_PLACEHOLDER, "tvoc_ppb", "Total volatile organic compounds",
        VALUE_INTEGER, "ppb", "[ppb]", CAT_SURVEY, "ambient", confirmed=False),
    "aqi": CodeSpec(
        "aqi", RMMS_PLACEHOLDER, "uba_aqi", "UBA air-quality index (1-5)",
        VALUE_INTEGER, "{index}", None, CAT_SURVEY, "ambient", confirmed=False),

    # ── Ambient light → no standard LOINC: placeholder, flagged ─────────────
    "lux": CodeSpec(
        "lux", RMMS_PLACEHOLDER, "ambient_lux", "Ambient illuminance",
        VALUE_QUANTITY, "lx", "lx", CAT_SURVEY, "ambient", confirmed=False),

    # ── Device metadata (not a clinical observation) ────────────────────────
    "distance_mm": CodeSpec(
        "distance", RMMS_PLACEHOLDER, "subject_distance", "Device-to-subject distance",
        VALUE_QUANTITY, "mm", "mm", CAT_SURVEY, "device", confirmed=False),
}


def code_for(field: str) -> CodeSpec | None:
    return CODES.get(field)
