<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OTA via Android — Plan & Options

> Decision document for adding an Android-based OTA path so a phone in the
> pits can flash any pod without needing a laptop. Not yet implemented.

The Linux/BlueZ client (`ble_ota.py`) works but requires a laptop, a
Python venv, and physical proximity. In the pits we want a phone in your
pocket to do the same job. This document lays out the realistic options,
ranked by effort-to-value.

---

## TL;DR recommendation

1. **Short term (this week)** — Validate the GATT protocol against
   Android with **nRF Connect for Mobile** macros. Zero code.
2. **Medium term (this quarter)** — Build a thin **Flutter** app using
   `flutter_blue_plus`. Single codebase reaches Android + iOS later.
3. **Long term (when transfer time matters)** — Add a **WiFi-SoftAP +
   HTTP OTA** path. ~30 s transfers vs ~5 min. Use it in the garage,
   keep BLE for the trail/pits where you don't want to leave the radio.

The on-device GATT service (`opendash_bt_ota.c`) requires **no changes**
for any of these. It's already a clean, vendor-agnostic protocol:
START → DATA chunks → END → status notifications, with RESUME from
persisted offset.

---

## Option A — nRF Connect for Mobile (validation, zero dev)

Nordic's free Android app supports recording GATT macros. You can:

1. Trigger `ENTER_BT_OTA` from CENTER as usual.
2. Open nRF Connect → connect to `OpenDash-RIGHT-OTA`.
3. Issue the `START` command write manually.
4. Use nRF Connect's "Send file content" feature to push the firmware
   bytes to the DATA characteristic (it'll fragment for you).
5. Issue `END`.

**Pros**
- Zero code. Works **today**.
- Lets us measure Android-native throughput before we build anything.
  Android typically negotiates a faster connection interval than BlueZ,
  so this is also a real performance datapoint.
- Useful for field debugging forever.

**Cons**
- Manual. Not a one-tap solution.
- No progress bar, no retry/RESUME automation.
- Not appropriate for non-technical users.

**Use it for**: confirming the protocol works on Android, measuring
upper-bound throughput, demoing.

---

## Option B — Flutter + flutter_blue_plus (recommended primary path)

A small Flutter app with one button per node. Pick a `.bin` from
device storage, tap "Flash RIGHT", watch a progress bar.

**Architecture**
- One Dart file: GATT client (mirror of `ble_ota.py`).
- One Dart file: UI (node picker, file picker, progress bar).
- ~600–800 LOC total. Reuse the START/DATA/END framing verbatim.

**Pros**
- Single codebase ships to Android **and** iOS later if wanted.
- `flutter_blue_plus` is actively maintained, supports MTU negotiation,
  PHY selection (request 2M), and chunked writes.
- Material UI is free.
- Easy to embed firmware bundles in the app and download/check signatures
  later.

**Cons**
- A Dart codebase to maintain.
- Background BLE is awkward on Android (foreground service required for
  long transfers — but our OTA is 5 min, app stays in foreground, fine).

**Required permissions**
- `BLUETOOTH_CONNECT`, `BLUETOOTH_SCAN`, `BLUETOOTH_ADVERTISE` (Android 12+)
- `ACCESS_FINE_LOCATION` (some Android versions still require it for BLE scan)
- `READ_MEDIA_*` or storage access for picking the firmware file

**Estimated effort**: ~2–3 evenings to first working version, another to
polish.

---

## Option C — React Native + react-native-ble-plx

Functionally equivalent to Option B. Choose based on what your team
prefers maintaining. Slightly larger surface area than Flutter, but the
JS ecosystem is bigger if you ever want to extend with charts/dashboards.

**Pros / Cons**: roughly the same as B. Skip if there's no React Native
team momentum.

---

## Option D — Native Android (Kotlin)

Most control, highest ceiling, most platform-specific code.

**When to choose**
- You need precise control over **connection priority** (`BluetoothGatt.requestConnectionPriority(CONNECTION_PRIORITY_HIGH)`)
  — this maps to a 7.5–15 ms connection interval, the single biggest
  throughput lever on Android.
- You need 2M-PHY negotiation (`BluetoothGatt.setPreferredPhy()`).
- You want background OTA via a foreground service.
- You plan to also build a full telemetry-viewer app later.

**When to skip**
- You don't have an Android dev. Native Android is more code than
  cross-platform options for the same feature surface.

**Estimated effort**: ~1–2 weeks to a polished single-purpose app.

---

## Option E — PWA + Web Bluetooth

Chrome on Android exposes Web Bluetooth. A static HTML page hosted on
GitHub Pages could do the job.

**Pros**
- No app to install or publish.
- Easy to share via a URL.

**Cons**
- iOS Safari does not support Web Bluetooth. Android-only.
- File picker UX is clunky.
- No persistent permissions; reconnect on every visit.
- Background transfer not possible.

**Use it for**: a quick fallback or a "tools page" link, not a primary path.

---

## Option F — WiFi SoftAP + HTTP OTA (orthogonal to BLE)

This is the **fast path**. ESP32 hosts a WiFi access point named
`OpenDash-Setup`. Your phone connects, opens `http://192.168.4.1/`,
picks the `.bin`, uploads.

**Throughput**: ~100 kB/s realistic, so a 2.4 MB image flashes in
**~25 seconds**. That's 12× faster than BLE.

**Pros**
- No custom app at all — works from any phone browser.
- Order-of-magnitude faster than BLE.
- ESP-IDF has `esp_https_ota` / `esp_https_server` ready to go.

**Cons**
- Disables ESP-NOW while WiFi-SoftAP is active. The other nodes go
  offline during the OTA window.
- You're broadcasting an AP — needs a token/password or you're a target
  in the pits.
- More on-device code than BLE (HTTP server, multipart parser).
- Bigger firmware footprint (~+100 kB).

**Use it for**: garage updates and big diff drops. Keep BLE for the
trail where you don't want to bring up a noisy 2.4 GHz radio.

---

## Decision matrix

| Option | Dev effort | UX | Speed | Cross-platform | Recommended for |
|---|---|---|---|---|---|
| A. nRF Connect macros | None | Geek | Same as Linux | Android only | Today, validation |
| B. Flutter | 2–3 evenings | Good | Same as Linux | Yes | **Primary path** |
| C. React Native | 2–3 evenings | Good | Same as Linux | Yes | If RN team exists |
| D. Native Kotlin | 1–2 weeks | Best | Best (HIGH priority API) | Android only | Long-term polish |
| E. PWA / Web BLE | 1 day | OK | Same as Linux | Android only | Stretch goal / fallback |
| F. WiFi SoftAP HTTP | 1 week (device) + 0 (client) | Best | **~25 s** | Yes (browser) | **Garage drops** |

---

## Recommended rollout

| Phase | Action | Value delivered |
|---|---|---|
| **Phase 0** | Test the existing GATT service from nRF Connect on a phone. Record throughput. | Confirms protocol is Android-compatible. |
| **Phase 1** | Flutter app, Phase 0's GATT flow, one button per node, progress bar. | Pit-friendly OTA from a phone for the trail. |
| **Phase 2** | Add WiFi SoftAP OTA to firmware (kept separate from BLE path). Browser UI. | 12× faster garage drops; no app needed. |
| **Phase 3** *(optional)* | Native Android client if/when you want background OTA, version checking, signed firmware enforcement. | Polished pro experience. |

---

## Things the firmware should add regardless of client

These are independent of the Android plan but make any client better:

- **Firmware version reported in the advertisement** so a client can
  refuse to "upgrade" to the same version it's already running.
- **Image signature verification** before commit (`esp_secure_boot_v2`).
- **Optional pairing / passkey** to prevent random phones from
  triggering OTA when the pod is in WAITING mode.
- **Adv name suffix with last 4 of MAC** so multiple pods of the same
  role (e.g. two test rigs) don't collide on scan.

---

## References

- `bleak` (Linux client we already have): https://bleak.readthedocs.io
- Flutter `flutter_blue_plus`: https://pub.dev/packages/flutter_blue_plus
- Android `BluetoothGatt.requestConnectionPriority`:
  https://developer.android.com/reference/android/bluetooth/BluetoothGatt#requestConnectionPriority(int)
- ESP-IDF `esp_https_ota`:
  https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/esp_https_ota.html
- nRF Connect for Mobile:
  https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-mobile
