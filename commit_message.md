feat(ota): comprehensive BLE OTA hardening and full-fleet coverage

- Fixed LEFT SDK configuration for proper PPCP parameters (corrected prefix from CONFIG_BT_NIMBLE_PPCP_* to CONFIG_BT_NIMBLE_SVC_GAP_PPCP_*)
- Expanded ble_ota.py with new command-line options (--list, --device, --address, --scan-timeout, --chunk-size)
- Added full-fleet BLE OTA coverage for all 11 node families (center, left, right, gps, pod1, pod2, relay-4ch-hd, relay-8ch-a, relay-8ch-b, mos-4ch-a, mos-4ch-b)
- Improved OTA handler consistency and reliability across all nodes
- Enhanced throughput optimization with 2M PHY, larger ACL buffers, and proper connection parameters
- Fixed CENTER demo data path to ensure safe operation with live sensor data
- Updated documentation including BLE_OTA.md, wiki/ota-bluetooth.md, and comprehensive OTA troubleshooting guide

This release significantly improves the reliability and performance of the OTA system, bringing it to a production-ready state for all node types with consistent behavior and optimized throughput.