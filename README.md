# NanoJS8

A pocket JS8 amateur-radio transceiver firmware for the **LilyGO T-Deck Plus** (ESP32-S3, 16&nbsp;MB flash, 8&nbsp;MB PSRAM).

**Status:** Pre-alpha. On-air verified for bidirectional MSG / ACK on 40&nbsp;m JS8 Normal between W5DMH and KD8PGB. Plenty of rough edges remain — see the [Status](#status) section.

> **Install in one click:** [w5dmh.github.io/nanojs8](https://w5dmh.github.io/nanojs8/)

---

## Hardware

| Item | Notes |
|---|---|
| LilyGO T-Deck Plus | ESP32-S3FN16R8, GPS, 16 MB flash, 8 MB PSRAM. The "Plus" variant matters — the GPS module and PSRAM are not in the standard T-Deck. |
| Radio interface | DigiRig (or any USB sound card + RTS-PTT adapter). Verified profile: `digirig-rts-only`. |
| USB-C Y-cable | So the T-Deck Plus and the DigiRig audio adapter can both be powered while connected. |
| HF SSB transceiver | Any radio that accepts PTT input. Verified on Xiegu G90 (`xiegu-g90-digirig` profile adds CI-V CAT). |

## Install (for testers)

**Easiest path:** [w5dmh.github.io/nanojs8](https://w5dmh.github.io/nanojs8/) — connect the T-Deck Plus over USB, click Install, done. Chrome or Edge on a desktop OS only (Web Serial requirement).

**Manual path:** Download `nanojs8-merged.bin` from the [latest release](https://github.com/W5DMH/nanojs8/releases/latest), then:

```bash
esptool.py --chip esp32s3 --port <COMx> write_flash 0x0 nanojs8-merged.bin
```

After flashing, first-boot setup is on the device's SETUP screen:
1. Enter callsign and Maidenhead grid (e.g. `EN83`)
2. Pick your radio profile
3. Tune the radio to **7.078 MHz USB** (the JS8 calling frequency on 40 m)
4. Plug in the DigiRig (or your USB audio adapter) via the Y-cable
5. UTC syncs automatically via GPS within a few minutes of getting sky visibility. You can also enter UTC manually on SETUP row 6.

## Status

Honest checklist of what's working and what isn't:

✅ Working today:
- JS8 Normal mode RX with full LDPC decode
- Multi-frame MSG reception and re-assembly
- Auto-ACK on received MSG verbs
- Multi-frame TX (up to 7+ frames verified on-air)
- INBOX with NVS persistence across reboots
- Heartbeat TX
- HEARD / DIRECTED / ALL activity screens
- GPS UTC sync (T-Deck Plus on-board u-blox MIA-M10Q)
- DigiRig / Xiegu G90 CAT via Icom CI-V

🚧 Known limitations:
- JS8 Normal mode only — Slow / Fast / Turbo not yet implemented
- Sensitivity is below desktop JS8Call (~3-6 dB depending on conditions)
- Audio level handling is manual — peak above ~28000 starts clipping and reducing decode margin; we don't warn yet
- Compound callsigns with slash prefix (e.g. `KH6/W5DMH`) get a placeholder `<...>` in HEARD displays
- LoRa radio (SX1262) is held in reset — not yet a radio interface
- Touch screen is unused; trackball + keyboard only

❌ Not yet:
- Direct FT8 or other digital-mode interop
- Internet / APRS-IS gateway

## Build from source (developers)

Requires **ESP-IDF v5.5.4** at `C:\esp\v5.5.4\esp-idf` (Windows) or `~/esp/v5.5.4/esp-idf` (Linux/macOS).

### Two build profiles

| Profile | Use case | Console | GPS |
|---|---|---|---|
| **dev** | Local development with Pi monitor on `/dev/serial0` | UART0 on Grove pins (GPIO 43 TX, 44 RX) | OFF |
| **release** | Public test build (what CI ships) | USB Serial-JTAG over USB-C | ON |

### Dev build (your normal workflow)

```powershell
# Windows PowerShell
& "C:\esp\v5.5.4\esp-idf\export.ps1"
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev" build
idf.py -p COM5 flash monitor
```

```bash
# Linux / macOS
. ~/esp/v5.5.4/esp-idf/export.sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev" build
idf.py -p /dev/ttyUSB0 flash monitor
```

For console monitoring with the Pi UART cable on `/dev/serial0`:
```bash
picocom -b 115200 /dev/serial0
```

### Release build (CI runs this; can run locally to test)

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release" build
idf.py merge-bin    # produces build/nanojs8-merged.bin
```

### Project layout

```
nanojs8/
├── main/                       app entry + Kconfig.projbuild
├── components/                 ~25 in-tree components
│   ├── nanojs8_js8_codec/      JSC codec + JS8 encode/decode
│   ├── nanojs8_js8_sync/       Sync detector + LDPC decode driver
│   ├── nanojs8_ft8_lib/        ft8_lib (vendored, JS8-customized)
│   ├── nanojs8_gfsk8/          Varicode + pack/unpack (vendored)
│   ├── nanojs8_rx_audio/       USB UAC → 12 kHz decimator + slot trigger
│   ├── nanojs8_tx_audio/       12 kHz → 48 kHz upsample + USB UAC out
│   ├── nanojs8_tx_queue/       Outbound message queue + drain task
│   ├── nanojs8_mailbox/        INBOX with NVS persistence
│   ├── nanojs8_activity/       HEARD / DIRECTED tables
│   ├── nanojs8_gps/            u-blox NMEA parser + UART driver
│   ├── nanojs8_ui/             Screen router + 9 screens
│   ├── nanojs8_display/        ST7789 panel driver
│   ├── nanojs8_keyboard/       T-Deck I²C keyboard (0x55)
│   ├── nanojs8_trackball/      T-Box rotary input
│   ├── nanojs8_ptt/            RTS-PTT + watchdog
│   ├── nanojs8_cat/            CAT facade
│   ├── nanojs8_cat_civ/        Icom CI-V protocol
│   ├── nanojs8_radio/          Profile registry
│   ├── nanojs8_usb_audio/      UAC host
│   ├── nanojs8_usb_serial/     CDC-ACM host (CP2102 etc.)
│   ├── nanojs8_platform_tdeck/ Board init (POWERON, I²C, LoRa hold-reset)
│   ├── nanojs8_jsc_map/        JSC dictionary partition reader
│   ├── nanojs8_time/           UTC anchor + slot math
│   └── nanojs8_config/         NVS-backed station config
├── assets/                     JSC dictionary source (≥ 262144 entries)
├── docs/                       GitHub Pages site + ESP Web Tools manifest
├── .github/workflows/          build.yml + release.yml (CI)
├── partitions.csv              16 MB layout
├── sdkconfig.defaults          shared baseline Kconfig
├── sdkconfig.dev               dev-build overlay (console UART0, GPS off)
├── sdkconfig.release           release-build overlay (console USB-JTAG, GPS on)
└── CMakeLists.txt              project root
```

### Releasing

CI builds the release profile, calls `idf.py merge-bin`, attaches the binaries to a GitHub Release, and deploys [w5dmh.github.io/nanojs8](https://w5dmh.github.io/nanojs8/) with the new version. Trigger:

```bash
git tag v0.7.0-alpha
git push --tags
```

The workflow file [`.github/workflows/release.yml`](.github/workflows/release.yml) handles the rest.

## License

GPL-3.0. Inherits from [gfsk8-modem-clean](https://github.com/jfrancis42/gfsk8-modem-clean) (jfrancis42) and the JS8Call lineage.

## Acknowledgments

- Jordan Sherer (KN4CRD) and the [JS8Call](http://js8call.com) team for the protocol and reference implementation
- Joe Taylor (K1JT) for FT8 / FT4 / JT9, on which JS8 builds
- Karlis Goba (YL3JG) for [ft8_lib](https://github.com/kgoba/ft8_lib), our LDPC and Costas base
- Jeff Francis for [gfsk8-modem-clean](https://github.com/jfrancis42/gfsk8-modem-clean)
- LilyGO for the T-Deck Plus hardware
- KD8PGB and L0LFN for being the first on-air test correspondents

Dan W5DMH · grid EN83 · [github.com/W5DMH/nanojs8](https://github.com/W5DMH/nanojs8)
