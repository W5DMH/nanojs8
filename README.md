# NanoJS8

A pocket JS8 amateur-radio transceiver controller for the M5Stack Cardputer ADV.

NanoJS8 ports the MicroJS8 (W5DMH, Pi Zero 2W Python) user experience to the
ESP32-S3 platform in ESP-IDF / FreeRTOS / C++. It replicates MicroJS8's screen
ring, protocol grammar, and operating ergonomics on a pocket-sized device with
a built-in keyboard and display.

**Status: Phase 3a - DigiRig + RTS-PTT.** USB host (OTG) live for the
Cardputer ADV's USB-C port. The firmware enumerates DigiRig (audio side
via UAC, control side via CP2102 CDC-ACM) and asserts the radio's PTT via
the CP2102 RTS line. HOME now shows live CAT status from the radio
service. SETUP gains an AUTOSTART toggle that controls whether the radio
service starts at boot. Console relocated to UART0 on the Grove port
(GPIO 1 TX / GPIO 2 RX) so the USB-C port is free for OTG host duty;
external USB-UART cable required for `idf.py monitor` and the new serial
commands. See [Build Specification §11](docs/) for the phased delivery plan.

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
#    Manager (Ports > USB JTAG/serial debug unit (COMx)), then flash:
idf.py -p COM11 flash

# Phase 3a note: `idf.py monitor` over the USB-C port no longer works
# because the running firmware's console moved to UART0 / Grove. Connect
# an external USB-UART cable to the Grove port (GPIO 1 TX, GPIO 2 RX, GND)
# and use:
#   idf.py -p COMyy monitor
# where COMyy is the FTDI cable's COM port. See "Phase 3a hardware setup"
# below.
```

Replace `COM11` with the actual COM port shown in Device Manager.

To exit `monitor`: `Ctrl+]`.

---

## Phase 3a hardware setup

Phase 3a uses the USB-C port for **OTG host duty** (talking to a DigiRig and
its attached radio). That means the console must move off USB-Serial-JTAG. The
new console lives on **UART0** routed to the **Grove port** (GPIO 1 = TX,
GPIO 2 = RX), which means you need an external USB-UART cable to see logs and
issue serial commands.

### What you need

- A USB-A-to-TTL UART cable (any FTDI FT232RL / CP2102 / CH340 dongle, ~$5)
- The Cardputer ADV's Grove connector (4-pin HY2.0)
- A powered USB hub (or USB-C Y-cable with power injection) for the OTG side

### Grove-to-USB-UART wiring

| Cardputer Grove pin | Signal | USB-UART cable pin |
|---|---|---|
| 1 (Black) | GND | GND |
| 2 (Red) | 5V | **leave unconnected** (Cardputer self-powers) |
| 3 (Yellow / G1) | GPIO 1 → ESP TX | cable RX |
| 4 (White / G2) | GPIO 2 → ESP RX | cable TX |

Console baud is 115200 8N1.

### Daily workflow

1. **Flash:** plug USB-C → PC, run `idf.py -p COMxx flash`. The ROM bootloader's
   own USB-CDC handles the flash regardless of where the running firmware's
   console is.
2. **Monitor:** connect the USB-UART cable to your PC, find its COM port,
   run `idf.py -p COMyy monitor` (where `COMyy` is the FTDI cable, not the
   Cardputer's USB-C).
3. **Test radio:** unplug the Cardputer's USB-C from the PC, plug it into a
   powered hub. Connect the DigiRig to a hub port. Logs still flow through
   the Grove/FTDI cable.

### Serial commands

Type at the `nanojs8>` prompt over the UART monitor:

- `radio start` — start the USB host stack, enumerate the DigiRig
- `radio stop` — release USB devices, stop the radio service
- `radio status` — dump current state, profile, PTT state, frame counters
- `ptt on` / `ptt off` — manually assert/release PTT (CP2102 RTS line)
- `help` — list registered commands

To make the service start automatically at boot, set `AUTOSTART: on` on the
SETUP screen. (Default is OFF so a fresh device behaves like Phase 0/1/2.)

---

## Definition of Done — Phase 3a

Phase 3a DoD is split into two tiers:

### Tier 1 — buildable & boots cleanly (no DigiRig required)

1. CI build succeeds; binary < 1 MB.
2. Splash shows `v0.3.0` and `Phase 3a - DigiRig + RTS-PTT`.
3. Free DRAM at boot > 200 KB.
4. Console comes up on UART0 at 115200 baud via Grove with the `nanojs8>` prompt.
5. `help` lists `radio` and `ptt` commands.
6. With nothing plugged into the USB-C port: `radio start` → service enters
   `ENUMERATING` state, HOME's CAT row reads `Waiting...`.
7. Plug an unrelated USB device (e.g. a phone) → enumerate log appears,
   `radio status` shows `enum_attempts > 0` but no profile match. CAT stays
   `Waiting...`.
8. Unplug → CAT returns to `Waiting...` within 2 s. No hangs across 10+
   plug/unplug cycles.
9. `radio stop` → CAT returns to `Disconnected`.
10. NVS migration from Phase 2 v2 → v3 preserves CALL/GRID/RADIO/GROUPS,
    defaults AUTOSTART to off.
11. SETUP shows 5 fields including AUTOSTART; toggle saves and persists.
12. Phase 0/1/2 behavior preserved (ring nav, NVS, validators, banners).

### Tier 2 — with DigiRig + powered hub + radio (full end-to-end)

1. Plug DigiRig (audio + serial) into powered hub, hub into Cardputer USB-C.
2. Within 2 s of `radio start`: HOME's CAT row reads `DigiRig RTS-PTT`.
3. `radio status` shows `status: CONNECTED`, `rx_frames > 0` and increasing.
4. `ptt on` → multimeter confirms CP2102 RTS line goes high; HOME shows
   `DigiRig RTS-PTT *TX*` in red.
5. `ptt off` → RTS goes low; HOME's red indicator clears.
6. PTT held for 20+ s without `ptt off` → auto-released by the watchdog;
   serial log shows the timeout warning.
7. Unplug DigiRig while PTT asserted → RTS released as a side effect of
   close; HOME returns to `Waiting...`.



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
│   ├── nanojs8_config/         NVS-backed configuration
│   ├── nanojs8_radio/          Phase 3a: USB host + UAC + CP2102 PTT
│   ├── nanojs8_ui/             input + screen router + screens
│   ├── usb_host_uac/           vendored from Mini-FT8 (registry regression workaround)
│   ├── M5Cardputer/            display + keyboard library  (vendored, patched)
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
