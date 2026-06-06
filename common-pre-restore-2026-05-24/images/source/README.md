<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Source Images

Place your original image files here for each display:

## Supported Image Types
- **Background images**: `background_<display>.jpg` or `background_<display>.png`
- **Splash screens**: `splash_<display>.jpg` or `splash_<display>.png`

Where `<display>` is one of: `center`, `leftright`, `gps`

## Examples
```
source/background_center.jpg    - Background for center display (800x480)
source/splash_center.png        - Boot splash for center display
source/background_leftright.jpg - Background for left/right gauges (480x480 round)
source/splash_gps.png           - Boot splash for GPS display (466x466 round)
```

## Auto-Scaling
The build system will automatically:
1. Scale images proportionally to fit display resolution (no stretching/skewing)
2. Maintain aspect ratio
3. Center the image if dimensions don't match exactly
4. Convert to RGB565 format for efficient storage
5. Generate C array headers for inclusion in firmware

## Manual Sizing (Optional)
If you want pixel-perfect control, provide images at exact resolution:
- Center: 800×480 pixels
- Left/Right: 480×480 pixels  
- GPS: 466×466 pixels

Images at exact size skip auto-scaling and use direct conversion.
