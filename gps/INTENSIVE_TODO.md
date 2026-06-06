<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash GPS Unit Firmware — Intensive Build TODO

> **⚠️ PARTIAL ARCHIVE:** This TODO was created before the GPS I2C protocol
> was fully understood. Some items are completed, some are superseded by
> the production v15L2 code. **The authoritative GPS reference is:**
> `wiki/LC76G-I2C-GPS-Driver-Guide.md` (Version 2.0.0, v15L2).
>
> I2C addresses corrected 2025-03: 0x28→**0x50** (write), 0x2A→**0x54** (read),
> **0x58** (data write) added. See wiki §2.3 for full address map.

## Phase 1: Research & Architecture Analysis

### 1.1 Center Display Pattern Study
- [ ] Read center display `display_init.c` (817 lines) for hardware init patterns
- [ ] Read center display `ui_manager.c` (639 lines) for 3-mode UI implementation
- [ ] Document button cycling mechanism from center display
- [ ] Identify LVGL version + configuration approach used in center
- [ ] Study center display's I2C bus setup and sensor polling strategy
- [ ] Review center display's task structure (update vs. render tasks)
- [ ] Note center display's locking/synchronization patterns

### 1.2 Waveshare BSP Hardware Discovery
- [ ] Research Waveshare ESP32-S3-Touch-AMOLED-1.75 pinout
- [ ] Identify touch controller: CST9217 (not CST816S)
- [ ] Map CST9217 I2C address (0x5A), RST (GPIO40), INT (GPIO11)
- [ ] Confirm GPS module: LC76G via I2C (not UART)
- [x] Map LC76G I2C addresses (write **0x50**, read **0x54**, data-write **0x58** at 100 kHz)
  - ~~0x28/0x2A were WRONG~~ — Corrected per CASIC protocol. See wiki §2.3.
- [ ] Identify IMU: QMI8658 at address 0x6B
- [ ] Map QMI8658 WHO_AM_I register (0x00 = 0x05)
- [ ] Confirm display: CO5300 QSPI AMOLED, 466×466 round, X gap = 6 px
- [ ] Map CO5300 QSPI pins: CS=12, SCLK=38, D0=4, D1=5, D2=6, D3=7, RST=39
- [ ] Identify CO5300 brightness control: register 0x51 (no PWM)
- [ ] Document shared I2C bus: GPIO15 (SDA) / GPIO14 (SCL) at 400 kHz
- [ ] List devices on shared I2C: touch, IMU, GPS, PMU, IO expander
- [ ] Extract CO5300 init command table from BSP source
- [ ] Document QSPI protocol details (opcode 0x02 for params, 0x32 for color)
- [ ] Note 2-pixel coordinate alignment requirement for QSPI

### 1.3 ESP-IDF 6.1 Capability Audit
- [ ] Verify ESP-IDF version: v6.1-dev-2441 (NOT 5.x)
- [ ] Check LVGL version: 9.2.2 (check 9.3+ API differences)
- [ ] Confirm USB component moved to component manager (not built-in)
- [ ] Research `esp_lvgl_adapter` compatibility with LVGL 9.2
- [ ] Map available I2C slave API in IDF 6.1 (no i2c_slave_transmit/receive)
- [ ] Document I2C slave callback signatures (on_receive, on_request)
- [ ] Verify touch component API: esp_lcd_touch_get_data vs deprecated get_coordinates
- [ ] Check SPI bus config struct for union field overlap issues

---

## Phase 2: BSP-Based Implementation (Failed Attempt)

### 2.1 BSP Dependency Setup
- [ ] Update `idf_component.yml` with Waveshare BSP dependency
- [ ] Add `lvgl/lvgl: "~9.xx.x"` constraint
- [ ] Configure `sdkconfig.defaults` for BSP build flags
- [ ] Set ESP-IDF target to ESP32-S3
- [ ] Configure partition table for flash layout

### 2.2 BSP Hardware Init Implementation
- [ ] Implement touch controller init via BSP
- [ ] Implement CO5300 QSPI panel init via BSP
- [ ] Implement display brightness control via BSP
- [ ] Integrate `esp_lvgl_adapter` for LVGL rendering
- [ ] Create boot button handler (GPIO0) via BSP

### 2.3 GPS Handler (BSP-Based)
- [ ] Get I2C handle from BSP (`bsp_i2c_get_handle()`)
- [ ] Implement LC76G I2C protocol (data length query + read)
- [ ] Implement NMEA $GPRMC parser (speed, heading, position, time, fix)
- [ ] Implement NMEA $GPGGA parser (altitude, satellites, HDOP)
- [ ] Create GPS polling task at 5 Hz on core 0

