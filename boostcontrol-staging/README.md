<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Boost Control — STAGING (peer review baseline)

> ## ⚠️ GPL-3.0 HERITAGE — READ BEFORE USING THIS CODE
>
> The algorithm and wire format captured in this folder **descend from
> MultiDisplay's `RPMBoostController` (Stephan Martin / Dominik Gummel, GPL-3.0)**.
> This is a **reference baseline for reading and PR comparison only**. It is **not**
> clean-room and must **not** be shipped, linked, or redistributed as-is.
>
> A true **clean-room rewrite** of the boost subsystem is a **v1.0.0 release gate**
> (see TODO §6.0 and the release-gate notice in `readme.md`). Until that rewrite
> lands, this project stays private / source-available — do not publish or
> redistribute this code or anything derived from it.

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
