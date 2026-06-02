# TRMNL Module

This directory contains the app-facing TRMNL sleep-screen implementation.

- `TrmnlDisplayConfig.h` is portable, host-testable display sizing logic.
- `TrmnlSleepClient.h/.cpp` owns TRMNL `/api/display` fetch, image download,
  PNG-to-1-bit-BMP pre-bake into `/.crosspoint/trmnl_sleep.bmp`, and temporary Wi-Fi setup.
- Sleep display reads the baked BMP with `SleepActivity::renderBitmapSleepScreen()` (same path as book covers).

The rest of the app should call this module instead of duplicating TRMNL
network, cache, or orientation behavior. Porting to another fork should mainly
require adapting the service dependencies used by `TrmnlSleepClient.cpp`:
storage, HTTP download, Wi-Fi credential lookup, URL helpers, and logging.
