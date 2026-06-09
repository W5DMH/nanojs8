/*
 * NanoJS8 v0.7 — main entry point
 * =================================
 * Layer 1: scaffolding only.
 *
 * Boots on the LilyGO T-Deck (ESP32-S3FN16R8), prints a version banner
 * to the serial console (UART0 on GPIO 43/44, exposed via the Grove
 * connector), reports key system facts, and idles. No display, no
 * keyboard, no USB host — those are added in Layers 2-5.
 *
 * The point of Layer 1 is to prove:
 *   1. The build system works with PSRAM enabled (MINIMAL_BUILD dodges
 *      the GCC 14.2.0 ICE on esp_lcd_panel_rgb.c).
 *   2. The 16 MB partition table is accepted.
 *   3. The chip boots into our application (boot:0x8 SPI_FAST_FLASH_BOOT).
 *   4. The UART console out the Grove pins is readable from the Pi.
 *   5. PSRAM is detected, sized, and initialized.
 *   6. The application keeps running indefinitely without crashes.
 *
 * Once those are verified on hardware, Layer 2 adds the platform
 * component (POWERON pin, I2C bus for keyboard, GPIO for backlight).
 *
 * License: GPL-3.0
 * Author: W5DMH (dan@hurdfarms.com)
 */

#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"     // L7.11a-fix1: one-shot self-test task sync
#include "freertos/idf_additions.h" // L7.13-fix3: xTaskCreatePinnedToCoreWithCaps for PSRAM-backed self-test stack
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_psram.h"

// L7.14: GPS subsystem + UART/GPIO APIs for console handover.
#include "gps.h"
#include "driver/uart.h"
#include "driver/gpio.h"

// Platform layer (Layer 2): T-Deck hardware initialization.
#include "platform_tdeck.h"

// Display layer (Layer 3): ST7789 panel driver and text rendering.
#include "display.h"

// Keyboard input (Layer 4): I²C reader for the C3 keyboard at 0x55.
#include "keyboard.h"

// USB audio (Layer 5): UAC 1.0 host for the CM119 / similar adapters.
#include "audio.h"

// USB serial (Layer 5b): CDC-ACM + CP210x host for the DigiRig's CP2102
// CAT/PTT bridge. Coexists with the audio component on the same USB host
// library — both share one physical USB-C port via the DigiRig's hub.
#include "usb_serial.h"

// Trackball input (Layer 6b.1): T-Box directional input on GPIO 1/2/3/15
// plus center click on GPIO 0. Produces virtual-key events that mirror
// the keyboard subsystem's get_key API.
#include "trackball.h"

// Persistent config (Layer 6b.1): NVS-backed storage for callsign, grid,
// freq, radio profile, etc. Available immediately after init.
#include "config.h"

// Layer 6b.4: radio profile registry + PTT controller with safety watchdog.
// Profiles describe how each supported radio is keyed; the PTT subsystem
// owns the hardware line drive plus the 20-second auto-release watchdog.
#include "radio.h"
#include "ptt.h"
#include "cat.h"

// L7.0: manual UTC clock subsystem (operator enters time via SETUP;
// volatile in RAM, reset every boot). Foundation for JS8 slot
// alignment in L7.1+.
#include "time_source.h"
#include "activity.h"     // L7.9: HEARD + DIRECTED store
#include "mailbox.h"      // L7.11f: COMPOSE→STORE mailbox

// L7.1: RX audio pipeline (decimator + ring + slot trigger). Sole
// consumer of the USB audio RX FIFO from L7.1 onward.
#include "rx_audio.h"

// L7.4b: gfsk8 modem (the JS8 wire layer). The component is linked via
// REQUIRES nanojs8_gfsk8 in main/CMakeLists.txt. We retain it because
// L7.6+ will lift its bpdecode174 + Varicode + JSC.cpp via the new
// js8 layers. We do not call into gfsk8 directly from main.c — all
// access is through the C-facade headers of the JS8 layers above.

// L7.5: JS8 sync detector. Built on Mini-FT8's monitor + ftx_find_candidates
// with the Costas pattern swapped to JS8 Normal in our vendored copy
// (components/nanojs8_ft8_lib/ft8/constants.c). Sync-only at this layer —
// L7.6 will add LDPC decode (bpdecode174 lifted from gfsk8), L7.7 adds
// Varicode + JSC for human-readable JS8 text.
#include "js8_sync.h"

// L7.11a: TX-side encoder + boot self-test. Wraps gfsk8::encode +
// Varicode::packHeartbeatMessage so we can produce 79-tone heartbeat
// frames. The self-test runs at boot and surfaces any encoder regression
// before the operator ever tries to transmit. Modulation, USB-TX, and
// PTT control land in L7.11b-d.
#include "js8_codec.h"

// L7.11c: TX audio path — pre-render 12k mono modulator output to 48k
// stereo PSRAM, stream out USB UAC TX endpoint via a hotkey-triggered
// worker task (no PTT yet; audio flows to DigiRig only).
#include "tx_audio.h"

// L7.11g.4: 4-deep TX FIFO for queued short-form replies (auto-ACK
// on incoming MSG verbs; future layers will reuse for QUERY MSGS,
// store-and-forward, etc). Init must come AFTER tx_audio is verified
// working since the queue's drain task calls transmit_text.
#include "tx_queue.h"

// L7.4a: memory-mapped JSC dictionary. Loaded once at boot from the
// custom `jsc_map` partition; provides O(1) word lookup for the JS8
// source codec without consuming any RAM. See components/nanojs8_jsc_map.
#include "jsc_map.h"

// Multi-screen UI framework (Layer 6b.2): screen stack, HOME and SETUP
// screens, focus state, render and input dispatch. Replaces the inline
// banner + status bar that lived directly in main.c through L6b.1.
#include "ui.h"