### 2.4 IMU Handler (BSP-Based)
- [ ] Get I2C handle from BSP
- [ ] Implement QMI8658 WHO_AM_I check (register 0x00)
- [ ] Implement QMI8658 reset and CTRL1-7 configuration
- [ ] Implement ±4g accel reading (8192 LSB/g conversion)
- [ ] Implement ±512 dps gyro reading (64 LSB/dps conversion)
- [ ] Implement G-force calculations (lateral, longitudinal, vertical)
- [ ] Implement pitch/roll angle computation
- [ ] Create IMU polling task at 100 Hz on core 0

### 2.5 UI Manager (BSP-Based, 3-Mode)
- [ ] Create GPS mode layout: speed (center), sats, coords, HDOP, heading, UTC time
- [ ] Create LAP mode layout: lap timer, delta-to-best, speed, auto-start detection
- [ ] Create GFORCE mode layout: total G (color-coded), breakdown, gyro, pitch/roll
- [ ] Implement boot button cycling through modes
- [ ] Create UI update task (200 ms, core 0)
- [ ] Create UI render task (10 ms lv_timer_handler, core 1)

### 2.6 I2C Slave Node (BSP-Based)
- [ ] Determine I2C port for inter-node bus (separate from onboard)
- [ ] Implement I2C slave at address 0x12
- [ ] Implement SET_DATA_POINT command handler
- [ ] Implement SET_BRIGHTNESS command handler
- [ ] Implement REQUEST_DATA (GPS/IMU response builder)
- [ ] Implement SYSTEM commands (ping, reboot)
- [ ] Implement OpenDash protocol (SYNC + CMD + LENGTH + PAYLOAD + CHECKSUM)

### 2.7 Main Entry Point (BSP-Based)
- [ ] Initialize NVS
- [ ] Load display configuration
- [ ] Call display_init (BSP path)
- [ ] Initialize GPS (get I2C from BSP)
- [ ] Initialize IMU (get I2C from BSP)
- [ ] Initialize UI manager
- [ ] Initialize I2C slave node
- [ ] Start all tasks
- [ ] Create health monitoring loop (10s heartbeat)

### 2.8 Build BSP Version
- [ ] Run `idf.py build`
- [ ] **EXPECTED RESULT**: Build fails — `esp_lvgl_adapter` incompatible with LVGL 9.2
- [ ] Document error: `lv_event_get_invalidated_area()` doesn't exist in 9.2 (added in 9.3)

---

## Phase 3: Pivot to Direct LVGL Integration (No BSP)

### 3.1 Hardware Extraction from BSP
- [ ] Extract CO5300 init command table from BSP source
- [ ] Extract all pin definitions
- [ ] Extract SPI bus configuration
- [ ] Extract I2C bus configuration
- [ ] Extract CST9217 touch configuration
- [ ] Extract CO5300 vendor commands and reset sequence
- [ ] Document register 0x51 brightness ramping
- [ ] Extract QSPI command opcode encoding (0x02, 0x32)

### 3.2 New Component Dependencies
- [ ] Update `idf_component.yml`: remove BSP
- [ ] Add `espressif/esp_lcd_co5300: "^2.0.3"`
- [ ] Add `waveshare/esp_lcd_touch_cst9217: "^1.0.4"`
- [ ] Add `espressif/esp_lcd_panel_io_additions: "^1.0.1"`
- [ ] Keep `lvgl/lvgl: "~9.2.0"` (locked constraint)
- [ ] Remove `usb` component (no longer needed)

### 3.3 CMakeLists.txt & sdkconfig Updates
- [ ] Update CMakeLists.txt to list all SRCS: main.c, display_init.c, gps_handler.c, imu_handler.c, ui_manager.c, i2c_node.c
- [ ] Update REQUIRES: common, lvgl__lvgl, driver, esp_lcd, esp_timer, esp_driver_gpio, esp_driver_spi, esp_driver_i2c
- [ ] Update sdkconfig.defaults: remove BSP-specific entries, keep I2C/LCD/SD relevant entries
- [ ] Verify partition table for 1 MB app partition

### 3.4 New display_init.c (Direct LVGL)
- [ ] **I2C Bus Init (i2c_bus_init)**:
  - I2C_NUM_1, SDA=GPIO15, SCL=GPIO14, 400 kHz master bus (shared by all onboard sensors)
  - Expose via `display_get_i2c_handle()`
  
- [ ] **SPI Bus Init (lcd_panel_init)**:
  - SPI2_HOST, 40 MHz QSPI
  - Set data0=4, data1=5, data2=6, data3=7, SCLK=38
  - CS=12, RST=39, no DC pin (QSPI mode)
  - Use SPI_DMA_CH_AUTO, SPICOMMON_BUSFLAG_QUAD
  
