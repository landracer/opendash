# OpenDash Display Configuration Rules

## ST7262 Display Controller Configuration

### Display Specifications (from ST7262 Datasheet)
- **Resolution**: 800×480 pixels
- **Interface**: RGB 16-bit (RGB565)
- **Pixel Clock**: 23-27 MHz (recommended 25 MHz)
- **Display Mode**: SYNC-DE mode (HSYNC, VSYNC, DE, DCLK + data)

### Timing Parameters (ST7262 Page 52 - Parallel 24-bit RGB Interface)
| Parameter | Symbol | Min | Typ | Max | Unit | Notes |
|-----------|-------|------|---|--|----|----|
| HSYNC Pulse Width | Thw | 2 | 4 | 8 | DCLK | |
| HSYNC Back Porch | Thbp | 4 | 8 | 48 | DCLK | |
| HSYNC Front Porch | Thfp | 4 | 8 | 48 | DCLK | |
| VSYNC Pulse Width | Tvw | 2 | 4 | 8 | HSYNC | |
| VSYNC Back Porch | Tvbp | 4 | 8 | 12 | HSYNC | |
| VSYNC Front Porch | Tvfp | 4 | 8 | 12 | HSYNC | |

### Current Implementation (display_init.c)
```c
#define LCD_H_RES       800
#define LCD_V_RES       480
#define LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)  // 16 MHz (conservative)
```

Timing values (ST7262 Page 52 - using TYPICAL values):
```c
.hsync_pulse_width = 4,   // Datasheet: 2-4-8, using typical
.hsync_back_porch = 8,    // Datasheet: 4-8-48, using typical
.hsync_front_porch = 8,   // Datasheet: 4-8-48, using typical
.vsync_pulse_width = 4,   // Datasheet: 2-4-8, using typical
.vsync_back_porch = 8,    // Datasheet: 4-8-12, using typical
.vsync_front_porch = 8,   // Datasheet: 4-8-12, using typical
```

### Bounce Buffer Configuration
- **Required**: YES (fb_in_psram=true + num_fbs=2)
- **Size**: 20 lines × 800 px = 16000 px = 32 KB SRAM
- **Purpose**: Eliminates PSRAM-AHB contention causing DMA underrun

### Framebuffer Configuration
- **Number of FBs**: 2 (double buffer for tear-free vsync)
- **Location**: PSRAM (`fb_in_psram = true`)
- **Render Mode**: `LV_DISPLAY_RENDER_MODE_DIRECT`
- **LVGL Buffer Height**: 50 lines

### RGB Polarity Settings
- **pclk_active_neg**: true (DCLK polarity: negative)
- **hsync_active_neg**: false (HSYNC polarity: positive by default)
- **vsync_active_neg**: false (VSYNC polarity: positive by default)
- **de_active_neg**: false (DE polarity: positive by default)

### GPIO Pin Assignments (Waveshare ESP32-S3-Touch-LCD-4.3)
| Signal | GPIO | Notes |
|--------|------|----|
| HSYNC | 46 | |
| VSYNC | 3 | |
| DE | 5 | |
| PCLK | 7 | |
| DISP_EN | -1 | Not used |
| Data D0-D15 | See display_init.c | RGB565: B3,B4,B5,B6,B7,G2,G3,G4,G5,G6,G7,R3,R4,R5,R6,R7 |

### Pixel Clock Consideration
- **ST7262 spec**: 23-27 MHz (recommended 25 MHz)
- **Current implementation**: 16 MHz (conservative)
- **Reason**: 16 MHz chosen to avoid PSRAM bandwidth contention
- **Note**: Higher clock values may cause visual artifacts due to PSRAM-AHB bus arbitration issues

### Key Files
- `center/main/display_init.c` - Display hardware initialization
- `center/main/CMakeLists.txt` - Build configuration
- `docs/pdf/ST7262.pdf` - ST7262 datasheet reference

### References
- ST7262 Datasheet: https://files.waveshare.com/wiki/common/ST7262.pdf
- ESP32-S3 LCD API: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/lcd.html
- LVGL 9.x Documentation: https://docs.lvgl.io/9.2/

### Build Verification
After any display configuration changes:
1. Build: `cd center && idf.py build`
2. Flash: `idf.py -p /dev/ttyACM0 flash`
3. Monitor: `idf.py -p /dev/ttyACM0 monitor` (60+ seconds)
4. Verify: Display shows LVGL startup without artifacts

### Common Issues
| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| Bottom rolling tear | PSRAM bandwidth contention | Increase bounce buffer (try 30 * LCD_H_RES) |
| Visual noise/artifacts | High pixel clock | Decrease LCD_PIXEL_CLOCK_HZ (try 14 MHz) |
| Display misalignment | Incorrect timing values | Match ST7262 datasheet typical values |
| Splash screen hang | LVGL memory too high | Keep LVGL memory ≤ 128 KB total |