"""RMMS Aggregator — the SBC (Radxa) service.

Subscribes to raw sensor JSON on the tablet broker, builds FHIR R4 Observations
(idempotent on Observation.identifier), buffers them in SQLite for ≥24 h cloud
outage tolerance, and POSTs transaction Bundles to the hospital FHIR endpoint.

See CLAUDE.md (this directory) for the authoritative spec, and the firmware
repo's docs/sbc-failover-and-idempotency.md for the cross-tier contract.
"""
__version__ = "0.1.0"
