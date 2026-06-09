# ATTRIBUTION — nanojs8_js8_codec

Component: `components/nanojs8_js8_codec/`
Layer:     NanoJS8 v0.7 L7.6 (LDPC decode + CRC + message extract)

## Upstream

`gfsk8-modem-clean` — Jordan Sherer / Allan Bazinet's JS8 modem extracted
from JS8Call-improved as a Qt-free C++17 static library.
License: GPL-3.0.

## What we vendored

All in `src/js8_codec.cpp` (anonymous namespace). All lifts are verbatim
except for the function-extraction noted below.

| Item | Source | Lines |
|---|---|---|
| LDPC constants (N, K, M, BP_MAX_*) | `JS8.cpp` | 187-203, 595-597 |
| `ParityCheckNode` struct | `JS8.cpp` | 636-639 |
| `Mn` matrix (174×3 ints) | `JS8.cpp` | 599-634 |
| `Nm` matrix (87 × ParityCheckNode) | `JS8.cpp` | 641-727 |
| `alphabet` (64-char string_view) | `JS8.cpp` | 849-852 |
| `crc12_compute` | `vendor/crc12.h` | full file (renamed from `crc12::compute`) |
| `CRC12<T>()` template | `JS8.cpp` | 893-895 |
| `bpdecode174` | `JS8.cpp` | 731-841 |
| `checkCRC12` | `JS8.cpp` | 897-918 |

### Modifications

- `crc12::compute` renamed to `crc12_compute` and pulled into anonymous
  namespace alongside `CRC12()` — eliminates the dependency on
  `vendor/crc12.h` and keeps the codec self-contained.
- `extractmessage174` was *split* into a CRC-check + an alphabet-mapping
  half (`extract_alphabet_chars`). Upstream returned a `std::string`; we
  write into a caller-provided 12-character buffer for zero-allocation use.
- `Mn` / `Nm` declared as file-static `constexpr` arrays in anonymous
  namespace (was likewise upstream — no change, just noted).
- C-callable wrapper `nanojs8_js8_decode_llrs` is our own glue (not in
  the upstream).

### What we did NOT vendor

- `extractmessage174` whole-form (we re-assembled the equivalent inline)
- `extract_alphabet_chars` upstream version's `std::string` use
- `Costas` arrays (already in `nanojs8_ft8_lib/ft8/constants.c` — JS8
  Normal Costas was swapped in there at L7.5)
- `DecodeMode<>` template, `Impl` class, `Decoder` class — see
  `components/nanojs8_gfsk8/ATTRIBUTION.md` and L7.4c notes

## Our own code

| File | What |
|---|---|
| `include/js8_codec.h` | Public C-callable API |
| `src/llr_extract.c` | JS8 LLR extraction from ft8_lib waterfall (natural-binary, no Gray code — modelled on `ft8_lib/ft8/decode.c:ft8_extract_likelihood`) |

The LLR extraction technique (max-of-set difference per bit) is the same
algorithm `ft8_lib` uses. The key JS8-specific change is removing the
`kFT8_Gray_map[]` indirection — JS8 maps bit triplets directly to tone
indices, while FT8 applies a Gray-code permutation. See header comment in
`llr_extract.c` for the verification trail.

## License

GPL-3.0 (inherited from gfsk8-modem-clean upstream and consistent with
the rest of NanoJS8 v0.7).
