# NanoJS8

A pocket JS8 amateur-radio transceiver controller for the M5Stack Cardputer ADV.

NanoJS8 ports the MicroJS8 (W5DMH, Pi Zero 2W Python) user experience to the
ESP32-S3 platform in ESP-IDF / FreeRTOS / C++. It replicates MicroJS8's screen
ring, protocol grammar, and operating ergonomics on a pocket-sized device with
a built-in keyboard and display.

**Status: Phase 2 - 7 screens.** Boots into a 7-screen ring matching MicroJS8's UI:
HOME / HEARD / DIRECTED / INBOX / COMPOSE / ALLCALL / SETUP. HOME shows the
current operator identity (CALL, GRID) plus placeholders for GPS / FREQ / CAT /
INBOX status. SETUP is fully editable with CALL, GRID, RADIO, GROUPS. Ring is
navigated with bare left arrow (`,`) and right arrow (`/`); both wrap. See
[Build Specification §11](docs/) for the phased delivery plan.

**License: GPL-3.0.** Inherits from gfsk8-modem-clean (jfrancis42) and
MicroJS8 lineage.

---

## Hardware

- M5Stack Cardputer ADV (ESP32-S3FN8, 8 MB flash, no PSRAM)
- microSD card (mandatory from Phase 4 onward; optional for Phase 0)
- M5Stack Cap LoRa-1262 (for GPS — added in Phase 6)

## Toolchain

- ESP-IDF **v5.5.4** (pinned — see [BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md))
- Windows 11 + PowerShell (primary) — Linux supported via parallel `.sh` scripts
- Optional: VS Code with the official Espressif ESP-IDF extension

---

## Quickstart — Windows 11

These steps assume ESP-IDF v5.5.4 is already installed at `C:\esp\v5.5.4\esp-idf`.
If not, see the [installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/index.html)
or [BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md) for the exact procedure.

```powershell
# 1. Activate the IDF environment (do this in every new PowerShell window):
& "C:\esp\v5.5.4\esp-idf\export.ps1"

# 2. Change into the project folder:
cd C:\dev\nanojs8

# 3. Fetch vendored M5Stack libraries (one-time, ~2-3 min on a fast link):
.\tools\fetch_components.ps1

# 4. Stage the gfsk8 modem upstream (Phase 4 prep — optional for Phase 0):
.\tools\apply_gfsk8_patches.ps1

# 5. Set the chip target:
idf.py set-target esp32s3

# 6. Build (~5-10 min on a modern machine for the first build):
idf.py build

# 7. Connect the Cardputer ADV via USB-C, find its COM port in Device
#    Manager (Ports > USB JTAG/serial debug unit (COMx)), then flash + monitor:
idf.py -p COM11 flash monitor
```

Replace `COM11` with the actual COM port shown in Device Manager.

To exit `monitor`: `Ctrl+]`.

---

## Definition of Done — Phase 1

After flash and one power cycle, the Cardputer ADV should:

1. Show the Phase 0 splash briefly (~1.2 s), then transition to the **SETUP** screen.
2. SETUP shows three fields: `CALL`, `GRID`, `RADIO`. The first is focused.
3. **Tab** moves focus through the fields in order, wrapping at the end.
4. **Enter** on a focused field enters EDIT mode.
   - Text fields (CALL, GRID): type to append, Backspace to delete, Enter to commit, Fn+\` to cancel.
   - Menu field (RADIO): Fn+`;` / Fn+`.` cycle options, Enter to commit, Fn+\` to cancel.
5. **Ctrl+S** at any time saves all fields to NVS. A green "Saved" banner appears for ~1.5 s.
6. Committing a field with an invalid value (e.g. callsign with `?`) shows a red "Invalid value" banner; the field stays in edit mode until the operator fixes it or presses Esc.
7. **Power cycle** the device — the saved values are still there.
8. On first boot with empty NVS, the defaults are `NOCALL` / `AA00` / `qdx`. The serial log warns that the placeholder callsign needs replacing.

### Key bindings (mirrors MicroJS8)

| Action | Key |
|---|---|
| Next field | Tab |
| Enter / exit edit mode | Enter / Fn+\` (Esc) |
| Cycle menu options | Fn+`;` (up) / Fn+`.` (down) |
| Save all | Ctrl+S |
| Previous / next screen (Phase 2+) | Fn+`,` / Fn+`/` |

---

## Definition of Done — Phase 0

After `flash monitor`, the Cardputer ADV display should show:

```
NanoJS8 v0.0.1
Phase 0 — boot diag

Chip:      ESP32-S3 rev v0.2
Free DRAM: <N> KB
SD:        Present  (or "Not Found" if no SD card)

Built <date> <time>
[Phase 0 boot diagnostic]
```

The serial monitor will show the same information logged via `ESP_LOGI`.

**Pass criteria:**
- Splash visible on the Cardputer ADV display
- Free DRAM >= 200 KB (with SD mounted)
- SD status correctly reflects whether a card is inserted

If those three are true, Phase 0 is complete and we move to Phase 1.

---

## Project layout

```
nanojs8/
├── CMakeLists.txt              project root
├── partitions.csv              8 MB layout
├── sdkconfig.defaults          baseline Kconfig overrides
├── main/                       app entry point
├── components/
│   ├── board_cardputer_adv/    Cardputer hardware init (forked from Mini-FT8)
│   ├── gfsk8_modem/            JS8 modem (vendored in Phase 4)
│   ├── M5Cardputer/            display + keyboard library  (fetched by script)
│   ├── M5GFX/                  graphics library            (fetched by script)
│   └── M5Unified/              power + sensors library     (fetched by script)
├── tools/
│   ├── fetch_components.ps1    pull M5Stack libs at pinned versions
│   ├── apply_gfsk8_patches.ps1 clone + patch JS8 modem
│   └── check_dram_budget.py    POST_BUILD memory check
└── docs/
    └── ...                     (Build Specification lives at repo root)
```

---

## Troubleshooting

**`idf.py: command not found`** — you didn't activate ESP-IDF. Run
`& "C:\esp\v5.5.4\esp-idf\export.ps1"` in your current PowerShell window.

**`fatal error: M5Cardputer.h: No such file or directory`** — you haven't run
`tools\fetch_components.ps1` yet. The M5Stack libraries are downloaded by
that script, not committed to the repo.

**`Failed to connect to ESP32-S3`** — check the COM port number in Device
Manager. Try holding the G0 key while plugging in the USB-C cable, then
releasing G0 after `esptool` connects.

**SD card shows "Not Found" with a card inserted** — Phase 0 uses
conservative SPI settings (5 MHz). Some older or low-quality cards may not
enumerate at all on the first cold boot. Try a different card, then file
an issue with the brand and capacity.

---

## See also

- [BUILD_ENVIRONMENT.md](BUILD_ENVIRONMENT.md) — exact toolchain pin record
- Build Specification (in the project root) — full design document
- [Mini-FT8](https://github.com/...) — reference codebase we forked from
- [MicroJS8](https://github.com/...) — UX reference
- [gfsk8-modem-clean](https://github.com/jfrancis42/gfsk8-modem-clean) — JS8 modem
