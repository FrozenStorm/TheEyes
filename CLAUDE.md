# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP-IDF firmware for a Waveshare ESP32-S3 board with **two round 240x240 GC9A01 LCDs** (one per "eye"). It renders an animated pair of eyes with LVGL 8.3. Target: `esp32s3`, 16 MB flash, PSRAM enabled. The code started from a Waveshare demo, so it still contains `EXAMPLE_*` names, Chinese comments and some unused drivers.

## Build / flash

You need ESP-IDF in the environment (`IDF_PATH`, `idf.py` on PATH). The project builds with ESP-IDF **v6.1** (`C:\esp\v6.1\esp-idf`, installed via EIM). In a fresh PowerShell, load the environment with `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`. The code was originally written for IDF 5.x, so use IDF 6 APIs (e.g. `rgb_ele_order` instead of `rgb_endian` in `esp_lcd_panel_dev_config_t`).

```
idf.py set-target esp32s3     # only needed for a fresh build dir
idf.py build
idf.py -p COMx flash monitor
idf.py menuconfig             # project options are under "LCD Configuration" (main/Kconfig.projbuild)
```

There are no tests or linters. Managed components (LVGL, esp_lcd_gc9a01, esp_lcd_touch_cst816s) are declared in `main/idf_component.yml` and fetched into `managed_components/` at build time.

The config lives in `sdkconfig`. `sdkconfig.defaults` holds the baseline. Important settings: `CONFIG_LV_COLOR_DEPTH=16`, `CONFIG_LV_COLOR_16_SWAP=y`, `CONFIG_LV_MEM_CUSTOM=y` (so `lv_mem_alloc` uses the stdlib malloc), `CONFIG_LV_USE_CANVAS`, `CONFIG_LV_USE_GIF`. `partitions.csv` defines a 3 MB factory app plus `flash_test` (FAT) and `model` (SPIFFS) data partitions.

## Architecture

All code is in the single `main` component. Each new `.c` file must be added by hand to `SOURCES` in `main/CMakeLists.txt`, and each new directory to `INCLUDE_DIRS`. Nothing is globbed. The GC9A01A and CST816 sources are only added when `CONFIG_WAVESHARE_1_28INCH_TOUCH_LCD` is set.

Boot sequence (`main/main.c`): `BAT_Init` → `I2C_Init` → `LCD_INIT` → `LVGL_Init` → `Animated_Eye1` (builds the UI) → `LVGL_Start`.

- **Two displays on one SPI bus**: `LCD_Driver/GC9A01A/GC9A01A.c` sets up SPI2 once in `LCD_Init()`. `LCD2_Init()` then attaches a second panel IO on the same bus. The panels share DC, SCLK and MOSI and have separate CS, RST and backlight pins. The results are the globals `panel_handle` and `panel_handle2`. Each panel sets its own mirror/swap_xy for the chosen rotation, and the second panel is mirrored relative to the first. Pin definitions are in `GC9A01A.h`.
- **Backlights**: LEDC channels 0 and 1 share timer 0 (`LCD_Driver.c`). `Set_Backlight(0..100)` drives both.
- **LVGL** (`LVGL_Driver/LVGL_Driver.c`): two `lv_disp_t` objects, the globals `disp` and `disp2`. Each has its own pair of PSRAM draw buffers, `full_refresh = 1`, and the panel handle stored in `user_data`, so both share one `flush_cb`. The tick comes from a 2 ms `esp_timer`. `lv_timer_handler()` runs in its own task (`LVGL_Start`, core 1, priority 2, 10 ms period). LVGL calls are not mutex-protected, so do any UI work inside LVGL timers/callbacks, not from other tasks.
- **Eye UI** (`LVGL_UI/Animated_Eye1.c`): each eye is an `lv_canvas`, centered on its display's active screen (`tv` and `tv2`), with a buffer from `lv_mem_alloc`. A 50 ms `lv_timer` moves eye 1's pupil toward a random target that changes every `MOVE_INTERVAL`. Eye 2 copies eye 1's position, then both canvases are redrawn completely (sclera, iris, pupil, highlight). Tunables are `#define`s at the top of the file. `blink_state` and `blink_timer` exist but nothing uses them yet.

Present but inactive (compiled, not called from `main`):
- Touch (`TOUCH_INIT`, CST816 on I2C0 and I2C1, and the LVGL indev registration in `LVGL_Init`, which is commented out).
- `Wireless_Init` (WiFi/BLE scan).
- `lv_fs_fatfs_init` (FATFS drive `S:` for LVGL).
- `LVGL_UI/Eyes_Data/AA.c`, a large embedded GIF `lv_img_dsc_t AA`.

Hardware caveat (comment in `main.c`): battery ADC uses ADC1 channel 0, which is GPIO1. Disable `BAT_Init` if GPIO1 is ever used for another display.
