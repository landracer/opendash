feat(nodes): add openDstream ESP-NOW → USB Serial/JTAG relay node

- Created standalone headless project (openDstream/) for ESP32-S3 with native USB-OTG
- Implements pure ESP-NOW slave relay: receives all frames on channel 6, pipes to USB Serial/JTAG
- Uses ESP-IDF v6.x high-level driver API (driver/usb_serial_jtag.h) — no HAL/LL complexity
- Zero display dependencies: no LVGL, no common component, no peripherals required
- Build verified: openDstream.bin (~267KB) compiles cleanly on ESP32-S3 target
- Status LED on GPIO8 (active-low) for alive indication
- Wiki documentation added: wiki/opendstream-relay-node.md with architecture, usage, troubleshooting
- Updated TODO.md: active node families count 11→12, added §2 section for openDstream development tracking
- Designed for bidirectional relay (PC ↔ ESP-NOW) — current implementation is unidirectional (ESP-NOW → USB)
- Future enhancements: bidirectional support, TinyUSB CDC-ACM, frame filtering, buffering

This node bridges the wireless OpenDash ESP-NOW network to multidisplay-app on PC via USB, enabling real-time telemetry visualization without display hardware.