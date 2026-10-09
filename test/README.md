<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Host Unit Tests

Hardware-free unit tests for the pure-logic core of `common/`. The real
source files (`opendash_protocol.c`, `opendash_data_model.c`, `node_health.c`,
`opendash_parachute.c`) are compiled **verbatim** against a thin fake layer
(`stubs/`): an in-memory NVS, a controllable virtual clock, no-op logging
macros that still evaluate their arguments, and a transparent mutex. Unity
(vendored under `unity/`, MIT — see `unity/LICENSE.txt`) drives the suites.

## Run locally

```bash
cmake -S test -B test/build
cmake --build test/build -j
ctest --test-dir test/build --output-on-failure
```

Set `OD_VERBOSE_LOG=1` to see the code-under-test's log output while running.

## What is covered today

| Suite | Protects |
|-------|----------|
| `test_protocol.c` | Frame codec: SYNC/checksum/truncation handling, round-trips, 252-byte boundary |
| `test_data_model.c` | Store set/get/update/overflow + cross-domain id partitioning (MD_* never aliases ECU ids) |
| `test_node_health.c` | Health state machine: instant-ONLINE on any rx; freq-mode windowed OFFLINE; heartbeat-mode honest silence (DEGRADED ~2 missed, OFFLINE ~4); NVS restore comes back AWAITING |
| `test_parachute.c` | Deploy config defaults, hostile-config clamping (NaN/negative/oversize), NVS round-trip |
| `test_roster.c` | Policy-gate table (every opcode class both ways, bootstrap one-shot latch, ineligible rows), seq-dedupe, fusion TTL/override, store round-trip |
| `test_fuzz_parsers.c` | P3.1 fuzz: seeded deterministic corpora on both byte sinks — codec accepts ONLY canonical frames; MD payload decoder TAG/length gates + determinism + golden values |

## Coverage (P3.2)

CI builds this harness `-DOD_COVERAGE=ON` and runs
[`scripts/coverage_gate.sh`](../scripts/coverage_gate.sh): per-file line
floors at the measured 2026-10-08 baseline; floors ratchet UP only.
Locally: `cmake -S test -B test/build-cov -DOD_COVERAGE=ON` then
`../scripts/coverage_gate.sh build-cov` from this directory.

## Rules

- Tests test **real code** — never edit `common/src` from here; if a test
  fails, the code or the test's expectation is wrong, and the protocol docs
  decide which.
- Nothing here runs on the MCU; the ESP-IDF builds (`.github/workflows/build.yml`
  matrix) are a separate CI job.
