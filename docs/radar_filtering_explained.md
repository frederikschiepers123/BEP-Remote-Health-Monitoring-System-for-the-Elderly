HOW THE RADAR FILTERING WORKS (in simple terms)
================================================

All of this lives in one file: components/sensor_radar/radar_filter.c
The design write-up is at the top of radar_filter.h, and the rationale is in
docs/adr/0005-mcu-side-radar-filtering.md.


WHY THE FILTER EXISTS
---------------------
The raw 60 GHz radar (MR60BHA2) is jumpy. Left alone, the numbers flicker every
frame, "ghost" targets make presence blink on and off, distance jitters, and
heart/breath rates bounce around. If we sent that straight to the mirror, the
tiles would be unreadable. So before any radar sample goes on the wire, it
passes through a plausibility filter that DEBOUNCES, VALIDATES, and SMOOTHS it.

Key point: the filter is pure logic. It takes one raw sample in and gives one
cleaned sample out - same shape, same fields. It doesn't touch hardware, so it
works for any radar (including the HMMD), and we can unit-test it on a laptop.

The whole thing is one function, radar_filter_apply(), called once per radar
reading. Think of it as a pipeline of stages:


STAGE 1 - IS SOMEONE ACTUALLY THERE? (presence debounce)
--------------------------------------------------------
We don't trust a single "presence" blip. Presence "evidence" = the radar's
presence flag OR a distance reading that falls in a sensible range.

  - Evidence has to persist for 10 seconds before we declare "present."
  - Once present, we need 8 seconds of silence before we declare "absent."

This is the classic anti-flicker trick: a brief ghost can't flip presence on,
and a momentary dropout can't flip it off.


STAGE 2 - IS THE DISTANCE BELIEVABLE? (gate + smooth)
-----------------------------------------------------
A distance reading is processed by a reusable little component called
StableValueFilter. It does four things in order:

  1. Range gate  - throw away anything outside 0.35 m - 1.5 m. Physically
     impossible readings are silently ignored.
  2. Jump guard  - if the value suddenly leaps more than 20 cm, treat it as a
     new candidate and restart the clock (don't believe a teleport).
  3. Confirm window - the value has to stay stable for ~6 s before we trust it.
  4. Low-pass smoothing - once trusted, a time-aware average glides toward new
     readings instead of snapping, so the displayed distance is smooth.


STAGE 3 - HEART AND BREATH RATES
--------------------------------
Vitals are only looked at when presence AND distance are both locked in.
Otherwise we'd be reading vitals off noise. Each vital then goes down two
parallel paths:

  A) The LIVE path (what the mirror actually shows).
     Each vital keeps a rolling 30-second list of recent readings and publishes
     the MEDIAN of that list. The median is the magic here: a single garbage
     reading gets out-voted by the good ones instead of throwing the display
     off. There's a two-tier twist - it prefers the last ~12 s of readings so
     the tile reacts when your real breathing rate changes, but if the sensor
     goes quiet (it sends heart and breath in alternating bursts), it falls back
     to the full 30 s so the number doesn't blank out. A value shows after ~5
     readings accumulate, and disappears if it goes too stale (>25 s old) or the
     person leaves.

  B) The ESTIMATE path (see Stage 5).

There's also a calibration step: the sensor reads high, so we subtract a fixed
offset (20 BPM off heart, 2 RPM off breath) at output time - a bench-measured
correction.


STAGE 3b - BREATH-HOLD DETECTION
--------------------------------
The breath RATE is a windowed frequency, so it can't drop during a held breath.
But the raw chest-motion AMPLITUDE does go flat. So we watch the amplitude: if
it stays below a threshold for ~5 s, we report "no respiratory motion" (a
possible breath-hold), null out the breath rate, and the mirror shows "No
breathing." One real breath clears it. (Hysteresis + a confirm window stop it
from triggering on the quiet pause at the top of a normal breath.)


STAGE 4 - THE QUALITY FLAG (q)
------------------------------
Every published sample gets a quality marker so the downstream Radxa knows how
much to trust it:
  0 = fresh and good
  2 = still validating or the value is a bit stale (preliminary)
  3 = invalid, with nothing to show
We never silently drop a sample - we always label it.


STAGE 5 - THE ROBUST "BEST ESTIMATE" (logged, not displayed)
------------------------------------------------------------
Separately, while a vital is stable we collect one clean sample per second into
a 20-second window, then reduce it to a single robust number using median + MAD
outlier rejection (statistically throw out the weird ones, average the rest).
It's logged to the dev console, not put on the wire. It's a higher-confidence
"this is really their heart rate" number for debugging/validation.


THE ONE-SENTENCE VERSION
------------------------
Raw radar is noisy, so before publishing we (1) require steady evidence before
saying someone's present, (2) reject impossible distances and smooth the rest,
(3) show the median of recent heart/breath readings so a single bad value can't
throw the tile off, (4) detect breath-holds from chest-motion amplitude, and
(5) tag every sample with a quality flag instead of ever dropping one.

The tunable constants (ranges, timeouts, thresholds) are all #defines at the top
of radar_filter.h with comments explaining where each number came from - most
were tuned against a live sensor on the bench.
