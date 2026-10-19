<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# relay-4ch-hd — 4-Channel HD Relay Controller (node 13)

**Active node.** Headless ESP32-WROOM-32E controller for high-amp loads
(radiator fans, water pump, fuel pump). No display, no IMU, no deployment logic.

## Identity

| Item | Value |
|---|---|
| Logical node ID | 13 (`OPENDASH_NODE_RELAY_4CH`) |
| Board | ESP32-WROOM-32E (classic core, no native USB) |
| Channels | 4 electromechanical relays, **active-LOW**, no PWM |
| Channel GPIOs | CH0=12 `RAD FAN 1`, CH1=13 `RAD FAN 2`, CH2=14 `WATER PUMP`, CH3=15 `FUEL PUMP` |
| Status LED | GPIO5 — heartbeat double-pulse; N flashes = N active channels |

## Trust model (honest status)

This node still runs the **pre-P1.3 trust block**: the first sender of a
center-class opcode (bit 7 clear) is latched as the center and added as a peer.
The roster policy-gate that replaces this (`docs/POLICY_GATE.md`, live on
`mos-4ch-a/b`) is **not yet wired here** — recorded as pending work. Payload-level
protection today: the mask format carries a target node-id byte and frames
addressed to another node are dropped.

## Protocol

- Transport: ESP-NOW, channel 1; frame per `common/include/opendash_protocol.h`.
- `SET_RELAY 0x08` mask format: `[0xCA, node_id, mask]` (bits 0-3 = channels
  0-3); immediate `RELAY_STATUS 0x85` confirmation echo `[0xCA, node_id, mask]`;
  legacy per-channel `[channel, state]` still accepted.
- Boot announce: after center discovery, reports actual channel state so
  Center clears any stale cache from before the power cycle.

## Build & Flash

```bash
cd relay-4ch-hd
idf.py set-target esp32        # classic ESP32 — NOT esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Protocol detail: [`docs/relay-espnow-protocol.md`](../docs/relay-espnow-protocol.md) ·
[`wiki/relay-mos-controllers.md`](../wiki/relay-mos-controllers.md)