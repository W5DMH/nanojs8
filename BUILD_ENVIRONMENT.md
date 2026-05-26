# Build Environment — NanoJS8

This file records the **exact** toolchain and dependency versions used to
produce the most recent committed release. It is regenerated at each tagged
release and committed alongside the binary in `release/`.

The reproducibility guarantee: **anyone can recreate a bit-identical NanoJS8
binary** by following this file, regardless of what their default ESP-IDF
install looks like or how much time has passed since the release was tagged.

See Build Specification §13.1 for the full long-term maintainability strategy.

---

## Current pins

| Item | Pinned value |
|---|---|
| **ESP-IDF version** | v5.5.4 |
| **Target chip** | ESP32-S3 (revision v0.2 verified) |
| **Compiler** | xtensa-esp-elf-gcc 14.2.0 (crosstool-NG `esp-14.2.0_20260121`) |
| **CMake** | ≥ 3.20 |
| **Python** | 3.11 / 3.12 / 3.13 / 3.14 (any of these — IDF auto-detects) |
| **Host OS** | Windows 11 (verified), Linux (compatible) |

## Managed components

These are pulled from the ESP-IDF Component Registry and locked via
`dependencies.lock` (committed to the repo). The versions below match what
`dependencies.lock` records for the current release.

| Component | Pinned version | Source |
|---|---|---|
| `espressif/esp_codec_dev` | ^1.5.9 | https://components.espressif.com/ |

(USB host components added in Phase 3:
`espressif/usb_host_uac ^1.2.0`, `espressif/usb_host_cdc_acm ^2.2.0`,
`espressif/esp_tinyusb ^2.1.0`.)

## Vendored libraries

Fetched by `tools/fetch_components.ps1` (or `.sh`) at the versions below:

| Library | Pinned version | Source |
|---|---|---|
| `M5GFX` | 0.2.17 | https://github.com/m5stack/M5GFX |
| `M5Unified` | 0.2.11 | https://github.com/m5stack/M5Unified |
| `M5Cardputer` | 1.1.1 | https://github.com/m5stack/M5Cardputer |
| `gfsk8-modem-clean` | (Phase 4 — pinned to SHA on first integration) | https://github.com/jfrancis42/gfsk8-modem-clean |

`board_cardputer_adv/` is committed directly to the repo (it's a project
artifact, forked from Mini-FT8 and modified for our pin map). Its provenance
is recorded in `components/board_cardputer_adv/PROVENANCE.md`.

## Reproducible-build procedure (year 2029, hypothetical)

```powershell
# 1. Pull the exact Docker image used at release time
docker pull espressif/idf:v5.5.4@<digest from the release notes>

# Inside the container (or use it as a one-shot):
docker run --rm -v ${PWD}:/project espressif/idf:v5.5.4 bash -c "
    cd /project &&
    idf.py set-target esp32s3 &&
    idf.py build
"

# 2. Verify SHA-256 of build/nanojs8.bin matches the release record below.
```

If this procedure ever fails, that's a reproducibility bug — open an issue.

---

## Release records

This section is appended to (never overwritten) for each tagged release.
Each release block documents the SHA-256 of the released binary, the
toolchain Docker digest, and the dependencies.lock contents.

### v0.0.1 — Phase 0 — (date TBD on first build)

| Field | Value |
|---|---|
| Tag | `v0.0.1` |
| Date | (filled at release time) |
| Docker image | `espressif/idf:v5.5.4` (digest: filled at release time) |
| Compiler | `xtensa-esp32s3-elf-gcc (crosstool-NG esp-14.2.0_20260121) 14.2.0` |
| Binary SHA-256 | (filled by `tools/release.ps1` at release time) |
| gfsk8-modem-clean SHA | (Phase 4 onward) |
| dependencies.lock | (committed contents will be appended here) |

(Add new sections above this line as releases are tagged.)
