<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# mos-4ch-a — 4-Channel MOS FET Controller (node 16)

**Active node.** Headless ESP32-WROOM-32E controller — no display. This node
**hosts the parachute/squib actuator path** (with `mos-4ch-b`) and is the only
node family where the **P1.3 roster policy-gate is LIVE** (`docs/POLICY_GATE.md`).
Read [`docs/safety-deployment-test-checklist.md`](../docs/safety-deployment-test-checklist.md)
and sign it off before trusting a live squib circuit.

## Identity

| Item | Value |
|---|---|
| Logical node ID | 16 (`OPENDASH_NODE_MOS_4CH_A`) |
| Board | ESP32-WROOM-32E (classic core, no native USB) |
| Channels | 4× MOS FET, **active-HIGH**, 1 kHz PWM (`SET_RELAY` payload[2] = duty 0-255) |
| Channel GPIOs | CH0=16, CH1=17, CH2=26, CH3=27 (labels shown on Center's relay-control screen) |
| Boost PWM output | Channel 3 (`MOS_BOOST_PWM_CHANNEL`) — boost subsystem writes duty every compute tick |
| Status LED | GPIO23 — heartbeat double-pulse; N flashes = N active channels; steady rapid flash = ESP-NOW init fault |

## Trust model (P1.3 — live in this firmware)

Every inbound frame passes `opendash_gate_decide()` (`common/src/opendash_roster.c`,
truth-table tested in `test/test_roster.c`):

- The **first bootstrap-eligible center-class frame latches the center MAC
  exactly once** (old "any sender re-latches" block deleted — the "grabbed GPS
  as center" bug is structurally dead: SYSTEM chatter and BOOST never latch).
- Voters are pinned via Center-pushed `ROSTER_PUSH 0x2C`; votes honored only
  from roster-pinned, node-id-bound senders.
- Roster is **NVS-persisted** (factory-fresh on miss/corrupt/version-mismatch).
- Rollover votes are fused **locally** (600 ms TTL, unanimous among pinned
  voters) — deploy survives center absence; AUTO_DETECT flag gates auto-fuse.
- `PERSIST_ARM` opt-in: arm state mirrors to NVS and restores at boot **only if
  the user enabled the flag**; default firmware boots DISARMED (owner ruling D4).
- Gate telemetry: `ctrl_rejected` counter + `last_reason` echo in the status blob.

## Fire path (safety-critical)

Channel energization is decided solely by `opendash_parachute_fire_verdict()`
in `common/` — a pure function whose full truth table runs in host tests on
every CI run. This main is pure verdict-transport: it applies the decision, it
does not compute gates. Deploy pulse uses an auto-off timer
(`OPENDASH_CMD_PARACHUTE_DEPLOY 0x2A`); disarm clears channels and the latch.

## Protocol

- Transport: ESP-NOW, channel 1; frame per `common/include/opendash_protocol.h`.
- Mask command format: `SET_RELAY 0x08` payload `[0xCA, node_id, mask, seq]`
  (bits 0-3 = channels 0-3; frames addressed to another node id are dropped);
  legacy per-channel format `[channel, state]` still accepted.
- Boot announce: after center bootstrap, sends actual channel state via
  `RELAY_STATUS 0x85` so Center clears any stale pre-power-cycle cache.
- Parachute opcodes: `0x27 SET_CONFIG`, `0x28 SET_ARM`, `0x29 PULL_ALL`,
  `0x2A DEPLOY`, `0x2B CALIBRATE`, status echo `0x95`, votes in `0x96`.
- BLE OTA: `common/src/opendash_bt_ota.c` (see `BLE_OTA.md`); the reserved
  wire field exists for future protocol evolution (documented wire room).

## Build & Flash

```bash
cd mos-4ch-a
idf.py set-target esp32        # classic ESP32 — NOT esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Full design: [`docs/POLICY_GATE.md`](../docs/POLICY_GATE.md) ·
subsystem: [`wiki/safety-deployment-system.md`](../wiki/safety-deployment-system.md) ·
protocol: [`docs/relay-espnow-protocol.md`](../docs/relay-espnow-protocol.md)