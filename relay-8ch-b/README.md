<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# relay-8ch-b — 8-Channel Relay Controller B (node 15)

> **Last updated:** 2026-10-21 — dated-header rollout (convention: see PROJECT_INDEX header). Content
> unchanged in this pass; content changes stay recorded in git history and the audit ledger.

**Active node.** Twin of [`relay-8ch-a/README.md`](../relay-8ch-a/README.md),
second physical module:

- Logical node ID **15** (`OPENDASH_NODE_RELAY_8CH_B`).
- Channels: 8 electromechanical relays, **active-LOW**; channel GPIOs CH0=32,
  CH1=33, CH2=25, CH3=26, CH4=27, CH5=14, CH6=12, CH7=13 (labels `RELAY B CH1..CH8`).
- Status LED GPIO23 (same pattern).
- Trust model: **pre-P1.3 center latch, pending roster-gate wiring** (same as
  relay-4ch-hd and relay-8ch-a); mask payload `[0xCA, node_id, mask, …]` with
  target-id drop.

Build: `cd relay-8ch-b && idf.py set-target esp32 && idf.py build`.
Protocol reference: [`docs/relay-espnow-protocol.md`](../docs/relay-espnow-protocol.md).