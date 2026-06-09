# nanojs8_gfsk8 — Attribution and Modifications

This component vendors source code from **gfsk8-modem-clean** by Jordan Sherer
(KN4CRD), the author of JS8Call. The upstream project is licensed under
GPL-3.0 and so is NanoJS8.

- Upstream repository: https://github.com/jfrancis42/gfsk8-modem-clean
- Upstream license: GPL-3.0
- Original copyright: (C) 2018-2024 Jordan Sherer <kn4crd@gmail.com>

## Files lifted unmodified

Headers and most source files were copied verbatim from the upstream
`src/` and `vendor/` directories:

```
src/JS8.cpp                       src/JS8.h
src/JS8Submode.h                  src/DecodedText.cpp
src/DecodedText.h                 src/FrequencyTracker.cpp
src/FrequencyTracker.h            src/js8codec.cpp
src/js8codec.h                    src/commons.h
src/constants.h                   src/crc12.h
src/fft_shim.h                    src/fftw_compat.h
src/ldpc_feedback.h               src/log.h
src/soft_combiner.h               src/tracker.h
src/whitening.h                   src/whitening_processor.h
include/gfsk8modem.h
vendor/kissfft/kiss_fft.c         vendor/kissfft/kiss_fft.h
vendor/kissfft/kiss_fftr.c        vendor/kissfft/kiss_fftr.h
vendor/kissfft/_kiss_fft_guts.h   vendor/kissfft/kiss_fft_log.h
vendor/crc12.h
```

KissFFT is BSD-3-Clause licensed (Mark Borgerding); its license header
is preserved in `vendor/kissfft/`.

## Files modified for the NanoJS8 RX-only ESP32 port

### `src/JSC.cpp` — multiple changes
1. **`decompress()` (RX path)**: reads dictionary words from a memory-mapped
   ESP-IDF partition instead of the compiled-in `JSC::map[]` array. One
   line changed: `JSC::map[idx].str` → `nanojs8_jsc_map_word(idx)`. New
   include: `#include "jsc_map.h"`. See `components/nanojs8_jsc_map/`
   for the partition format and loader.
2. **`compress()`, `codeword()`, `exists()`, both `lookup()` overloads**:
   bodies replaced with stubs that return empty / false. These are
   TX-side functions; NanoJS8 v0.7 is RX-only. Stubbing removes
   ODR-use of `JSC::list[]` and `JSC::map[]`, allowing those ~6 MB of
   data to be omitted from the binary entirely. When TX support is
   added in a later layer, real bodies will be restored and the
   lookups will be backed by an mmap'd `JSC::list` partition.

### `src/Varicode.cpp` — one function stubbed
- **`Varicode::extendedChars()`**: body replaced with a stub returning
  an empty string. Original body iterated `JSC::prefix[]` which has no
  definition in our RX-only build. The function has zero callers in
  the gfsk8 codebase so stubbing is a no-op functionally.

### `src/api.cpp` — PSRAM placement for large globals
- `dec_data` (RxAudioBuffer, 1.44 MB) and `specData` (RxSpectrum) are
  decorated with `EXT_RAM_BSS_ATTR` so the linker places them in
  PSRAM rather than internal DRAM. ESP-IDF sdkconfig requires
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`.
- New include: `esp_attr.h` for the macro.

### `CMakeLists.txt` — new
- Component build script. Sets C++20 as required by the upstream
  source (uses `std::span`, `<concepts>`, `<numbers>`).

## Files omitted entirely

These upstream files are not included in this component:

- `src/JSC_list.cpp` (262,144-entry TX-only lookup table, ~6 MB source)
- `src/JSC_map.cpp` (262,144-entry RX dictionary — replaced by
  `nanojs8_jsc_map` memory-mapped partition)
- `src/JSC_checker.cpp` (Qt-dependent UI spell-checker)
- `src/JSC.h` field references in TX functions (no source change; the
  declarations remain in the header for ABI stability, just no longer
  ODR-used)

## License compliance

Per GPL-3.0:
- The complete corresponding source code for the modified gfsk8-modem-
  clean is available in this directory.
- All changes are listed above and documented inline in the modified
  files with NanoJS8 L7.4b modification comments.
- The original copyright header has been preserved in `JSC.cpp` and
  other modified files.
- NanoJS8 as a whole is also GPL-3.0 licensed; see the project root
  LICENSE file.
