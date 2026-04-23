# src/models/enums.py

"""
This module defines enumeration types for status and events used in the registration software.
"""

from enum import Enum


class RegistrationStatus(str, Enum):
    SEARCHING = "SEARCHING"
    IDENTIFIED = "IDENTIFIED"
    UNAUTHORIZED = "UNAUTHORIZED"
    NOT_DECODED = "NOT_DECODED"
    REGISTERED = "REGISTERED"
    OUT_OF_RANGE = "OUT_OF_RANGE"

class IdentificationEvent(str, Enum):
    DEVICE_FOUND = "DEVICE_FOUND"
    IDENTIFIED = "IDENTIFIED"

class QrDetectorEvent(str, Enum):
    QR_DETECTED = "QR_DETECTED"
    QR_UNDECODED = "QR_UNDECODED"