# ATTRIBUTION — nanojs8_ft8_lib

Component: `components/nanojs8_ft8_lib/`
Layer:     NanoJS8 v0.7 L7.5+ (sync detection)

## Upstream

Mini-FT8 — https://github.com/Y4HL/Mini-FT8 (an ESP-IDF/M5Stack port of
Karlis Goba's ft8_lib). Lifted from `components/ft8_lib/`. Original
ft8_lib by Karlis Goba (YL3JG), CC0-style permissive license.

## What we vendored

```
ft8/constants.c, .h   ft8/decode.c, .h    ft8/encode.c, .h
ft8/ldpc.c, .h         ft8/message.c, .h   ft8/text.c, .h
ft8/crc.c, .h          ft8/debug.h
common/monitor.c, .h   common/fft_wrapper.c, .h
common/common.h        common/stpcpy_compat.h
```

## What we explicitly did NOT vendor

- `fft/` — Mini-FT8's bundled KissFFT. We use the one already vendored
  in `nanojs8_gfsk8` (`vendor/kissfft/`). Having two KissFFT copies in
  the build would produce duplicate-symbol link errors.
- `common/stpcpy_compat.c` — `_GNU_SOURCE` in our sdkconfig already
  supplies `stpcpy` at link time. The .h is retained.
- `common/audio.c`, `common/wave.c` — host-side WAV file I/O, not used.

## Modifications to upstream source

All edits are marked inline with the comment tag `NANOJS8 MODIFICATION`
to make them grep-discoverable.

### `ft8/constants.c`

The single most important change: swapped the FT8 Costas sync pattern
for the JS8 Normal Costas pattern.

```c
// Before (FT8):
const uint8_t kFT8_Costas_pattern[7] = { 3, 1, 4, 0, 6, 5, 2 };

// After (JS8 Normal — Type::ORIGINAL in gfsk8):
const uint8_t kFT8_Costas_pattern[7] = { 4, 2, 5, 6, 1, 3, 0 };
```

The array's name was kept as `kFT8_Costas_pattern` because all of
`decode.c`'s sync-scoring code references that exact symbol. Renaming
would have required modifying decode.c too, and the upstream source is
already conceptually "FT8 timing + Costas" — we just change what
specific Costas the array holds.

### `common/fft_wrapper.h`

Re-routed KissFFT include path:

```c
// Before:
#include "../fft/kiss_fftr.h"
// After:
#include <kissfft/kiss_fftr.h>
```

Resolved via `REQUIRES nanojs8_gfsk8` in our CMakeLists, which puts
gfsk8's `vendor/` directory on the include path.

### `common/monitor.c`

Three changes:

1. `MONITOR_NFFT_MAX` bumped from 960 to 4096 to support our 12 kHz
   sample rate with `freq_osr = 2` (yields nfft = 3840).

2. `WF_STATIC_SIZE` reduced to 1 (dummy). At 12 kHz with freq_osr=2 the
   waterfall is ~333 KB — too large for DRAM BSS. We always heap-allocate
   to PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.

3. Default log level changed from `LOG_INFO` to `LOG_WARN`. (L7.3 lesson:
   INFO floods the 115200 UART console and garbles output.)

### Component-level

`CMakeLists.txt`: not vendored; written from scratch. Adds the warning
suppression flags Mini-FT8 used upstream (`-Wno-unused-const-variable`,
`-Wno-format`, `-Wno-format-truncation`).

## License

ft8_lib is permissive-licensed (effectively CC0 for the core). Our
modifications are GPL-3.0 to be consistent with the rest of NanoJS8 v0.7.
