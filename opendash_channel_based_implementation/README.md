<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Channel-Based ESP-NOW Implementation

> **STATUS: NEW OPENDASH ARCHITECTURE STANDARD (April 2026)**
>
> This is the authoritative reference implementation for the OpenDash
> channel-based, event-driven communication system. **All new code MUST
> follow this architecture.** Polling and pinging are PROHIBITED.

This directory contains a complete rewrite of the ESP-NOW master implementation
with channel-based architecture for the opendash project. This approach
eliminates ALL polling/pinging overhead while providing a scalable, efficient
communication system suitable for safety-critical racecar applications.

## ⚠️ MANDATORY: No Polling, No Pinging

The old architecture used a 20ms PING broadcast loop for node discovery and
keepalive. **This is permanently deprecated.** The new architecture uses:

1. **Push-based data flow** — nodes send data when it changes, not when polled
2. **One-time registration** — nodes announce themselves once at boot, center
   registers them into the appropriate priority channel
3. **Heartbeat via data flow** — if a node is sending data on its channel, it's
   alive. No separate keepalive needed.
4. **Offline detection via absence** — if no data arrives within the channel's
   timeout window, mark the node offline. No polling required.

## Key Features

- **Zero Polling Overhead**: No PING broadcasts, no periodic status checks
- **Event-Driven Architecture**: Data sent only when updated (push, not pull)
- **4 Priority Channels**: Critical (10ms), Medium (50ms), Low (200ms), Control (immediate)
- **Persistent Connections**: Nodes register once, communicate continuously
- **Smart Data Routing**: Auto-route data points to correct channel by type
- **Delta Updates**: Only send values that changed since last transmission
- **QoS Per Channel**: Retry policies and delivery guarantees per priority level
- **Scalable**: Supports 18+ node types with zero additional polling overhead

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         CENTER (MASTER)                             │
│                                                                     │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────┐  ┌─────────────┐  │
│  │ CH0 CRITICAL│  │ CH1 MEDIUM  │  │ CH2 LOW  │  │ CH3 CONTROL │  │
│  │ ≤10ms       │  │ ≤50ms       │  │ ≤200ms   │  │ immediate   │  │
│  │             │  │             │  │          │  │             │  │
│  │ GPS pos     │  │ LEFT pod    │  │ Diag     │  │ Relay ON/OFF│  │
│  │ BMS pack V  │  │ RIGHT pod   │  │ Config   │  │ Brightness  │  │
│  │ RPM/boost   │  │ POD1-8      │  │ Logging  │  │ Reboot      │  │
│  │ Alarms      │  │ Relay state │  │ Errors   │  │ OTA trigger │  │
│  └──────┬──────┘  └──────┬──────┘  └────┬─────┘  └──────┬──────┘  │
│         │                │              │                │          │
│         └────────────────┴──────────────┴────────────────┘          │
│                              │                                      │
│                   ┌──────────┴──────────┐                           │
│                   │ ESP-NOW Transport   │                           │
│                   │ (opendash_espnow.c) │                           │
│                   └─────────────────────┘                           │
└─────────────────────────────────────────────────────────────────────┘
                              ▲
               Push data on change (no polling)
                              │
    ┌─────────┬───────────────┼───────────────┬──────────┐
    │         │               │               │          │
┌───┴───┐ ┌──┴───┐    ┌──────┴──────┐  ┌─────┴────┐ ┌──┴──────┐
│ GPS   │ │ BMS  │    │ LEFT (UART) │  │  RIGHT   │ │ RELAY   │
│ CH0   │ │ CH0  │    │ CH1         │  │  CH1     │ │ CH2/CH3 │
│ push  │ │ push │    │ relay MD    │  │  passive │ │ cmd rx  │
└───────┘ └──────┘    └─────────────┘  └──────────┘ └─────────┘
```

## Directory Structure

```
opendash_channel_based_implementation/
├── README.md                           ← You are here
├── channel_config.h                    ← Channel timing, priorities, buffer sizes
├── channel_management.h                ← Channel manager API (init, send, receive, register)
├── channel_management.c                ← Channel manager implementation
├── node_definitions.h                  ← Node types → channel assignments
├── espnow_master.h                     ← Master controller API (channel-based)
├── espnow_master.c                     ← Master controller implementation
├── examples/
│   └── channel_assignment_example.c    ← Node/channel assignment demo
└── documentation/
    ├── architecture_overview.md        ← Full architecture deep-dive
    └── migration_guide.md              ← Step-by-step migration from polling
```

## Channel Definitions

| Channel | Priority | Max Interval | Data Types | Nodes |
|---------|----------|-------------|------------|-------|
| CH0 CRITICAL | Highest | 10ms | GPS, BMS, engine RPM/boost, alarms | GPS, BMS |
| CH1 MEDIUM | Standard | 50ms | Pod display data, relay feedback, sensors | LEFT, RIGHT, POD1-8 |
| CH2 LOW | Background | 200ms | Diagnostics, config, logging, relay status | RELAY, MOS, system |
| CH3 CONTROL | Immediate | 5ms | Relay ON/OFF, brightness, reboot, OTA | All (command target) |

## Data Flow Rules

1. **Nodes push data** — they don't wait to be asked
2. **Center routes data** — received data is forwarded to appropriate consumers
3. **Commands use CH3** — relay toggles, OTA triggers, reboots go through control channel
4. **Status via data presence** — no separate heartbeat; data flow IS the heartbeat
5. **Offline = no data** — configurable per-channel timeout triggers offline status

## Migration from Polling Architecture

See [documentation/migration_guide.md](documentation/migration_guide.md) for the
complete step-by-step migration path. Key changes:

1. Remove all `PING` broadcast calls from `espnow_master.c`
2. Replace `espnow_master_task()` polling loop with channel dispatcher
3. Add `ANNOUNCE` message to slave node boot sequence (one-time registration)
4. Update slave nodes to push data proactively instead of waiting for requests
5. Update offline detection from PING-timeout to data-absence-timeout