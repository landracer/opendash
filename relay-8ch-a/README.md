<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# relay-8ch-a — 8-Channel Relay Controller A (node 14)

**Active node.** Headless ESP32-WROOM-32E controller for low-amp device loads.

## Identity

| Item | Value |
|---|---|
| Logical node ID | 14 (`OPENDASH_NODE_RELAY_8CH_A`) |
| Board | ESP32-WROOM-32E (classic core, no native USB) |
| Channels | 8 electromechanical relays, **active-LOW**, no PWM |
| Channel GPIOs | CH0=32, CH1=33, CH2=25, CH3=26, CH4=27, CH5=14, CH6=12, CH7=13 (labels `RELAY A CH1..CH8`) |
| Status LED | GPIO23 — heartbeat double-pulse; N flashes = N active channels |

## Trust model (honest status)

**Pre-P1.3 block, like relay-4ch-hd:** first center-class sender is latched as
the center (bit-7 rule); roster gate wiring is pending. Mask frames carry a
target node-id byte; frames for other nodes are dropped.

## Protocol

- Transport: ESP-NOW, channel 1; `SET_RELAY 0x08` mask payload
  `[0xCA, node_id, relay_mask, seq]`; state echo `[0xCA, node_id, actual_mask]`
  (compact audit response) on `RELAY_STATUS 0x85`.

## Build & Flash

```bash
cd relay-8ch-a
idf.py set-target esp32        # classic ESP32 — NOT esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Protocol detail: [`docs/relay-espnow-protocol.md`](../docs/relay-espnow-protocol.md) ·
[`wiki/relay-mos-controllers.md`](../wiki/relay-mos-controllers.md)