- [ ] **CO5300 Panel Config**:
  - Use `esp_lcd_co5300.h` component
  - Set vendor init commands (page switch, RGB565, brightness enable, column/row address, sleep out, display ON)
  - Panel create, reset, init
  - Set gap: x_offset=6, y_offset=0
  - Display ON
  
- [ ] **CST9217 Touch Handler (touch_init)**:
  - Address 0x5A, RST=GPIO40, INT=GPIO11
  - Use I2C handle from onboard bus
  - Enable mirror_x=1, mirror_y=1, swap_xy=0
  - Use `esp_lcd_touch_cst9217.h`
  - Create input device for LVGL
  
- [ ] **LVGL Direct Init (lvgl_init_direct)**:
  - Create display: `lv_display_create()` with 466×466 resolution
  - RGB565 color format, 16-bit
  - Set flush callback: `esp_lcd_panel_draw_bitmap()` to CO5300
  - Set rounder callback: align all coordinates to 2-pixel boundaries
  - Create double-buffered draw buffers in PSRAM (50 lines per buffer)
  - Create LVGL mutex for thread safety
  - Setup tick timer via esp_timer (2 ms period)
  - Create LVGL display handle
  
- [ ] **Boot Button (boot_button_init, button_read_task)**:
  - GPIO0 with internal pullup
  - Debounce 50 ms
  - Task reads button state, triggers UI mode cycling
  - Rate: 50 Hz polling
  
- [ ] **Touch Reading Task (touch_read_task)**:
  - Poll CST9217 at 50 Hz
  - Use new API: `esp_lcd_touch_get_data()` + `esp_lcd_touch_point_data_t`
  - Update global touch_x, touch_y, touch_pressed
  - Lock LVGL during update
  
- [ ] **Brightness Control (display_set_brightness)**:
  - Map 0-100% to 0-255 register value
  - Write register 0x51 via QSPI command encoding: (LCD_OPCODE_WRITE_CMD << 24) | (0x51 << 8)
  - Use `esp_lcd_panel_io_tx_param()`
  
- [ ] **Public API Functions**:
  - `display_init()` — orchestrate all init, start tasks
  - `display_get_i2c_handle()` — return i2c_bus_handle
  - `display_lvgl_lock(timeout_ms)` — acquire mutex
  - `display_lvgl_unlock()` — release mutex
  - `display_get_lvgl_disp()` — return lvgl_disp pointer
  - `display_set_brightness(0-100)`
  - `display_get_brightness()`

### 3.5 New display_init.h (Public API Definitions)
- [ ] `#define GPS_LCD_H_RES 466`
- [ ] `#define GPS_LCD_V_RES 466`
- [ ] Function declarations for all public API
- [ ] Include `driver/i2c_master.h` for bus handle type
- [ ] Include `lvgl.h` for display type

### 3.6 New gps_handler.c (LC76G I2C Protocol)
- [ ] **I2C Device Setup**:
  - Get I2C handle from `display_get_i2c_handle()`
  - Create **three** slave devices: write addr **0x50**, read addr **0x54**, data-write addr **0x58** at 100 kHz
  - ~~0x28/0x2A were WRONG~~ — Corrected. Plus 0x58 is REQUIRED for WAKE mechanism.
  
- [ ] **LC76G Protocol Implementation**:
  - Query data length: read 8-byte command response
  - Receive 4-byte LE uint32 data length
  - Send read command
  - Read up to 256 bytes of NMEA ASCII
  
- [ ] **NMEA Parser**:
  - Parse $GPRMC: status, time (HHMMSS.SS), latitude, N/S, longitude, E/W, speed, heading, date
  - Parse $GPGGA: time, latitude, N/S, longitude, E/W, fix quality, satellites, HDOP, altitude, geoid height
  - Validate checksums
  - Convert strings to float values
  - Store in gps_data_t struct
  
- [ ] **GPS Task (gps_handler_task)**:
  - Poll at 5 Hz on core 0
  - Call `gps_handler_get_data()` for current data
  
- [ ] **Public API**:
  - `gps_handler_init()` — create I2C devices, validate communication
  - `gps_handler_start()` — create task
  - `gps_handler_get_data(gps_data_t *out)` — copy current GPS data

### 3.7 New gps_handler.h (Data Structure & Declarations)
- [ ] Define `gps_data_t` struct:
  - latitude, longitude, altitude (float)
  - speed, heading (float)
  - satellites, hdop, accuracy (float)
  - fix_valid, fix_quality (bool/uint8)
  - hour, minute, second (uint8)
- [ ] Function declarations
- [ ] Include necessary headers

### 3.8 New imu_handler.c (QMI8658 Register Driver)
- [ ] **I2C Device Setup**:
  - Get I2C handle from `display_get_i2c_handle()`
  - Create slave device at address 0x6B
  
