"""Service entrypoint: wire components and run the post loop.

    python -m rmms_aggregator

Pipeline: MQTT subscriber (background thread) ingests samples → SQLite; the main
thread drains the pending-Observation queue to the FHIR endpoint with retry /
dead-letter. Schema is created by Alembic at deploy time (`alembic upgrade head`).
"""
from __future__ import annotations

import logging
import re
import signal
import threading
import time

from . import __version__
from .config import get_settings
from .fhir.builder import build_post_bundle, partition_valid
from .fhir.client import FhirClient
from .fhir.oauth import TokenProvider
from .health.server import start_health_server
from .mqtt.client import MqttClient
from .mqtt.publisher import publish_time_set
from .mqtt.subscriber import Ingestor
from .retry.deadletter import MAX_ATTEMPTS, next_delay_s
from .storage.repository import Repository

log = logging.getLogger("rmms_aggregator")

_SAMPLE_RETENTION_MS = 30 * 24 * 60 * 60 * 1000   # §10.4: keep samples 30 days
_PURGE_INTERVAL_S = 3600


def _now_ms() -> int:
    return int(time.time() * 1000)


class _RedactingFilter(logging.Filter):
    """Scrub secrets from log lines (§14.1). Secrets are normally never logged
    (config uses SecretStr; tokens are never formatted), but this is a defensive
    net for OAuth bearer tokens / client_secret that might appear in an error
    string echoed back from a dependency."""

    _PATTERNS = (
        re.compile(r"(?i)(bearer\s+)[A-Za-z0-9._\-]+"),
        re.compile(r"(?i)(client_secret=)[^&\s\"']+"),
        re.compile(r"(?i)(access_token\"?\s*[:=]\s*\"?)[A-Za-z0-9._\-]+"),
    )

    def filter(self, record: logging.LogRecord) -> bool:
        try:
            msg = record.getMessage()
        except Exception:
            return True
        redacted = msg
        for pat in self._PATTERNS:
            redacted = pat.sub(r"\1<redacted>", redacted)
        if redacted != msg:
            record.msg = redacted
            record.args = ()
        return True


def _post_loop(repo: Repository, client: FhirClient, ingestor: Ingestor,
               settings, stop: threading.Event) -> None:
    consecutive_failures = 0
    last_purge = 0.0
    while not stop.is_set():
        # The whole iteration is guarded: a transient DB/transport error must
        # back off and continue, never kill the store-and-forward loop.
        try:
            # 1) promote retry-exhausted rows to dead_letter so they aren't re-fetched
            repo.dead_letter_exhausted(MAX_ATTEMPTS, _now_ms())

            # 2) periodic sample retention purge (observations are kept; §10.4)
            mono = time.monotonic()
            if mono - last_purge > _PURGE_INTERVAL_S:
                removed = repo.purge_samples_older_than(_now_ms() - _SAMPLE_RETENTION_MS)
                if removed:
                    log.info("purged %d samples older than 30 days", removed)
                last_purge = mono

            # 3) re-drive samples stored before their device→patient binding (§12)
            repushed = ingestor.redrive_unbuilt()
            if repushed:
                log.info("re-drove %d previously-unbuilt samples", repushed)

            rows = repo.fetch_pending(settings.bundle_max)
            if not rows:
                consecutive_failures = 0
                stop.wait(settings.bundle_interval_s)
                continue

            ids = [r[0] for r in rows]
            # Build the transaction Bundle. If a stored Observation is corrupt,
            # isolate it (dead-letter just that row) so the rest still post.
            try:
                bundle = build_post_bundle(rows)
            except Exception as exc:
                valid, bad = partition_valid(rows)
                log.error("bundle build failed (%s); isolating %d bad row(s)", exc, len(bad))
                if bad:
                    repo.record_failure(bad, f"bundle: {exc}", _now_ms(), dead_letter=True)
                if not valid:
                    continue
                rows, ids = valid, [r[0] for r in valid]
                bundle = build_post_bundle(rows)

            result = client.post_bundle(bundle)
            now = _now_ms()
            if result.ok:
                consecutive_failures = 0
                server_ids = {ids[i]: loc for i, loc in enumerate(result.locations)
                              if i < len(ids) and loc}
                repo.mark_posted(ids, server_ids, now)
                log.info("posted %d observations", len(ids))
            elif not result.retriable:
                log.error("permanent FHIR error, dead-lettering %d: %s", len(ids), result.error)
                repo.record_failure(ids, result.error, now, dead_letter=True)
                consecutive_failures = 0
            else:
                consecutive_failures += 1
                repo.record_failure(ids, result.error, now, dead_letter=False)   # attempts++
                delay = result.retry_after_s or next_delay_s(consecutive_failures - 1)
                log.warning("retriable FHIR error (%s); backing off %.0fs", result.error, delay)
                stop.wait(delay)
        except Exception:
            consecutive_failures += 1
            delay = min(next_delay_s(consecutive_failures - 1), 60.0)
            log.exception("post-loop iteration failed; backing off %.0fs", delay)
            stop.wait(delay)


def main() -> int:
    settings = get_settings()
    logging.basicConfig(
        level=getattr(logging, settings.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    logging.getLogger().addFilter(_RedactingFilter())   # §14.1 secret redaction
    log.info("rmms-aggregator %s starting", __version__)

    repo = Repository(settings.db_path)

    tokens = TokenProvider(
        settings.fhir_oauth_token_url, settings.fhir_oauth_client_id,
        settings.fhir_oauth_client_secret.get_secret_value(), settings.fhir_oauth_scopes)
    if not tokens.enabled:
        log.warning("OAuth disabled (no RMMS_FHIR_OAUTH_TOKEN_URL) — DEV ONLY, no auth")
    client = FhirClient(settings.fhir_endpoint, tokens, settings.post_timeout_s)

    # Time-sync publisher needs the (not-yet-created) MQTT client → late binding.
    holder: dict[str, MqttClient] = {}

    def time_pub(uuid: str) -> None:
        c = holder.get("c")
        if c is not None:
            publish_time_set(c, uuid)

    ingestor = Ingestor(repo, settings, time_sync_publisher=time_pub)
    mqtt = MqttClient(settings, ingestor.handle)
    holder["c"] = mqtt

    def status_provider() -> dict:
        pending, dead = repo.counts()
        return {
            "status": "ok" if mqtt.connected else "degraded",
            "broker": {"connected": mqtt.connected},
            "queue_depth": {"pending": pending, "dead_letter": dead},
            "version": __version__,
        }

    health = start_health_server(settings.health_port, status_provider)

    stop = threading.Event()

    def _shutdown(signum, _frame):  # noqa: ANN001
        log.info("signal %s — shutting down", signum)
        stop.set()

    signal.signal(signal.SIGTERM, _shutdown)
    signal.signal(signal.SIGINT, _shutdown)

    mqtt.start()
    try:
        _post_loop(repo, client, ingestor, settings, stop)
    finally:
        mqtt.stop()
        health.shutdown()
        log.info("stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