static const char *TAG = "nanojs8";

// Version constants. Bumped per release. v0.7.0 is the T-Deck port.
#define NANOJS8_VERSION       "0.7.0-pre-alpha-L7.16-fix2"
#define NANOJS8_VERSION_PHASE "L7.16-fix2: second crash in the KD8PGB/P → W5DMH MSG round-trip — stack overflow in tx_queue_drain task. L7.16 itself worked (MSG body was committed to mailbox + persisted to NVS, confirmed by 'load: 1 entries restored' on the post-crash boot). The new failure was the FOLLOW-UP auto-ACK enqueue: tx_queue_drain has a 4 KB DRAM stack, while transmit_text() (called from the drain) runs a synchronous nanojs8_js8_text_frame_count() pre-flight that triggers std::regex via Varicode::pack on the caller's stack — per the project journal that needs 14-20 KB. The user-compose path doesn't trip this because screen_compose calls text_frame_count itself from the UI task first, leaving the pack-cache hot when tx_queue_drain later re-runs the check. Auto-ACKs from js8sync post fresh wires to tx_queue with a cold cache, so drain hits the full regex appetite on a 4 KB stack and overflows. Fix: drain task stack bumped 4 KB → 16 KB and moved from DRAM to PSRAM via xTaskCreatePinnedToCoreWithCaps (same pattern as js8sync L7.14-fix7 and tx_audio worker). The drain → transmit_text path is pure DSP/encode work — no NVS writes — so the PSRAM stack is safe; constraint is documented at the task-create site so future changes don't reintroduce NVS calls from this task. Memory delta: -4 KB DRAM, +16 KB PSRAM (PSRAM has 2.7 MB free; DRAM headroom improves). Hindsight: when L7.14-fix9 added the synchronous pre-flight to transmit_text(), it needed an audit of ALL callers for stack sufficiency; that audit was missed and tx_queue_drain was lurking."

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Pretty-print a memory size in either KB or MB depending on magnitude.
// Returns the number of bytes written (excluding null terminator).
static int format_size(char *buf, size_t buflen, size_t bytes) {
    if (bytes >= 1024 * 1024) {
        return snprintf(buf, buflen, "%.2f MB", bytes / (1024.0 * 1024.0));
    }
    return snprintf(buf, buflen, "%.0f KB", bytes / 1024.0);
}