- [ ] **Register Read/Write Helpers**:
  - `write_reg(reg, value)`
  - `read_reg(reg, &value)`
  - `read_regs(reg, buffer, len)`
  
- [ ] **QMI8658 Initialization (imu_configure)**:
  - Read WHO_AM_I (register 0x00), expect 0x05
  - Initialize CTRL1-CTRL7 registers
  - Set ±4g accel range
  - Set ±512 dps gyro range
  - Set 100 Hz ODR
  - Enable accel and gyro
  
- [ ] **Sensor Data Reading (read_sensor_data)**:
  - Read 12 bytes of accel/gyro data
  - Parse X, Y, Z accel (int16 each)
  - Parse X, Y, Z gyro (int16 each)
  - Convert accel: value / 8192 = G
  - Convert gyro: value / 64 = dps
  
- [ ] **Derived Calculations**:
  - Total G: sqrt(x² + y² + z²)
  - Lateral G: Y component
  - Longitudinal G: X component
  - Vertical G: Z component
  - Pitch: atan2(accel_x, sqrt(accel_y² + accel_z²))
  - Roll: atan2(accel_y, accel_z)
  
- [ ] **IMU Task (imu_handler_task)**:
  - Poll at 100 Hz on core 0
  
- [ ] **Public API**:
  - `imu_handler_init()` — init registers, validate WHO_AM_I
  - `imu_handler_start()` — create task
  - `imu_handler_get_data(imu_data_t *out)` — copy current IMU data

### 3.9 New imu_handler.h (Data Structure & Declarations)
- [ ] Define `imu_data_t` struct:
  - accel_x, accel_y, accel_z (float)
  - gyro_x, gyro_y, gyro_z (float)
  - g_lateral, g_longitudinal, g_vertical (float)
  - total_g (float)
  - pitch, roll (float)
- [ ] Function declarations

### 3.10 New ui_manager.c (3-Mode LVGL UI)
- [ ] **Mode Enum & Layout**:
  - GPS mode (0), LAP mode (1), GFORCE mode (2)
  - Common layout: status_label (top), primary_value + unit (center large), info_panel (4 lines), bottom_left/right, mode_indicator
  
- [ ] **GPS Mode Display Logic (update_gps_mode)**:
  - Primary value: speed (in appropriate unit)
  - Status: "GPS ACTIVE" or "NO FIX"
  - Info lines: [SAT COUNT] [LAT] [LON] [HDOP] [HEADING] [UTC TIME]
  
- [ ] **LAP Mode Display Logic (update_lap_mode)**:
  - Primary value: lap timer (HH:MM:SS)
  - Status: "LAP TIMER"
  - Info lines: [DELTA TO BEST] [CURRENT SPEED] [AVG SPEED] [LAP COUNT]
  - Auto-start on movement (accel > threshold)
  
- [ ] **GFORCE Mode Display Logic (update_gforce_mode)**:
  - Primary value: total G with color coding (green < 0.5G, yellow < 1.5G, red >= 1.5G)
  - Status: "G-FORCE MONITOR"
  - Info lines: [LATERAL] [LONGITUDINAL] [VERTICAL] [PITCH/ROLL]
  - Continuous gyro display
  
- [ ] **UI Update Task (ui_update_task)**:
  - 200 ms period on core 0
  - Poll GPS data via `gps_handler_get_data()`
  - Poll IMU data via `imu_handler_get_data()`
  - Call mode-specific update function
  - Call `update_value()` with dp_id for OpenDash integration
  
- [ ] **UI Render Task (ui_render_task)**:
  - 10 ms period (100 Hz) on core 1
  - Call `lv_timer_handler()` with lock held
  
- [ ] **Boot Button Integration**:
  - Listen to button events
  - Cycle through modes on press
  - Update `current_mode` and redraw
  
- [ ] **Public API**:
  - `ui_manager_init(layout_config)` — configure layout from NVS
  - `ui_manager_start()` — create tasks
  - `ui_manager_update_value(dp_id, value)` — for OpenDash data points
  - `ui_manager_next_screen()` — cycle mode
  - `ui_manager_set_display_mode(mode)`
  - `ui_manager_get_current_screen()` — return current mode

### 3.11 New ui_manager.h (API & Structures)
- [ ] Define `gps_display_mode_t` enum
- [ ] Define layout structures for different modes
- [ ] Function declarations

### 3.12 New i2c_node.c (Callback-Based I2C Slave)
- [ ] **I2C Slave Configuration**:
  - Port: I2C_NUM_0 (per OpenDash protocol header)
  - Slave address: 0x12
  - SDA: GPIO17 (NOTE: GPIO15 used by onboard bus)
  - SCL: GPIO16 (per protocol header)
  - 400 kHz
  - send_buf_depth and receive_buf_depth set to OPENDASH_MSG_MAX_SIZE
  
