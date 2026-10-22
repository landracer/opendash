<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# mos-4ch-b — 4-Channel MOS FET Controller (node 17)

> **Last updated:** 2026-10-21 — dated-header rollout (convention: see PROJECT_INDEX header). Content
> unchanged in this pass; content changes stay recorded in git history and the audit ledger.

**Active node.** Twin of [`mos-4ch-a/README.md`](../mos-4ch-a/README.md) — same
firmware shape, same safety architecture, second physical module:

- Logical node ID **17** (`OPENDASH_NODE_MOS_4CH_B`); announce payload `[17, 0x01, 0x00]`.
- Channels: 4× MOS FET, **active-HIGH**, 1 kHz PWM; channel GPIOs CH0=16,
  CH1=17, CH2=26, CH3=27 (labels `MOS B CH1..CH4`); boost PWM channel 3.
- Status LED GPIO23 (same pattern as MOS-A).
- **P1.3 roster gate LIVE** (`opendash_gate_decide()`; NVS-persisted roster;
  one-shot center bootstrap latch; MOS-local vote fusion; `PERSIST_ARM` opt-in).
- Fire decisions come only from `opendash_parachute_fire_verdict()` in `common/`
  (table-tested in `test/test_parachute.c`) — this main is pure verdict-transport.

Build: `cd mos-4ch-b && idf.py set-target esp32 && idf.py build`.
Everything else — gate semantics, mask format, pulse/auto-off, telemetry
counters — is documented once in [`mos-4ch-a/README.md`](../mos-4ch-a/README.md)
and [`docs/POLICY_GATE.md`](../docs/POLICY_GATE.md); do not fork that prose here.