"""OAuth 2.0 client-credentials token provider (CLAUDE.md §9.1).

Caches the access token until 30 s before expiry; refreshes on demand. When no
token URL is configured (dev against HAPI), `token()` returns None and the
client posts without auth — but the caller must log a clear "DEV ONLY" warning
(§20: do not bypass OAuth silently).

Dev uses ``client_secret_post``. Production should use the SMART backend-services
JWT client assertion (§9.1) — that path swaps the grant body here; the cache and
call sites are unchanged. (authlib is available for the JWT flow.)
"""
from __future__ import annotations

import logging
import threading
import time

import httpx

log = logging.getLogger(__name__)


class TokenProvider:
    def __init__(self, token_url: str, client_id: str, client_secret: str,
                 scopes: str, skew_s: int = 30) -> None:
        self._url = token_url
        self._client_id = client_id
        self._secret = client_secret
        self._scopes = scopes
        self._skew = skew_s
        self._lock = threading.Lock()
        self._token: str | None = None
        self._expiry: float = 0.0

    @property
    def enabled(self) -> bool:
        return bool(self._url)

    def token(self) -> str | None:
        if not self._url:
            return None
        now = time.monotonic()
        with self._lock:
            if self._token and now < self._expiry - self._skew:
                return self._token
            self._token, ttl = self._fetch()
            self._expiry = now + ttl
            return self._token

    def _fetch(self) -> tuple[str, float]:
        resp = httpx.post(self._url, data={
            "grant_type": "client_credentials",
            "client_id": self._client_id,
            "client_secret": self._secret,
            "scope": self._scopes,
        }, timeout=15.0)
        resp.raise_for_status()
        body = resp.json()
        access = body.get("access_token")
        if not access:
            raise RuntimeError("token endpoint returned no access_token")
        return access, float(body.get("expires_in", 300))
