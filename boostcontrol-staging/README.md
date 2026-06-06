<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Boost Control — STAGING (peer review baseline)

This is the original staging design that the production module in
`common/include/opendash_boost.h` + `common/src/opendash_boost.c` is
expected to honor. **DO NOT DELETE.** Compare PRs against this folder.

Key requirements captured here:
- PID with aggressive + conservative tunings and activation thresholds
- Per-gear duty maps and setpoint maps (8 gears × 16 RPM points)
- Safety overlays: overboost, EFR speed, EGT yellow/critical, fuel pressure
- Mode switch: NORMAL / RACE
- Throttle-position boost reduction curve

Heritage: ported from MultiDisplay's RPMBoostController by Stephan
Martin / Dominik Gummel (GPL-3.0).
