"""Retry backoff schedule + dead-letter decision (CLAUDE.md §9.2).

Pure module — stdlib only, so the policy is unit-testable.

Retry 5xx / connection errors / 429 with exponential backoff (1,2,4,8,16,32 s)
up to MAX_ATTEMPTS, then dead-letter. A non-retriable error (a 4xx contract
violation) dead-letters immediately.
"""
from __future__ import annotations

_BACKOFF_S = (1.0, 2.0, 4.0, 8.0, 16.0, 32.0)
MAX_ATTEMPTS = 6


def next_delay_s(attempts_made: int) -> float:
    """Delay before the next attempt, given how many attempts already failed."""
    if attempts_made < 0:
        attempts_made = 0
    idx = min(attempts_made, len(_BACKOFF_S) - 1)
    return _BACKOFF_S[idx]


def should_dead_letter(attempts_made: int, retriable: bool) -> bool:
    """True if this batch should move to the dead-letter store rather than retry.

    Non-retriable failures dead-letter at once; retriable ones only after the
    attempt budget is exhausted."""
    if not retriable:
        return True
    return attempts_made >= MAX_ATTEMPTS