// Log everything the application knows about the chip and memory at boot.
// This is the "fingerprint" we'll compare in subsequent layers to detect
// any unexpected platform changes.
static void log_boot_diagnostics(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================================");
    ESP_LOGI(TAG, " NanoJS8 v%s", NANOJS8_VERSION);
    ESP_LOGI(TAG, " %s", NANOJS8_VERSION_PHASE);
    ESP_LOGI(TAG, " Built %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, " ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "========================================================");
    ESP_LOGI(TAG, "");

    // Chip information
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip:");
    ESP_LOGI(TAG, "  Model       : %s",
             chip_info.model == CHIP_ESP32S3 ? "ESP32-S3" :
             chip_info.model == CHIP_ESP32   ? "ESP32" :
             "unknown");
    ESP_LOGI(TAG, "  Revision    : v%u.%u",
             chip_info.revision / 100, chip_info.revision % 100);
    ESP_LOGI(TAG, "  Cores       : %u", chip_info.cores);
    ESP_LOGI(TAG, "  Features    : %s%s%s%s",
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (chip_info.features & CHIP_FEATURE_BT)       ? "BT "   : "",
             (chip_info.features & CHIP_FEATURE_BLE)      ? "BLE "  : "",
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM)? "ePSRAM" : "");

    // Flash size
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        char sz[32];
        format_size(sz, sizeof(sz), flash_size);
        ESP_LOGI(TAG, "  Flash       : %s", sz);
    } else {
        ESP_LOGW(TAG, "  Flash       : (read failed)");
    }

    // PSRAM — critical for v0.7+. We log size AND verify it's present.
    // If the build enabled SPIRAM but the chip doesn't have it (e.g. wrong
    // T-Deck variant), this lets the operator catch it immediately.
    if (esp_psram_is_initialized()) {
        char sz[32];
        format_size(sz, sizeof(sz), esp_psram_get_size());
        ESP_LOGI(TAG, "  PSRAM       : %s (initialized)", sz);
    } else {
        ESP_LOGE(TAG, "  PSRAM       : NOT INITIALIZED");
        ESP_LOGE(TAG, "  Phase 4 (gfsk8 decoder) will NOT work.");
        ESP_LOGE(TAG, "  Either CONFIG_SPIRAM is wrong or hardware is mismatched.");
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Heap (boot-time totals):");
    ESP_LOGI(TAG, "  Internal free : %" PRIu32 " B",
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  Internal total: %" PRIu32 " B",
             (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  PSRAM free    : %" PRIu32 " B",
             (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "  PSRAM total   : %" PRIu32 " B",
             (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "");
}


// ---------------------------------------------------------------------------
// Layer 5 had a loopback task here that read RX audio and echoed it
// to TX as a plumbing-verification stunt. L7.1 removed it because
// (a) the new `nanojs8_rx_audio` component is the sole RX consumer
// and a second reader would split the FIFO incorrectly, and (b) for
// actual JS8 operation we don't want our mic input echoed back out
// to the radio's audio-in line — that would self-jam if PTT keys.
//
// The heartbeat audio sample counts still tick (the underlying
// driver counts them on every read inside nanojs8_audio_read).

// ---------------------------------------------------------------------------
// L7.11a-fix1: self-test task wrapper
// ---------------------------------------------------------------------------
//
// The TX encoder self-test (Varicode pack/unpack + gfsk8::encode → 79 tones)
// is a stack hog: std::regex_search inside packHeartbeatMessage uses ~8 KB
// alone, and the LDPC encoding chain in gfsk8 adds another ~6 KB. Total
// peak stack is well above the default ESP-IDF main-task allocation
// (~3.5 KB), so calling nanojs8_js8_encode_self_test() directly from
// app_main blows the stack and triggers an immediate reboot loop.
//
// Permanently bumping CONFIG_ESP_MAIN_TASK_STACK_SIZE would solve it but
// permanently bloats DRAM because app_main never exits (it runs the
// heartbeat loop forever). Instead we spawn a one-shot task (size set
// by TX_SELF_TEST_STACK_BYTES below), wait on a semaphore for completion,
// then let it self-delete — extra stack lives only for the duration of
// the test.
//
// This also establishes the task pattern we'll reuse in L7.11d for the
// real on-air TX task.

typedef struct {
    SemaphoreHandle_t done;
    bool              result;
} self_test_ctx_t;

// L7.11f-fix2e: shrunk from 32 KB (fix2c and earlier) to 16 KB. On-air
// measurement under L7.11a-fix1 reported the encode + modulate + render
// chain's high-water at 4100 B used — 16 KB gives ~3× safety margin
// while freeing 16 KB of contiguous internal RAM. fix2d's body-cap
// bump (32→96 chars × 3 static buffers: activity table + screen_directed
// snapshot + screen_all render rows) cost 12 KB of internal RAM, which
// pushed the largest free contiguous block below the 32 KB needed to
// host this one-shot task at boot — manifesting as xTaskCreate rc=-1
// and "TX disabled until next boot" in the L7.11f-fix2d log. The real
// TX worker (tx_worker_task in tx_audio.c) uses a separately-sized
// stack and is unaffected by this constant.
#define TX_SELF_TEST_STACK_BYTES (16u * 1024u)

static void self_test_task(void *arg) {
    self_test_ctx_t *ctx = (self_test_ctx_t *)arg;

    // Phase 1: encoder + modulator (subtests A, B, C).
    ctx->result = nanojs8_js8_encode_self_test();

    // Phase 2: TX audio path (subtest D) — runs only if phase 1 passed,
    // because subtest D reads from the modulator buffer that subtest C
    // populated. A failure of phase 1 means there's no valid modulator
    // output to upsample, so skipping D is correct.
    //
    // We deliberately do NOT short-circuit on a phase-1 failure here
    // because we still want the stack-hwm log line printed below; we
    // just skip the phase-2 call and let the final result reflect the
    // phase-1 result.
    if (ctx->result) {
        if (!nanojs8_tx_audio_self_test()) {
            ESP_LOGE("tx_self_test", "Self-test D failed — "
                                      "TX audio path not operational");
            ctx->result = false;
        }
    }

    // Log stack high-water mark so we know how much of the configured
    // stack was actually consumed by the encode + modulate + render
    // chain. This number directly sizes the real TX worker task.
    // uxTaskGetStackHighWaterMark returns the smallest free stack value
    // seen — subtract from configured size for "max used". Returned in
    // machine words on FreeRTOS Xtensa, so multiply by sizeof(StackType_t)
    // for bytes.
    const UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(NULL);
    const uint32_t hwm_bytes = (uint32_t)hwm_words * sizeof(StackType_t);
    ESP_LOGI("tx_self_test", "Stack high-water: %" PRIu32 " B free of %u KB "
                              "(self-test chain used %" PRIu32 " B)",
             hwm_bytes,
             (unsigned)(TX_SELF_TEST_STACK_BYTES / 1024u),
             (uint32_t)TX_SELF_TEST_STACK_BYTES - hwm_bytes);

    xSemaphoreGive(ctx->done);
    vTaskDeleteWithCaps(NULL);   // L7.13-fix3: matches WithCaps create
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------

void app_main(void) {
    // Layer 2: bring up T-Deck hardware (POWERON, backlight, I²C bus).
    // Do this FIRST so subsequent diagnostics see the real platform state.
    // If init fails, log and continue — the heartbeat will still run so
    // we can read the error via UART. We don't want to brick the boot.
    esp_err_t init_err = tdeck_platform_init();
    if (init_err != ESP_OK) {
        // Logging will only work if the console UART came up despite init
        // failure. Should always succeed since UART0 doesn't depend on
        // POWERON, but worth noting in case of future changes.
        // Don't return — continue to the heartbeat so we can diagnose.
    }

    log_boot_diagnostics();

    // Layer 3: bring up the display. Requires platform init (POWERON and
    // backlight) to have succeeded; gracefully skip if not.
    bool display_ready = false;
    if (init_err == ESP_OK) {
        esp_err_t disp_err = nanojs8_display_init();
        if (disp_err == ESP_OK) {
            display_ready = true;
            ESP_LOGI(TAG, "Display ready — initial UI screen will be painted "
                          "by nanojs8_ui_init after config loads");
        } else {
            ESP_LOGE(TAG, "Display init failed (%s) — continuing without screen",
                     esp_err_to_name(disp_err));
        }
    } else {
        ESP_LOGW(TAG, "Skipping display init: platform init failed earlier");
    }

    ESP_LOGI(TAG, "Layer 5 active. Listening for keys, awaiting audio device.");
    ESP_LOGI(TAG, "Heartbeat every 30 seconds:");
    ESP_LOGI(TAG, "");

    // Layer 6b.1: bring up persistent config (NVS-backed). Done early
    // so subsequent subsystems can consult it (e.g. radio profile, freq).
    // Failure here is non-fatal — defaults are still in the cache so
    // the firmware can run, just won't survive reboots.
    esp_err_t cfg_err = nanojs8_config_init();
    if (cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "Config init non-fatal failure (%s) - running with defaults",
                 esp_err_to_name(cfg_err));
    }

    // Layer 6b.1: bring up the trackball. Adds GPIO 0/1/2/3/15 ISRs for
    // navigation events. Independent of other subsystems but requires
    // platform_init() to have driven POWERON high.
    bool tb_ready = false;
    if (init_err == ESP_OK) {
        esp_err_t tb_err = nanojs8_trackball_start();
        if (tb_err == ESP_OK) {
            tb_ready = true;
            ESP_LOGI(TAG, "Trackball subsystem ready - roll the T-Box");
        } else {
            ESP_LOGE(TAG, "Trackball start failed (%s)", esp_err_to_name(tb_err));
        }
    }

    // Layer 4: bring up the keyboard subsystem.
    bool kb_ready = false;
    if (init_err == ESP_OK) {
        esp_err_t kb_err = nanojs8_keyboard_start();
        if (kb_err == ESP_OK) {
            kb_ready = true;
            ESP_LOGI(TAG, "Keyboard subsystem ready - type on the T-Deck");
        } else {
            ESP_LOGE(TAG, "Keyboard start failed (%s)", esp_err_to_name(kb_err));
        }
    }

    // Layer 5: bring up the USB audio subsystem. This installs the USB
    // host library and the UAC class driver, then waits for a USB audio
    // device to be plugged in (via Y-cable + charger).
    bool audio_ready = false;
    if (init_err == ESP_OK) {
        esp_err_t aud_err = nanojs8_audio_start();
        if (aud_err == ESP_OK) {
            audio_ready = true;
            ESP_LOGI(TAG, "USB audio subsystem started - plug in DigiRig via Y-cable");
        } else {
            ESP_LOGE(TAG, "USB audio start failed (%s)", esp_err_to_name(aud_err));
        }
    }

    // Layer 5b: bring up the USB serial subsystem. MUST come AFTER the audio
    // subsystem — they share the USB host library, which audio.cpp installs.
    // Skipping this on audio failure isn't strictly necessary (serial would
    // install its own CDC-ACM driver and just sit waiting) but it would log
    // confusing "no device" lines forever, so we gate it.
    bool serial_ready = false;
    if (audio_ready) {
        esp_err_t ser_err = nanojs8_serial_start();
        if (ser_err == ESP_OK) {
            serial_ready = true;
            ESP_LOGI(TAG, "USB serial subsystem started - DigiRig CAT/PTT path");
        } else {
            ESP_LOGE(TAG, "USB serial start failed (%s)", esp_err_to_name(ser_err));
        }
    }

    // L7.1: the loopback task that lived here through L7.0 has been
    // removed (see comment near line 168 for why). The new RX audio
    // pipeline gets started later — it depends on the time subsystem
    // being ready (for the slot trigger) — so its start lives near
    // the time_start() call below.

    // Layer 6b.4: bring up the PTT controller. Reads the active radio
    // profile from config and tells the serial layer which line to use
    // for PTT. Starts the 20-second watchdog task. Does NOT require
    // serial to be connected yet — the line config is pushed down to
    // the serial layer's pending state and applied at connect time.
    if (serial_ready) {
        esp_err_t ptt_err = nanojs8_ptt_start();
        if (ptt_err == ESP_OK) {
            ESP_LOGI(TAG, "PTT subsystem started with %dms watchdog",
                     NANOJS8_PTT_WATCHDOG_MS);
        } else {
            ESP_LOGE(TAG, "PTT start failed (%s)", esp_err_to_name(ptt_err));
        }
    } else {
        ESP_LOGW(TAG, "Skipping PTT start: serial subsystem not ready");
    }

    // Layer 6b.5: bring up the CAT facade. Registers an RX callback
    // with the serial layer and configures the CI-V parser based on
    // the active profile's address bytes. Profiles with cat == CAT_NONE
    // just sit silently — no harm in starting CAT in that case.
    // Must come AFTER nanojs8_serial_start() since we register a
    // callback on the serial driver.
    if (serial_ready) {
        esp_err_t cat_err = nanojs8_cat_start();
        if (cat_err == ESP_OK) {
            ESP_LOGI(TAG, "CAT facade started");
        } else {
            ESP_LOGE(TAG, "CAT start failed (%s)", esp_err_to_name(cat_err));
        }
    } else {
        ESP_LOGW(TAG, "Skipping CAT start: serial subsystem not ready");
    }

    // L7.0: time subsystem. Cheap (no tasks, no hardware), but must
    // start BEFORE the UI so HOME's clock band can read is_set()
    // without crashing on uninitialized state. Resets to "not set"
    // every boot — operator enters UTC via SETUP each session.
    esp_err_t time_err = nanojs8_time_start();
    if (time_err == ESP_OK) {
        ESP_LOGI(TAG, "Time subsystem started — enter UTC via SETUP "
                      "(JS8 slot alignment unavailable until then)");
    } else {
        ESP_LOGE(TAG, "Time start failed (%s)", esp_err_to_name(time_err));
    }

    // L7.9: activity store. Cheap — just initialises a mutex and the
    // two static tables. Must come before nanojs8_js8_sync_start so the
    // first decode has a place to land. Failure is non-fatal but blanks
    // the HEARD/DIRECTED screens.
    esp_err_t act_err = nanojs8_activity_init();
    if (act_err == ESP_OK) {
        ESP_LOGI(TAG, "Activity store ready (HEARD/DIRECTED tables)");
    } else {
        ESP_LOGE(TAG, "Activity init failed (%s) — HEARD/DIRECTED "
                      "screens will appear empty", esp_err_to_name(act_err));
    }

    // L7.11g.2: NVS-persistent mailbox. Loads any prior UNREAD/READ/
    // STORE/DELIVERED entries from namespace 'nj8_inbox' at startup;
    // every state change is committed to flash. Non-fatal failure
    // here only affects mailbox operations — TX/RX/UI keep working.
    esp_err_t mb_err = nanojs8_mailbox_init();
    if (mb_err == ESP_OK) {
        ESP_LOGI(TAG, "Mailbox ready (L7.11g.2 NVS-persistent; "
                      "INBOX UI in L7.11g.3)");
    } else {
        ESP_LOGE(TAG, "Mailbox init failed (%s) — store-and-forward "
                      "and INBOX disabled", esp_err_to_name(mb_err));
    }

    // L7.4a: memory-map the JSC dictionary partition. Non-fatal if
    // missing — JS8 decoder will simply fail to come up later. We
    // log loud but keep booting so a malformed jsc_map.bin (e.g.
    // operator forgot to flash it) doesn't brick the whole device.
    esp_err_t jsc_err = nanojs8_jsc_map_init();
    if (jsc_err == ESP_OK) {
        ESP_LOGI(TAG, "JSC dictionary mmap'd: %" PRIu32 " entries, "
                      "pool %" PRIu32 " B",
                 nanojs8_jsc_map_count(), nanojs8_jsc_map_pool_size());

        // L7.11h.1 (Layer C2): build the sorted-by-string lookup index in
        // PSRAM. Required by JSC::exists()/lookup() and (in C3) compress().
        // Idempotent — safe to call multiple times. Non-fatal if it fails
        // (TX compress would degrade, but RX decompress is unaffected, and
        // fix6 sanitization keeps wire reliable either way).
        extern esp_err_t nanojs8_jsc_init(void);
        const esp_err_t jsc_idx_err = nanojs8_jsc_init();
        if (jsc_idx_err == ESP_OK) {
            ESP_LOGI(TAG, "JSC sorted index ready for compress lookups");
        } else {
            ESP_LOGE(TAG, "JSC sorted index init failed (%s) — TX "
                          "compress() will be unavailable; fix6 "
                          "sanitization remains in effect",
                     esp_err_to_name(jsc_idx_err));
        }
    } else {
        ESP_LOGE(TAG, "JSC dictionary load failed (%s) — JS8 decode "
                      "will be unavailable until jsc_map.bin is flashed",
                 esp_err_to_name(jsc_err));
    }

    // L7.1: bring up the RX audio pipeline. Sole consumer of the
    // USB audio RX FIFO from this point forward. Allocates 1.44 MB
    // PSRAM for its ring buffer and spawns two tasks on Core 1
    // (decimator + slot trigger). The slot trigger is gated on UTC
    // being set, so it's harmless if the operator hasn't entered
    // time yet — just sleeps and re-polls.
    //
    // Depends on:
    //   - nanojs8_audio_start (already done above) — for nanojs8_audio_read
    //   - nanojs8_time_start  (just above)         — for slot alignment
    if (audio_ready) {
        esp_err_t rx_err = nanojs8_rx_audio_start();
        if (rx_err == ESP_OK) {
            ESP_LOGI(TAG, "RX audio pipeline started (48k→12k decimator "
                          "+ 30s PSRAM ring + JS8 Normal slot trigger)");
        } else {
            ESP_LOGE(TAG, "RX audio start failed (%s) — JS8 decode "
                          "will be unavailable", esp_err_to_name(rx_err));
        }

        // L7.5: bring up the JS8 sync detector. It runs on Core 1,
        // self-gates on UTC being set, and polls rx_audio's slots_fired
        // counter for new slot snapshots. Per-slot it logs the candidate
        // count + each candidate's score/freq/dt under tag "js8sync".
        // No LDPC decode yet — that arrives at L7.6.
        esp_err_t sync_err = nanojs8_js8_sync_start();
        if (sync_err == ESP_OK) {
            ESP_LOGI(TAG, "JS8 sync detector started (ft8_lib monitor + "
                          "JS8 Normal Costas — tune to 7.078 MHz USB)");
        } else {
            ESP_LOGE(TAG, "JS8 sync detector start failed (%s) — "
                          "JS8 RX unavailable",
                     esp_err_to_name(sync_err));
        }
    } else {
        ESP_LOGW(TAG, "Skipping RX audio start: audio subsystem not ready");
    }

    // L7.11a: TX encoder boot self-test. Verifies that:
    //   (1) Varicode::packHeartbeatMessage + unpackHeartbeatMessage round-trip
    //       a 'HB EN83' frame intact (protocol layer)
    //   (2) gfsk8::encode produces 79 tones with valid [0,7] range and the
    //       expected JS8 Normal Costas {4,2,5,6,1,3,0} at positions 0-6, 36-42,
    //       and 72-78 (physical layer)
    // Both subtests run unconditionally — failure does NOT block boot, but
    // surfaces a clear ERROR in the log so a regression is impossible to miss.
    // Modulation, USB-TX, and PTT land in L7.11b-d; nothing radiates here.
    //
    // L7.11a-fix1: run on a one-shot 32 KB task because the regex inside
    // packHeartbeatMessage and the LDPC chain inside gfsk8::encode each
    // need several KB of stack — the default main-task stack (~3.5 KB)
    // is far too small and crashes during sub-test B (and sometimes A).
    // See self_test_task() above for the full rationale.
    {
        self_test_ctx_t st_ctx = {
            .done   = xSemaphoreCreateBinary(),
            .result = false,
        };
        if (!st_ctx.done) {
            ESP_LOGE(TAG, "TX self-test: could not create sync semaphore — "
                          "skipping (TX disabled until next boot)");
        } else {
            // L7.13-fix3: previously this used xTaskCreate, which puts
            // the 16 KB stack in internal RAM. Late in boot, internal
            // RAM gets fragmented enough that the largest contiguous
            // block can drop below 16 KB even with ~18 KB total free —
            // observed in production as an intermittent TX-disabled
            // boot. Switched to xTaskCreatePinnedToCoreWithCaps with
            // MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT (mirroring
            // tx_worker_task since L7.11h.2-fix2). PSRAM has ~4.8 MB
            // free at this point — fragmentation is not a concern.
            // The matching cleanup is vTaskDeleteWithCaps inside
            // self_test_task above.
            //
            // Diagnostic capture: log both internal and PSRAM heap
            // state right before the create call so any future failure
            // explains itself without needing a debugger.
            ESP_LOGI(TAG,
                "TX self-test: pre-create heap — internal free=%u "
                "largest=%u, PSRAM free=%u largest=%u; requesting "
                "%u-byte stack in PSRAM",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                (unsigned)TX_SELF_TEST_STACK_BYTES);

            BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(
                self_test_task, "tx_self_test",
                TX_SELF_TEST_STACK_BYTES,
                &st_ctx, 5, NULL, tskNO_AFFINITY,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (rc != pdPASS) {
                ESP_LOGE(TAG,
                    "TX self-test: xTaskCreatePinnedToCoreWithCaps "
                    "failed (rc=%d) — even PSRAM declined the stack; "
                    "PSRAM free=%u largest=%u (needed %u). TX disabled "
                    "until next boot.",
                    (int)rc,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                    (unsigned)TX_SELF_TEST_STACK_BYTES);
            } else {
                // Wait for the task to complete. portMAX_DELAY is safe here:
                // self_test_task always signals before vTaskDelete, even if
                // the encoder logic fails internally (result is set to false
                // before the give).
                xSemaphoreTake(st_ctx.done, portMAX_DELAY);
                if (st_ctx.result) {
                    ESP_LOGI(TAG, "TX encoder ready (heartbeat verbs)");

                    // L7.11g.4: bring up the 4-deep TX queue now that
                    // tx_audio is verified working. The queue's drain
                    // task will sit idle until something gets enqueued
                    // (auto-ACK on incoming MSG verbs is the first
                    // producer; future g.4-fix layers add QUERY MSGS
                    // replies, store-and-forward, etc).
                    esp_err_t qerr = nanojs8_tx_queue_init();
                    if (qerr != ESP_OK) {
                        ESP_LOGE(TAG, "tx_queue init failed (%s) — "
                                      "auto-ACK + queued replies "
                                      "DISABLED, COMPOSE-driven TX "
                                      "still works",
                                 esp_err_to_name(qerr));
                    } else {
                        ESP_LOGI(TAG, "TX queue armed "
                                      "(auto-ACK on MSG verbs ON)");
                    }
                } else {
                    ESP_LOGE(TAG, "TX encoder self-test FAILED — "
                                  "TX disabled until fixed");
                }
            }
            vSemaphoreDelete(st_ctx.done);
        }
    }

    // L7.14: GPS subsystem.
    //
    // Phase 0 (always): parser self-test. Exercises the NMEA checksum
    // + RMC parsing code path with fixed strings — no UART involved —
    // so a regression in the parser is caught at boot regardless of
    // whether the operator has CONFIG_NANOJS8_GPS_ENABLED on. Pattern
    // matches the encoder self-test above.
    if (!nanojs8_gps_self_test()) {
        ESP_LOGE(TAG, "GPS parser self-test FAILED — GPS path not "
                      "trusted; manual UTC still works");
        // Don't bail — operator can still enter UTC manually and
        // operate without GPS. Just refuse to proceed with the
        // hardware handover.
    } else if (nanojs8_gps_is_enabled()) {
        // Phase 1 (CONFIG_NANOJS8_GPS_ENABLED=y only): console
        // handover. After this block the UART0 console is gone and
        // the runtime log lines we relied on for debugging
        // (heartbeat-every-30s, per-decode RX lines, SNR values,
        // CAT TX traces) become invisible. The on-screen surface
        // (DIRECTED, HEARD, HOME) is the only runtime diagnostic
        // from here forward. Boot logs above this line still made
        // it out, so the user sees everything up to this point.
        //
        // For runtime debug: rebuild with
        //   idf.py menuconfig → NanoJS8 → GPS = n
        // and the console comes back exactly as today.
        ESP_LOGI(TAG, "GPS enabled — handing UART0 console off to "
                      "GPS UART1 in 200 ms (this is the last log "
                      "line you'll see on serial0; switch to the "
                      "T-Deck screen for runtime status)");

        // Drain anything in flight. fflush is enough for stdout-routed
        // ESP_LOGx; the small delay gives the UART FIFO time to clock
        // out the last bytes at 115200 baud.
        fflush(stdout);
        fflush(stderr);
        vTaskDelay(pdMS_TO_TICKS(200));

        // Silence all future ESP_LOGx — they'd otherwise try to write
        // to a UART0 we're about to tear down. Set BEFORE the driver
        // teardown to close the race window.
        esp_log_level_set("*", ESP_LOG_NONE);

        // Tear down UART0. Default IDF boot uses polling/blocking
        // writes (no driver installed); uart_driver_delete returns
        // ESP_FAIL silently in that case, which we ignore. We then
        // explicitly free both GPIOs from the UART0 pin matrix.
        (void)uart_driver_delete(UART_NUM_0);
        gpio_reset_pin(GPIO_NUM_43);
        gpio_reset_pin(GPIO_NUM_44);

        // Phase 2: bring up GPS on UART1. On failure, the operator
        // can still set UTC manually via SETUP row 6 — the rest of
        // the firmware doesn't depend on GPS being up.
        esp_err_t gps_err = nanojs8_gps_init();
        if (gps_err != ESP_OK) {
            // Logging is silenced at this point, but the HOME GPS
            // row will show NO_FIX permanently so the operator sees
            // the failure on screen.
            (void)gps_err;
        }
    }

    // Layer 6b.2: bring up the UI dispatcher. Must be the LAST init step
    // since it picks the initial screen based on config state and paints
    // it. Requires display + config to be ready; safely skips its render
    // calls below if display_ready is false.
    if (display_ready) {
        esp_err_t ui_err = nanojs8_ui_init();
        if (ui_err != ESP_OK) {
            ESP_LOGE(TAG, "UI init failed (%s) - screens will not render",
                     esp_err_to_name(ui_err));
            display_ready = false;
        }
    }

    // Main loop: drain keyboard + trackball events into the UI dispatcher,
    // render the active screen, emit heartbeat every 30 seconds.
    //
    // The active screen refreshes dynamic content (audio peak meter,
    // serial state, trackball counters) on each iteration; static
    // chrome (header, separator) was painted once by the screen's
    // on_enter callback when it became active.
    //
    // Layer 6b.1 shortens the keyboard wait from 500 ms to 100 ms so
    // trackball events (which are queue-buffered) get drained quickly.
    // 100 ms is well below human perception of latency for navigation.
    const uint32_t KEY_WAIT_MS = 100;
    uint32_t seconds = 0;
    uint32_t last_heartbeat_sec = 0;
    uint32_t last_keys_logged = 0;
    uint64_t last_rx_samples_logged = 0;
    while (true) {
        // Block up to 100 ms waiting for a key. The UI dispatcher will
        // route printable keys to the focused field once edit mode lands
        // in L6b.3; for L6b.2 only TAB/arrows/CLICK do anything.
        uint8_t key = kb_ready ?
                      nanojs8_keyboard_get_key(KEY_WAIT_MS) : 0;
        if (key != 0) {
            nanojs8_ui_handle_input(key);
        } else if (!kb_ready && !audio_ready && !tb_ready) {
            // Nothing active; just sleep so we don't spin.
            vTaskDelay(pdMS_TO_TICKS(KEY_WAIT_MS));
        }

        // Drain ALL pending trackball events (non-blocking). Each event
        // is routed to the UI dispatcher which forwards to the active
        // screen's handle_input — UP/DOWN move focus, LEFT/RIGHT switch
        // screens (where allowed), CLICK is reserved for edit mode in
        // L6b.3.
        if (tb_ready) {
            uint8_t tb_ev;
            while ((tb_ev = nanojs8_trackball_get_event(0)) != 0) {
                nanojs8_ui_handle_input(tb_ev);
            }
        }

        // L6b.6: per-iteration CAT maintenance. Fires deferred initial
        // freq probe when serial finally becomes ready (handles the
        // boot-time race where the radio profile was applied before
        // the CP2102 enumerated). Cheap when nothing pending — single
        // atomic load.
        nanojs8_cat_tick();

        // Render the active screen. Internally tracks "first draw after
        // entry" so it does the full repaint then incremental updates
        // for the dynamic status rows on subsequent calls.
        if (display_ready) {
            nanojs8_ui_render();
        }

        // Heartbeat
        seconds = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
        if (seconds - last_heartbeat_sec >= 30) {
            last_heartbeat_sec = seconds;
            uint32_t total_keys = kb_ready ? nanojs8_keyboard_total_keys() : 0;
            uint64_t rx_samp = audio_ready ? nanojs8_audio_rx_samples_total() : 0;
            uint64_t tx_samp = audio_ready ? nanojs8_audio_tx_samples_total() : 0;

            // Format audio info compactly. nanojs8_audio_*_info() are
            // safe to call even before a device is connected — they
            // just return zeros.
            nanojs8_audio_stream_info_t rx_info = {0}, tx_info = {0};
            nanojs8_audio_rx_info(&rx_info);
            nanojs8_audio_tx_info(&tx_info);

            // Serial counters and state. Safe to query before serial is
            // started (returns UNAVAILABLE / 0s).
            nanojs8_serial_info_t ser_info = {0};
            nanojs8_serial_get_info(&ser_info);
            uint64_t ser_tx = serial_ready ? nanojs8_serial_tx_bytes_total() : 0;
            uint64_t ser_rx = serial_ready ? nanojs8_serial_rx_bytes_total() : 0;
            const char* ser_state =
                (ser_info.status == NANOJS8_SERIAL_STATUS_READY)       ? "ready" :
                (ser_info.status == NANOJS8_SERIAL_STATUS_OPENING)     ? "open" :
                (ser_info.status == NANOJS8_SERIAL_STATUS_ERROR)       ? "err"  :
                                                                          "n/a";

            // Layer 6b.1: trackball counters (per-direction) and config state.
            uint32_t tb_u = tb_ready ? nanojs8_trackball_count(NANOJS8_TRACKBALL_UP)    : 0;
            uint32_t tb_d = tb_ready ? nanojs8_trackball_count(NANOJS8_TRACKBALL_DOWN)  : 0;
            uint32_t tb_l = tb_ready ? nanojs8_trackball_count(NANOJS8_TRACKBALL_LEFT)  : 0;
            uint32_t tb_r = tb_ready ? nanojs8_trackball_count(NANOJS8_TRACKBALL_RIGHT) : 0;
            uint32_t tb_c = tb_ready ? nanojs8_trackball_count(NANOJS8_TRACKBALL_CLICK) : 0;
            const nanojs8_config_t *cfg = nanojs8_config_get();
            const char *cfg_state = nanojs8_config_is_configured() ? "OK" : "SETUP_NEEDED";

            // Layer 6b.5 hotfix1: CAT counters for first-flight visibility.
            // tx     — commands we sent (read_freq, set_freq)
            // ok     — frames received from the radio (post echo filter)
            // echo   — frames recognized as our own half-duplex echoes
            // drop   — malformed / oversize frames the parser threw out
            //
            // If tx > 0 but ok == 0 → our command went out but the radio
            // didn't reply (check baud, CIV addr, cable). If echo > 0
            // matches tx → half-duplex is wired so the radio's wire echoes
            // our bytes back; that's normal. If drop > 0 → noise on the
            // line or a protocol mismatch; investigate.
            uint32_t cat_tx = 0, cat_ok = 0, cat_echo = 0, cat_drop = 0;
            nanojs8_cat_get_counters(&cat_ok, &cat_echo, &cat_drop, &cat_tx);

            // L7.1: RX audio pipeline stats. Slots fired since boot is
            // the primary "is JS8 alignment working" signal. Last peak
            // is a rough audio-level indicator (0..32767). Ring fill %
            // tells us the ring buffer is keeping up with the decimator.
            nanojs8_rx_audio_stats_t rxa = {0};
            nanojs8_rx_audio_get_stats(&rxa);
            uint32_t ring_fill_pct = (uint32_t)((uint64_t)rxa.ring_samples_total
                                                * 100 /
                                                NANOJS8_RX_AUDIO_RING_SAMPLES);
            if (ring_fill_pct > 100) ring_fill_pct = 100;

            // L7.5: gfsk8 JS8 sync detector stats. Replaces the L7.4c
            // gfsk8::Decoder stats. Reports candidate counts (Costas
            // sync matches per slot) — no decoded payloads yet, that's
            // L7.6 territory.
            nanojs8_js8_sync_stats_t syn = {0};
            nanojs8_js8_sync_get_stats(&syn);

            // L7.11g.4: TX queue observability — depth + lifetime
            // totals so we can spot drained ACKs, overflow drops,
            // or tx_err drops in the heartbeat without resorting
            // to per-event logs.
            nanojs8_tx_queue_stats_t txq = {0};
            nanojs8_tx_queue_get_stats(&txq);

            // L7.0: UTC clock state. When the operator hasn't entered
            // UTC yet, the heartbeat says so explicitly — operators
            // need an obvious "no slot alignment available" signal
            // for the upcoming JS8 layers.
            //
            // Buffer sizing: worst-case format expansion is
            //   "UTC HH:MM:SS (set NNNNNNNs ago)\0"
            //   = 4 + 8 + 6 + 7 + 6 + 1 = 32 bytes (PRIu32 max 7 digits
            //   since age_ms is uint32 / 1000 → cap 4,294,967).
            // We size to 40 for safety margin. GCC -Werror=format-truncation
            // is unforgiving — last L7.0 build failed at 24 bytes (lesson).
            char utc_buf[40];
            uint8_t uh = 0, um = 0, us = 0;
            if (nanojs8_time_get_utc(&uh, &um, &us)) {
                uint32_t age_s = nanojs8_time_age_ms() / 1000;
                snprintf(utc_buf, sizeof(utc_buf),
                         "UTC %02u:%02u:%02u (set %" PRIu32 "s ago)",
                         (unsigned)uh, (unsigned)um, (unsigned)us, age_s);
            } else {
                snprintf(utc_buf, sizeof(utc_buf), "UTC not set");
            }

            ESP_LOGI(TAG, "[heartbeat] %" PRIu32 "s up, %s, "
                          "%" PRIu32 " B int free, "
                          "%" PRIu32 " B psram free, "
                          "keys=%" PRIu32 " (+%" PRIu32 "), "
                          "audio rx=%llu (+%llu) tx=%llu, "
                          "rx_rate=%" PRIu32 " tx_rate=%" PRIu32 ", "
                          "ser %s @%" PRIu32 " TX=%llu RX=%llu, "
                          "tb U=%" PRIu32 " D=%" PRIu32 " L=%" PRIu32
                          " R=%" PRIu32 " C=%" PRIu32 ", "
                          "cat tx=%" PRIu32 " ok=%" PRIu32
                          " echo=%" PRIu32 " drop=%" PRIu32 ", "
                          "rx_audio slots=%" PRIu32 " peak=%d ring=%" PRIu32 "%%, "
                          "js8sync slots=%" PRIu32 " cand_last=%" PRIu32
                          " cand_total=%" PRIu32 " best_score=%d "
                          "dec_last=%" PRIu32 " dec_total=%" PRIu32 " "
                          "cpu=%" PRIu32 "ms stack_min=%" PRIu32 "B, "
                          "txq d=%" PRIu32 " enq=%" PRIu32 " tx=%" PRIu32
                          " ov=%" PRIu32 " err=%" PRIu32 ", "
                          "cfg=%s call=%s grid=%s",
                     seconds, utc_buf,
                     (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     total_keys, total_keys - last_keys_logged,
                     (unsigned long long)rx_samp,
                     (unsigned long long)(rx_samp - last_rx_samples_logged),
                     (unsigned long long)tx_samp,
                     rx_info.sample_rate, tx_info.sample_rate,
                     ser_state, ser_info.baud_rate,
                     (unsigned long long)ser_tx,
                     (unsigned long long)ser_rx,
                     tb_u, tb_d, tb_l, tb_r, tb_c,
                     cat_tx, cat_ok, cat_echo, cat_drop,
                     rxa.slots_fired, (int)rxa.last_slot_peak, ring_fill_pct,
                     syn.slots_processed, syn.last_slot_candidates,
                     syn.total_candidates, (int)syn.last_slot_best_score,
                     syn.last_slot_decodes, syn.total_decodes,
                     syn.last_slot_cpu_ms, syn.stack_min_free,
                     txq.depth, txq.total_enqueued,
                     txq.total_transmitted,
                     txq.total_dropped_overflow,
                     txq.total_dropped_tx_err,
                     cfg_state, cfg->callsign,
                     cfg->grid[0] ? cfg->grid : "(unset)");
            last_keys_logged = total_keys;
            last_rx_samples_logged = rx_samp;
        }
    }
}