- [ ] **ISR Callbacks**:
  - `on_receive` callback: copies incoming data to FreeRTOS queue
  - `on_request` callback: (fallback only, responses queued proactively)
  
- [ ] **RX Event Structure**:
  - Typedef struct with data buffer[256] and length
  - FreeRTOS queue of 8 events
  
- [ ] **Message Processing (process_message)**:
  - Handle OPENDASH_CMD_SET_DATA_POINT: push to UI
  - Handle OPENDASH_CMD_SET_BRIGHTNESS: call `display_set_brightness()`
  - Handle OPENDASH_CMD_REQUEST_DATA: build GPS or IMU response
    - Build via `build_gps_response()`: dp_id (2) + value (4) + timestamp (4)
    - Build via `build_imu_response()`: dp_id (2) + value (4) + timestamp (4)
  - Handle OPENDASH_CMD_SYSTEM: ping/reboot
  
- [ ] **TX Response Queueing**:
  - Serialize message via `opendash_i2c_serialize()`
  - Call `i2c_slave_write(slave_handle, tx_buffer, tx_len, &written_len, 100)`
  
- [ ] **I2C Node Task (i2c_node_task)**:
  - Wait on rx_queue with 500 ms timeout
  - Deserialize message
  - Call `process_message()`
  - Run on core 0
  
- [ ] **Public API**:
  - `i2c_node_init()` — create I2C slave, queue, register callbacks (NON-FATAL on failure)
  - `i2c_node_start()` — create task (only if init succeeded)

### 3.13 New i2c_node.h (API Declarations)
- [ ] Function declarations: `i2c_node_init()`, `i2c_node_start()`

### 3.14 Updated main.c (Complete Init Order)
- [ ] **Step 1**: Initialize NVS (`nvs_flash_init()`)
- [ ] **Step 2**: Load config (`opendash_config_load()`)
- [ ] **Step 3**: Call `display_init()` (initializes I2C master, QSPI, touch, LVGL)
- [ ] **Step 4**: Call `gps_handler_init()` (get I2C handle, create slave devices)
- [ ] **Step 5**: Call `imu_handler_init()` (get I2C handle, init registers)
- [ ] **Step 6**: Call `ui_manager_init()` (create LVGL display objects)
- [ ] **Step 7**: Call `i2c_node_init()` — **non-fatal**, log warning if fails
- [ ] **Step 8**: Start all tasks
  - `gps_handler_start()`
  - `imu_handler_start()`
  - `ui_manager_start()`
  - `if (i2c_node_ok) { i2c_node_start(); }` — only if init succeeded
- [ ] **Main Loop**: 10-second health monitoring, log uptime/GPS fix/satellites/mode

### 3.15 Build Direct Implementation
- [ ] Run `rm -rf build managed_components`
- [ ] Run `idf.py set-target esp32s3`
- [ ] Run `idf.py build`
- [ ] **EXPECTED RESULT**: Configuration succeeds, 7 managed components resolved

---

## Phase 4: Build Iteration 1 (I2C Slave API)

### 4.1 Identify Compilation Error
- [ ] Build fails: `i2c_slave_transmit()` and `i2c_slave_receive()` don't exist
- [ ] Research ESP-IDF 6.1 I2C slave API
- [ ] **FINDING**: API is callback-based, not polling-based

