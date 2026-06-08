# Architecture Decision Records

Each significant architectural decision is recorded here as a short ADR.

## Format

```
# ADR-NNN: Title

**Status:** Accepted | Superseded by ADR-NNN | Deprecated
**Date:** YYYY-MM-DD

## Context
What forced a decision?

## Decision
What was decided?

## Consequences
What becomes easier or harder?
```

## Index

| # | Title | Status |
|---|-------|--------|
| (none yet — first decision record goes here) | | |

## When to write an ADR

Per CLAUDE.md §4: any new dependency, any change to `board_pico2wh.h`, any new
MQTT topic or command, any change to the security model, or any deviation from
the bring-up order in §15 requires an ADR before the code is written.
