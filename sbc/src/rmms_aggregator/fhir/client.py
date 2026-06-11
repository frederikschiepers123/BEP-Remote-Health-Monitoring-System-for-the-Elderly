"""FHIR HTTP client — POST transaction Bundles (CLAUDE.md §9.2).

Retry policy (§9.2): retry 5xx / connection errors / 429 (honouring Retry-After);
do NOT retry other 4xx (a contract violation that won't self-heal → dead-letter).
The caller (the post loop) owns the backoff schedule and the pending→dead_letter
transition; this client just classifies one attempt.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field

import httpx

from .oauth import TokenProvider

log = logging.getLogger(__name__)


@dataclass(slots=True)
class PostResult:
    ok: bool
    retriable: bool
    error: str = ""
    retry_after_s: float | None = None
    # FHIR response-entry locations, in request order (best-effort server ids).
    locations: list[str] = field(default_factory=list)


class FhirClient:
    def __init__(self, endpoint: str, token_provider: TokenProvider,
                 timeout_s: float = 30.0) -> None:
        self._endpoint = endpoint.rstrip("/")
        self._tokens = token_provider
        self._timeout = timeout_s

    def post_bundle(self, bundle: dict) -> PostResult:
        headers = {
            "Content-Type": "application/fhir+json",
            "Accept": "application/fhir+json",
        }
        token = self._tokens.token()
        if token:
            headers["Authorization"] = f"Bearer {token}"
        elif self._tokens.enabled:
            return PostResult(False, True, "OAuth token unavailable")

        try:
            resp = httpx.post(self._endpoint, json=bundle, headers=headers,
                              timeout=self._timeout)
        except httpx.RequestError as exc:
            return PostResult(False, retriable=True, error=f"connection: {exc}")

        if resp.is_success:
            return PostResult(True, retriable=False, locations=self._locations(resp))

        if resp.status_code == 429:
            ra = resp.headers.get("Retry-After")
            return PostResult(False, retriable=True, error="429 throttled",
                              retry_after_s=float(ra) if ra and ra.isdigit() else None)
        if 500 <= resp.status_code < 600:
            return PostResult(False, retriable=True,
                              error=f"{resp.status_code}: {resp.text[:500]}")
        # 4xx (≠429): permanent contract error → dead-letter.
        return PostResult(False, retriable=False,
                          error=f"{resp.status_code}: {resp.text[:500]}")

    @staticmethod
    def _locations(resp: httpx.Response) -> list[str]:
        try:
            body = resp.json()
        except ValueError:
            return []
        return [
            (e.get("response") or {}).get("location", "")
            for e in body.get("entry", [])
        ]