### 4.2 ESP-IDF 6.1 I2C Slave API Research
- [ ] Document available functions:
  - `i2c_new_slave_device()` ✓ (exists)
  - `i2c_del_slave_device()` ✓ (exists)
  - `i2c_slave_write()` ✓ (exists) — signature: (handle, data, len, &write_len, timeout_ms)
  - `i2c_slave_register_event_callbacks()` ✓ (exists)
  - `i2c_slave_reset_tx_fifo()` ✓ (exists for non-ESP32)
  - `i2c_slave_transmit()` ✗ (doesn't exist)
  - `i2c_slave_receive()` ✗ (doesn't exist)
- [ ] Document callback types:
  - `i2c_slave_received_callback_t on_receive` — params: (handle, const i2c_slave_rx_done_event_data_t*, user_data)
  - `i2c_slave_request_callback_t on_request` — params: (handle, const i2c_slave_request_event_data_t*, user_data)
  - Both return bool (whether high-priority task woken)
- [ ] Document event data structures:
  - `i2c_slave_rx_done_event_data_t`: buffer (uint8_t*), length (uint32_t)
  - `i2c_slave_request_event_data_t`: (empty struct)
- [ ] Document `i2c_slave_event_callbacks_t` struct: {on_request, on_receive}

### 4.3 Rewrite i2c_node.c for Callback API
- [ ] Remove polling task loop
- [ ] Add RX queue structure: {data[256], length}
- [ ] Create FreeRTOS queue: `xQueueCreate(8, sizeof(i2c_rx_event_t))`
- [ ] Implement `on_receive` callback:
  - IRAM_ATTR function (ISR context)
  - Copy received data to queue event struct
  - Send event to queue via `xQueueSendFromISR()`
  - Return whether high-priority task woken
- [ ] Implement `on_request` callback:
  - IRAM_ATTR function (ISR context)
  - Fallback only (responses are pre-queued)
  - Return false
- [ ] Register callbacks in `i2c_node_init()`:
  - Fill `i2c_slave_event_callbacks_t` struct
  - Call `i2c_slave_register_event_callbacks(slave_handle, &cbs, NULL)`
- [ ] Rewrite `i2c_node_task()`:
  - Loop: `xQueueReceive(rx_queue, &event, timeout)`
  - Deserialize message from queue event data
  - Call `process_message()`
- [ ] Replace all `i2c_slave_transmit()` calls with `i2c_slave_write()`:
  - Signature: `i2c_slave_write(slave_handle, tx_buffer, tx_len, &write_len, 100)`
  - Store return value in `written` variable
  - Log `write_len` in debug output
- [ ] Add `receive_buf_depth` to slave config

### 4.4 Build Iteration 1
- [ ] Run `idf.py build`
- [ ] **EXPECTED RESULT**: Clean build, 553 KB binary (47% partition free)

---

## Phase 5: Flash & Monitor (Boot Loop Issue)

### 5.1 Flash Firmware
- [ ] Run `idf.py -p /dev/ttyACM0 flash monitor`
- [ ] **OBSERVED**: Boot loop — Guru Meditation: LoadProhibited crash

### 5.2 Diagnose Root Causes
- [ ] Capture serial output with timeout
- [ ] Search for error patterns: "Guru Meditation", "panic", "LoadProhibited"
- [ ] Found: Same backtrace every reboot
  - `xPortEnterCriticalTimeout` → `xQueueReceive` crash
  - LoadProhibited on address 0x0000004c
- [ ] Review boot log: System reaches step 7 (I2C node init)
- [ ] Found error log: `E (2544) i2c.common: I2C bus id(1) has already been acquired`

### 5.3 Root Cause 1: I2C Port Conflict
- [ ] **Problem**: Both display_init.c and i2c_node.c use I2C_NUM_1
- [ ] **Evidence**: Error log "I2C bus id(1) has already been acquired"
- [ ] **Impact**: i2c_node_init() fails with ESP_ERR_INVALID_STATE
- [ ] **Solution**: Change i2c_node.c to use I2C_NUM_0 (per OpenDash protocol header)

### 5.4 Root Cause 2: GPIO15 Pin Conflict
- [ ] **Problem**: i2c_node logs report "SDA: GPIO15, SCL: GPIO16"
- [ ] **Issue**: But display_init.c also uses GPIO15 for onboard I2C bus
- [ ] **Evidence**: Two different I2C buses cannot share SDA pin
- [ ] **Solution**: Override i2c_node SDA to GPIO17 (free on this board)
- [ ] **Note**: SCL remains GPIO16 per OpenDash protocol header

### 5.5 Root Cause 3: NULL Queue Crash
- [ ] **Problem**: i2c_node_init() fails → rx_queue never created
- [ ] **Evidence**: i2c_node_task starts anyway, calls `xQueueReceive(NULL, ...)`
- [ ] **Impact**: LoadProhibited on NULL pointer dereference in FreeRTOS queue code
- [ ] **Solution**: 
  - Make i2c_node_init() non-fatal (don't ESP_ERROR_CHECK)
  - Record success in bool flag
  - Only call i2c_node_start() if init succeeded
  - Log warning if init fails (inter-node comms optional)

### 5.6 Additional Issues Found
- [ ] **SPI GPIO0 Warnings**: Unused pins default to GPIO0 due to union zero-init
  - Cause: Not setting mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num to -1
  - Fix: Use only `data*_io_num` field names in struct initializer (avoid union double-init warnings)
- [ ] **Deprecated Touch API**: `esp_lcd_touch_get_coordinates()` marked for removal in 2.0.0
  - Replace with `esp_lcd_touch_get_data()` + `esp_lcd_touch_point_data_t` struct
  - Update touch_read_task accordingly

---

## Phase 6: Final Fixes

### 6.1 Fix i2c_node.c Port & GPIO

**In i2c_node.c definitions:**
- [ ] Change `#define I2C_NODE_PORT I2C_NUM_0` (was I2C_NUM_1)
- [ ] Add `#define I2C_NODE_SDA_PIN GPIO_NUM_17` (override GPIO15)
- [ ] Add `#define I2C_NODE_SCL_PIN OPENDASH_I2C_SCL_PIN` (GPIO16)
- [ ] Update log message in `i2c_node_init()` to show correct GPIO and port

**In i2c_node.c slave config:**
- [ ] Change `.i2c_port = I2C_NODE_PORT` (now 0)
- [ ] Change `.sda_io_num = I2C_NODE_SDA_PIN` (now 17)
- [ ] Change `.scl_io_num = I2C_NODE_SCL_PIN` (now 16)
- [ ] Add `.receive_buf_depth = I2C_NODE_RX_BUF_SIZE`

### 6.2 Fix SPI Bus Config (GPIO0 Warnings)

**In display_init.c lcd_panel_init():**
- [ ] Reorder struct initializer to match actual struct layout:
  - `.data0_io_num = LCD_PIN_DATA0` (data0/mosi union)
  - `.data1_io_num = LCD_PIN_DATA1` (data1/miso union)
  - `.sclk_io_num = LCD_PIN_SCLK` (sclk is separate)
  - `.data2_io_num = LCD_PIN_DATA2` (data2/quadwp union)
  - `.data3_io_num = LCD_PIN_DATA3` (data3/quadhd union)
  - DO NOT set mosi/miso/quadwp/quadhd fields (to avoid union double-init warnings)
- [ ] Add comment explaining the union layout

### 6.3 Fix Deprecated Touch API

**In display_init.c touch_read_task():**
- [ ] Replace variable declarations:
  - Old: `uint16_t x[1], y[1]; uint16_t strength[1]; uint8_t count = 0;`
  - New: `esp_lcd_touch_point_data_t points[1]; uint8_t count = 0;`
- [ ] Replace API call:
  - Old: `bool got_touch = esp_lcd_touch_get_coordinates(touch_handle, x, y, strength, &count, 1);`
  - New: `esp_err_t ret = esp_lcd_touch_get_data(touch_handle, points, &count, 1);`
- [ ] Update condition:
  - Old: `if (got_touch && count > 0) { ... touch_x = x[0]; touch_y = y[0]; ... }`
  - New: `if (ret == ESP_OK && count > 0) { ... touch_x = points[0].x; touch_y = points[0].y; ... }`

### 6.4 Fix main.c (Non-Fatal I2C Node Init)

**In main.c Step 7:**
- [ ] Replace:
  ```c
  ESP_LOGI(TAG, "Initializing I2C slave node (addr 0x12)...");
  ESP_ERROR_CHECK(i2c_node_init());
  ESP_LOGI(TAG, "[7/8] I2C slave node initialized");
  ```
- [ ] With:
  ```c
  ESP_LOGI(TAG, "Initializing I2C slave node (addr 0x12)...");
  bool i2c_node_ok = (i2c_node_init() == ESP_OK);
  if (i2c_node_ok) {
      ESP_LOGI(TAG, "[7/8] I2C slave node initialized");
  } else {
      ESP_LOGW(TAG, "[7/8] I2C slave node FAILED — inter-node comms disabled");
  }
  ```

**In main.c Step 8:**
- [ ] Replace:
  ```c
  i2c_node_start();
  ```
- [ ] With:
  ```c
  if (i2c_node_ok) {
      i2c_node_start();
  }
  ```

---

## Phase 7: Build & Verify

### 7.1 Rebuild After All Fixes
- [ ] Run `idf.py build`
- [ ] **EXPECTED RESULT**: 
  - Clean build
  - No warnings (except possibly deprecated API notes)
  - SPI bus config properly initializes
  - Touch API compiles correctly
  - I2C node compiles with callback API

### 7.2 Flash Firmware
- [ ] Run `idf.py -p /dev/ttyACM0 flash`
- [ ] Verify flash completes successfully

### 7.3 Monitor Serial Output (Verify Clean Boot)
- [ ] Run `idf.py -p /dev/ttyACM0 monitor`
- [ ] **EXPECTED SEQUENCE**:
  ```
  [1/8] NVS initialized
  [2/8] Configuration loaded
  [3/8] Display initialized (CO5300 QSPI + CST9217 touch)
  [4/8] GPS module initialized
  [5/8] IMU sensor initialized
  [6/8] UI manager initialized (GPS / LAP / GFORCE modes)
  [7/8] I2C slave node initialized
  [8/8] All tasks running
  System ready!
  ```
- [ ] **NO CRASHES** — No "Guru Meditation" errors, no resets
- [ ] Verify heartbeat logs appear (10-second intervals):
  ```
  Uptime: 10s | GPS fix: NO | Sats: 0 | Mode: 0
  ```

### 7.4 Functional Verification
- [ ] Press boot button (GPIO0) — verify UI cycles through modes (GPS → LAP → GFORCE → GPS)
- [ ] Observe LVGL rendering to QSPI AMOLED display
- [ ] Verify touch input works (if CST9217 is responsive)
- [ ] Monitor GPS task logs — should show regular I2C polling attempts
- [ ] Monitor IMU task logs — should show regular polling and G-force calculations
- [ ] Verify 10-second heartbeat continues without interruption

---

## Phase 8: Integration & Testing

### 8.1 Verify Hardware Integration
- [ ] GPS data appears in logs (even if satellites = 0 initially)
- [ ] IMU data updates with accelerometer readings
- [ ] Display brightness responds to SET_BRIGHTNESS commands (if I2C node connected)
- [ ] Boot button LED indicators (if present) respond

### 8.2 Verify OpenDash I2C Protocol
- [ ] Connect center display as I2C master
- [ ] Send SET_DATA_POINT commands — verify GPS updates UI
- [ ] Send REQUEST_DATA commands — verify GPS node responds with float values
- [ ] Send SYSTEM ping — verify status response
- [ ] Send SET_BRIGHTNESS — verify display brightness changes

### 8.3 Test GPS Acquisition
- [ ] Allow system to run for 1+ minute outdoors
- [ ] Monitor GPS logs — watch for satellite acquisition
- [ ] Verify NMEA sentence parsing (lat/lon/speed/heading updates)
- [ ] Test different GPS positions if possible

### 8.4 Test IMU Responsiveness
- [ ] Tilt device and observe GFORCE mode readings
- [ ] Verify G-force calculations are reasonable (should be ~1G at rest in any orientation)
- [ ] Verify gyro readings respond to rotation
- [ ] Check pitch/roll angles are reasonable

### 8.5 Stress Testing
- [ ] Let system run for extended period (hours)
- [ ] Monitor for crashes, memory leaks, task hangs
- [ ] Verify heap remains stable
- [ ] Check CPU core utilization is reasonable

---

## Phase 9: Documentation & Cleanup

### 9.1 Code Documentation
- [ ] Add file headers to all .c/.h files
- [ ] Add function documentation (parameters, return values, examples)
- [ ] Add inline comments for complex algorithms (NMEA parsing, G-force math, QSPI protocol)
- [ ] Document I2C addresses and GPIO pins in comments

### 9.2 README Updates
- [ ] Document hardware pinout
- [ ] Document I2C devices (onboard bus vs. inter-node bus)
- [ ] Document GPS protocol (LC76G I2C)
- [ ] Document IMU registers and calculations
- [ ] Document OpenDash I2C protocol (this node's role)
- [ ] Document 3 display modes and controls
- [ ] Document build instructions and dependencies

### 9.3 Configuration Documentation
- [ ] Document sdkconfig.defaults settings
- [ ] Document component manager dependencies
- [ ] Document ESP-IDF version requirement (6.1-dev)
- [ ] Document LVGL configuration (9.2.2, RGB565, draw buffer size)

### 9.4 Performance Profiling
- [ ] Measure task CPU usage:
  - GPS task (5 Hz, core 0)
  - IMU task (100 Hz, core 0)
  - UI update (200 ms, core 0)
  - UI render (10 ms, core 1)
  - I2C node task (event-driven, core 0)
- [ ] Measure memory usage:
  - Stack sizes for all tasks
  - Heap fragmentation
  - PSRAM usage (draw buffers)
- [ ] Verify no priority inversion or starvation

### 9.5 Final Checklist
- [ ] All 8 init steps pass ✓
- [ ] No crash loops ✓
- [ ] No compiler warnings (as errors) ✓
- [ ] Clean serial output ✓
- [ ] Hardware responsive (display, button, touch) ✓
- [ ] GPS task running ✓
- [ ] IMU task running ✓
- [ ] UI manager cycling modes ✓
- [ ] I2C slave node initialized (or gracefully skipped) ✓
- [ ] 10-second health monitoring working ✓
- [ ] Binary size acceptable (<1 MB partition) ✓

---

## Summary Stats
- **Total Tasks**: 150+
- **Files Modified/Created**: 11 core files
- **Lines of Code**: ~3500 total
- **Build Iterations**: 2 major pivots, 4 build attempts, multiple fix cycles
- **Problems Encountered & Resolved**: 4 (BSP incompatibility, I2C port conflict, GPIO pin conflict, NULL queue crash)
- **Hardware Devices Integrated**: 5 (display, touch, GPS, IMU, I2C slave)
- **Display Modes**: 3 (GPS, LAP, GFORCE)
- **Task Execution**: 6 concurrent tasks (GPS, IMU, UI update, UI render, touch, I2C node, button)
- **Communication Protocols**: 4 (QSPI to display, I2C to sensors, OpenDash serial on I2C, FreeRTOS IPC)

