"""Typed, env-driven configuration (CLAUDE.md §5).

All configuration comes from environment variables (prefix ``RMMS_``) plus an
optional ``/etc/rmms/aggregator.env`` file in production. No config files in the
repo, no lab-specific defaults (the audit catches hardcoded IPs).
"""
from __future__ import annotations

from functools import lru_cache

from pydantic import SecretStr
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_prefix="RMMS_",
        env_file=("/etc/rmms/aggregator.env", ".env"),
        env_file_encoding="utf-8",
        extra="ignore",
    )

    # ── Tablet MQTT broker (mTLS) ──────────────────────────────────────────
    broker_host: str
    broker_port: int = 8883
    broker_ca_path: str
    broker_cert_path: str
    broker_key_path: str

    # ── Hospital FHIR endpoint + OAuth (empty token URL ⇒ no-auth dev mode) ─
    fhir_endpoint: str
    fhir_oauth_token_url: str = ""
    fhir_oauth_client_id: str = ""
    fhir_oauth_client_secret: SecretStr = SecretStr("")
    fhir_oauth_scopes: str = "system/Observation.write"

    # ── Local persistence ──────────────────────────────────────────────────
    db_path: str = "/var/lib/rmms/aggregator.db"

    # ── Behaviour knobs ────────────────────────────────────────────────────
    log_level: str = "INFO"
    health_port: int = 9100
    estimated_transport_latency_ms: int = 500   # §8.5 fallback for wall_ms == None
    bundle_max: int = 50                          # §9.2 max Observations / Bundle
    bundle_interval_s: float = 5.0                # …or this elapsed, whichever first
    post_timeout_s: float = 30.0
    publish_time_sync: bool = True                # publish rmms/<uuid>/time/set on connect

    @property
    def auth_enabled(self) -> bool:
        return bool(self.fhir_oauth_token_url)


@lru_cache(maxsize=1)
def get_settings() -> Settings:
    return Settings()   # type: ignore[call-arg]  (fields populated from env)
