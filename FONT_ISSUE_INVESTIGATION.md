# Font Issue Investigation - LVGL Include Path Error (RESOLVED)

## Problem Statement (Clarified)
**Original report:** "We are still getting compile errors. Looks like the file engebold is there but it's looking for an extra .10 at the end of the file."

**Actual issue:** The error was `engebold_18.c:10:10` (line 10, column 10), not a ".10" file extension. The real error was:
```
fatal error: lvgl/lvgl.h: No such file or directory
   10 | #include "lvgl/lvgl.h"
```

## Root Cause

The `lv_font_conv` tool by default generates font files with this include structure:
```c
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"  // This was causing the error
#endif
```

The ESP-IDF build system was taking the `else` branch and trying to include `"lvgl/lvgl.h"`, but the LVGL component in ESP-IDF expects the simple include path `"lvgl.h"`.

## Solution

Modified `common/fonts/convert_fonts.py` to add the `--lv-include lvgl.h` parameter when calling `lv_font_conv`. This tells the tool to use the simple include path compatible with ESP-IDF:

```python
cmd = [
    lv_font_conv_cmd,
    '--font', str(ttf_path),
    '--size', str(size),
    '--bpp', str(bpp),
    '--format', 'lvgl',
    '--range', char_range,
    '--lv-include', 'lvgl.h',  # Added: Use simple include for ESP-IDF compatibility
    '--output', str(output_path)
]
```

Now all generated font files use `#include "lvgl.h"` in both branches, matching the rest of the OpenDash codebase.

## Verification

All existing OpenDash source files use the simple include:
- `common/include/opendash_fonts.h` → `#include "lvgl.h"`
- All display_init.c files → `#include "lvgl.h"`
- All ui_manager.c files → `#include "lvgl.h"`

Generated fonts now match this pattern.

## ✅ Current State - FIXED

**The include path issue has been resolved.** Font generation now produces ESP-IDF compatible files.

### What Changed

1. **Modified `common/fonts/convert_fonts.py`**: Added `--lv-include lvgl.h` parameter
2. **Regenerated all font files**: Now use correct include path
3. **Verified compatibility**: Matches existing OpenDash code style

### To Apply the Fix

Users experiencing this error should:

1. **Pull the latest changes** from this PR
2. **Regenerate fonts** (automatic during build, or manually):
   ```bash
   cd common/fonts
   python3 convert_fonts.py --force
   ```
3. **Clean and rebuild**:
   ```bash
   cd center  # or whichever project
   rm -rf build
   idf.py build
   ```

The error `fatal error: lvgl/lvgl.h: No such file or directory` should now be resolved.

## Files Modified in This Session

### Code Changes (Committed)
- **`common/fonts/convert_fonts.py`**: Added `--lv-include lvgl.h` parameter to fix include path
- **`FONT_ISSUE_INVESTIGATION.md`**: Updated with root cause analysis and solution

### Generated Files (Not Committed - Auto-generated at Build Time)
- `common/fonts/generated/engebold_14.c` ✓ Fixed
- `common/fonts/generated/engebold_18.c` ✓ Fixed
- `common/fonts/generated/engebold_32.c` ✓ Fixed
- `common/fonts/generated/montserrat_14.c` ✓ Fixed
- `common/fonts/generated/montserrat_18.c` ✓ Fixed
- `common/fonts/generated/montserrat_32.c` ✓ Fixed

All font files now correctly include `"lvgl.h"` instead of `"lvgl/lvgl.h"`.

## Summary

**Issue:** Font files generated with wrong LVGL include path (`"lvgl/lvgl.h"` instead of `"lvgl.h"`)  
**Root Cause:** Missing `--lv-include` parameter in font conversion command  
**Solution:** Modified `convert_fonts.py` to specify simple include path  
**Result:** ESP-IDF build now finds LVGL headers correctly  

The compilation error `fatal error: lvgl/lvgl.h: No such file or directory` is now resolved.
