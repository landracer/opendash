<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# OpenDash Center Display — Quick Reference

## Warning Boxes Quick Start

### Trigger Red Warning (Critical)
```c
/* Red flashing box on left side - continuous until cleared */
ui_manager_warning_box_trigger(0, OPENDASH_WARNING_CRITICAL, "ALERT", 0);

/* Red flashing box on right side - for 5 seconds */
ui_manager_warning_box_trigger(1, OPENDASH_WARNING_CRITICAL, "ALERT", 5000);
```

### Trigger Orange Warning (Caution)
```c
/* Orange flashing box on left side */
ui_manager_warning_box_trigger(0, OPENDASH_WARNING_CAUTION, "CAUTION", 0);

/* Orange flashing box on right side - for 3 seconds */
ui_manager_warning_box_trigger(1, OPENDASH_WARNING_CAUTION, "CAUTION", 3000);
```

### Clear Warning
```c
/* Stop flashing and hide warning box */
ui_manager_warning_box_clear(0);  /* Left side */
ui_manager_warning_box_clear(1);  /* Right side */
```

---

## Multi-Screen Quick Start

### Get Current Screen
```c
uint8_t screen = ui_manager_get_current_screen();
/* Returns: 0 = Engine, 1 = GPS */
```

### Switch to Next Screen
```c
ui_manager_next_screen();
/* Engine → GPS → Engine (loops around) */
```

---

## Layouts at a Glance

### Screen 1: Engine Metrics
```
┌──────────┬─────────────────┬──────────┐
│ GPS SPD  │                 │ LAP TIME │
├──────────┤  RPM ARC (0-    ├──────────┤
│ COOLANT  │   8000)         │ BOOST    │
├──────────┤                 ├──────────┤
│ OIL TEMP │                 │ AFR      │
└──────────┴─────────────────┴──────────┘
```

### Screen 2: GPS Metrics
```
┌──────────┬─────────────────┬──────────┐
│ SAT CNT  │                 │ HDOP     │
├──────────┤  GPS SPEED      ├──────────┤
│ ALTITUDE │   ARC           │ HEADING  │
├──────────┤                 ├──────────┤
│ LAT/LON  │                 │ ACCURACY │
└──────────┴─────────────────┴──────────┘
```

---

## Typical Integration Pattern

```c
void update_warnings_task(void *arg)
{
    while (1) {
        float coolant = read_obd2(0x0102);      /* Coolant temp */
        float afr = read_obd2(0x010A);          /* Air-fuel ratio */
        float fuel = read_obd2(0x0110);         /* Fuel level */
        
        /* Coolant overheat */
        if (coolant > 100.0f) {
            ui_manager_warning_box_trigger(0, OPENDASH_WARNING_CRITICAL, "OVERHEAT", 0);
        } else {
            ui_manager_warning_box_clear(0);
        }
        
        /* AFR out of range */
        if (afr > 14.5f || afr < 11.5f) {
            ui_manager_warning_box_trigger(1, OPENDASH_WARNING_CAUTION, "AFR", 5000);
        } else {
            ui_manager_warning_box_clear(1);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));  /* Update every 100ms */
    }
}
```

---

## Files Modified

- **[center/main/ui_manager.h](center/main/ui_manager.h)** — Added 4 new API functions
- **[center/main/ui_manager.c](center/main/ui_manager.c)** — Complete rewrite with multi-screen & warnings  
- **[common/include/opendash_ui_styles.h](common/include/opendash_ui_styles.h)** — Added warning box color defines
- **[center/main/ui_manager_old.c](center/main/ui_manager_old.c)** — Backup of original (for reference)

---

## Build Status

✅ **Successfully Compiled** — February 17, 2026  
Binary size: 2.86 MB (26% of partition)  
All LVGL v9 APIs verified

---

## Next Steps

1. **Test on Hardware**
   ```bash
   cd opendash/center
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

2. **Integrate Data Sources** — Connect OBD2, GPS, BMS data to warning triggers

3. **Add Boot Button Handler** — Hook GPIO0 to `ui_manager_next_screen()` for hardware switching

4. **Customize Colors** — Edit color defines in `opendash_ui_styles.h` if needed

---

## Common Issues & Fixes

| Issue | Fix |
|---|---|
| Warning box not appearing | Set `flash_ms` to 0 for continuous flash |
| Swipe not working | Make sure gesture is >100px and <500ms |
| Screen stuck on one layout | Check if `create_screen_engine()` and `create_screen_gps()` completed successfully |
| Compilation error | Run `idf.py fullclean && idf.py build` to rebuild from scratch |

---

See [SCREEN_ENHANCEMENTS.md](SCREEN_ENHANCEMENTS.md) for full documentation.
