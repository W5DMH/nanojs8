/*
 * platform_tdeck.cpp — LilyGO T-Deck hardware platform initialization
 * ====================================================================
 * See platform_tdeck.h for API documentation.
 *
 * Implementation notes:
 *
 *   POWERON pin (GPIO 10): The T-Deck has a single power rail FET that
 *   gates power to ALL peripherals — display backlight, ESP32-C3 keyboard
 *   coprocessor, touch controller, SX1262 LoRa, ES7210 codec, microSD.
 *   None of those will work until GPIO 10 is driven HIGH. This is the
 *   FIRST thing we do at platform init, before any peripheral access.
 *
 *   Settle delay (100 ms): The power rail takes a few ms to stabilize.
 *   100 ms is conservative; halves of that work in practice but we have
 *   plenty of boot-time budget and the safety margin is worth it.
 *
 *   I²C scan: We scan addresses 0x08..0x77 (the 7-bit USB range) and log
 *   any that ACK. The keyboard ESP32-C3 should respond at 0x55. The GT911
 *   touch controller should respond at 0x5D OR 0x14 (depends on board
 *   variant). Other addresses can show up if the operator has attached
 *   peripherals to Grove.
 *
 *   I²C error log suppression: ESP-IDF's new I²C driver loudly logs every
 *   NACK as an ERROR-level message, which is fine in production but
 *   floods the scan with 100+ errors. We bump the i2c.master log level
 *   to NONE for the scan duration, then restore.
 *
 *   LoRa keepout: The SX1262 is on the SHARED SPI bus (MOSI/MISO/SCK).
 *   If it were powered up and listening, it could spuriously respond to
 *   SPI traffic intended for the display. We hold it in reset (RST LOW)
 *   AND drive its CS HIGH (inactive) to be doubly sure.
 *
 *   ES7210 codec: Not actively initialized. After POWERON, the codec
 *   starts up in a low-power idle state and stays there until something
 *   writes to its I²C control interface. NanoJS8 never does, so it stays
 *   asleep and draws minimal current.
 */

#include "platform_tdeck.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace nanojs8 {
namespace platform {
namespace tdeck {

static const char* TAG = "platform_tdeck";

// File-static storage for the I²C bus handle. Created once in init(),
// retrieved via i2c_bus() by subsequent layers.
static i2c_master_bus_handle_t s_i2c_bus = nullptr;

// -----------------------------------------------------------------------
// GPIO configuration helpers
// -----------------------------------------------------------------------

// Configure a single GPIO as output with no pull, then drive it to the
// specified level. The T-Deck doesn't use pulls on these pins — the
// peripherals have their own pull resistors as needed.
static esp_err_t gpio_init_output(int pin, int initial_level) {
    gpio_config_t cfg = {
        .pin_bit_mask  = 1ULL << pin,
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) failed: %s", pin, esp_err_to_name(err));
        return err;
    }
    return gpio_set_level(static_cast<gpio_num_t>(pin), initial_level);
}

// -----------------------------------------------------------------------
// I²C bus scan diagnostic
// -----------------------------------------------------------------------

// Scan addresses 0x08..0x77, log everything that ACKs.
//
// The new ESP-IDF I²C driver logs an ERROR for every NACK, which would
// flood the console with 100+ messages during a full scan. We temporarily
// silence i2c.master and restore after.
//
// Each probe has a 50 ms timeout — enough to be reliable, short enough
// that a full scan completes in < 6 seconds even if every device times
// out (which they won't; ACKs are sub-millisecond).
//
// Known devices to expect on T-Deck:
//   0x55 — ESP32-C3 keyboard coprocessor
//   0x5D or 0x14 — GT911 touch (one of these, depending on board variant)
static void i2c_scan_and_log() {
    if (!s_i2c_bus) {
        ESP_LOGW(TAG, "i2c_scan_and_log: bus not initialized, skipping");
        return;
    }

    // Silence the driver's per-NACK error log during the scan
    esp_log_level_t prev_level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    ESP_LOGI(TAG, "I²C scan on GPIO %d (SDA) / GPIO %d (SCL) @ %d Hz:",
             PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        esp_err_t err = i2c_master_probe(s_i2c_bus, addr, /*timeout_ms=*/50);
        if (err == ESP_OK) {
            const char* note = "";
            switch (addr) {
                case I2C_ADDR_KEYBOARD: note = " — ESP32-C3 keyboard"; break;
                case I2C_ADDR_TOUCH_A:
                case I2C_ADDR_TOUCH_B:  note = " — GT911 touch";       break;
                default: break;
            }
            ESP_LOGI(TAG, "  0x%02X ACK%s", addr, note);
            ++found;
        }
    }

    // Restore previous log level
    esp_log_level_set("i2c.master", prev_level);

    if (found == 0) {
        ESP_LOGW(TAG, "  No I²C devices responded — keyboard and touch are "
                      "missing. Either POWERON didn't enable peripherals, "
                      "or the keyboard ESP32-C3 firmware is corrupted.");
    } else {
        ESP_LOGI(TAG, "I²C scan complete: %d device(s) found", found);
    }
}

// -----------------------------------------------------------------------
// init()
// -----------------------------------------------------------------------

esp_err_t init() {
    // L7.11i.fix4: Align with the documented LilyGO recipe and
    // Meshtastic's earlyInitVariant() pattern. Both reference firmwares
    // (which run reliably on this exact hardware per user testimony
    // Jun 8 2026) drive POWERON HIGH as the FIRST thing — before any
    // logging, before any other GPIO configuration, before any I²C
    // pin manipulation. They then WAIT (LilyGO's example uses 500 ms;
    // their comment says verbatim "give LILYGO-KEYBOARD some startup
    // time"). Only after the wait do they touch the I²C bus.
    //
    // Two crucial things we were doing wrong:
    //   (a) L7.11i.fix1 preconditioned SDA/SCL HIGH as GPIO outputs
    //       BEFORE POWERON. LilyGO doesn't do this; Meshtastic doesn't
    //       do this. The likely problem: when the C3 wakes up on the
    //       peripheral rail, it sees its I²C pins actively driven from
    //       outside (rather than the floating-then-pulled-by-bus state
    //       it expects). Its I²C peripheral may interpret this as bus
    //       activity in progress and lock up. Then when we later
    //       transition SDA/SCL from GPIO-output mode to I²C-master
    //       mode, glitches on the transition can compound the
    //       confusion. Removed in this fix.
    //   (b) We waited only 100 ms after POWERON before initializing
    //       the I²C bus. LilyGO explicitly uses 500 ms. Field-test
    //       boot logs Jun 8 2026 showed the C3 either responded at
    //       the I²C scan (~150 ms after POWERON) or stayed dead — a
    //       longer settle delay BEFORE bus init might catch the
    //       cases where C3 needs more time to come up cleanly.
    //
    // Cost: +400 ms boot time per boot (100 → 500 ms). Benefit:
    // matches the recipe that works on the reference firmwares.
    //
    // History: L7.4a-fix1 (Jun 4 2026) tried POWERON LOW→HIGH cycling
    // — caused BOD via inrush, reverted in L7.4a-fix2. L7.11i added
    // 1 s pre-probe delay in kb subsystem — didn't help (delay was
    // AFTER bus init, too late). L7.11i.fix1 added SDA/SCL
    // preconditioning — didn't help, may have hurt; removed here.
    // L7.11i.fix2 added auto-restart via esp_restart() — 0/4 success
    // (peripheral rail doesn't collapse during S3 soft-reset);
    // removed in L7.11i.fix3.

    // STEP 1: POWERON HIGH — must be the FIRST action.
    //
    // From Meshtastic's variants/esp32s3/t-deck/variant.cpp comment:
    //   "GPIO10 manages all peripheral power supplies"
    //   "Turn on peripheral power immediately after MUC starts."
    //   "If some boards are turned on late, ESP32 will reset due to low voltage."
    esp_err_t err = gpio_init_output(PIN_POWERON, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drive POWERON pin: %s", esp_err_to_name(err));
        return err;
    }

    // STEP 2: settle delay. LilyGO's official keyboard example uses 500 ms,
    // commented as "give LILYGO-KEYBOARD some startup time". This is the
    // ESP32-C3 keyboard coprocessor boot time on the freshly-energized
    // peripheral rail. WE MUST NOT TOUCH THE I²C BUS DURING THIS WAIT —
    // the C3 needs to come up with its I²C pins seeing the natural
    // floating-then-bus-pullup state, NOT driven from outside.
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "T-Deck platform init starting...");
    ESP_LOGI(TAG, "  POWERON (GPIO %d) HIGH — peripheral rail enabled "
                  "(per LilyGO recipe: 500 ms settle delay completed "
                  "before any I²C activity)",
             PIN_POWERON);

    // STEP 3: Backlight ON. Visual indicator that init reached this stage.
    err = gpio_init_output(PIN_BACKLIGHT, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to drive backlight pin: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  Backlight (GPIO %d) ON", PIN_BACKLIGHT);

    // 4. Hold LoRa SX1262 in reset and assert its CS HIGH (inactive).
    //    This keeps the chip off the shared SPI bus so it can't interfere
    //    with display traffic. Phase 6+ will properly init the LoRa
    //    module; until then, we want it silent.
    err = gpio_init_output(PIN_RADIO_RST, 0);   // LOW = held in reset
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to hold LoRa in reset: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_init_output(PIN_RADIO_CS, 1);    // HIGH = inactive
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deassert LoRa CS: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  LoRa SX1262 held in reset (RST=LOW, CS=HIGH) — off SPI bus");

    // 5. Initialize the I²C master bus. Shared by keyboard (Layer 4) and
    //    touch (Layer 7+, deferred). We create the bus here; devices
    //    register themselves later via i2c_master_bus_add_device().
    //
    //    glitch_ignore_cnt=7 is the standard default for cleanish wiring.
    //    enable_internal_pullup=true because the T-Deck doesn't have
    //    strong external pullups on this bus (it's a short on-PCB trace).
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = static_cast<gpio_num_t>(PIN_I2C_SDA),
        .scl_io_num = static_cast<gpio_num_t>(PIN_I2C_SCL),
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,         // 0 = let driver pick
        .trans_queue_depth = 0,     // 0 = synchronous mode only
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,      // don't power-down during light sleep
        },
    };
    err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  I²C master bus created (port %d)", I2C_PORT);

    // 6. Bus scan diagnostic. Logs which devices ACK so the operator can
    //    verify the keyboard ESP32-C3 firmware is alive (0x55) and the
    //    touch controller is present (0x5D or 0x14).
    i2c_scan_and_log();

    ESP_LOGI(TAG, "T-Deck platform init OK");
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus() {
    return s_i2c_bus;
}

void backlight_set(bool on) {
    gpio_set_level(static_cast<gpio_num_t>(PIN_BACKLIGHT), on ? 1 : 0);
}

} // namespace tdeck
} // namespace platform
} // namespace nanojs8

// ---------------------------------------------------------------------------
// C interface
// ---------------------------------------------------------------------------

extern "C" esp_err_t tdeck_platform_init(void) {
    return nanojs8::platform::tdeck::init();
}

extern "C" void tdeck_platform_backlight_set(bool on) {
    nanojs8::platform::tdeck::backlight_set(on);
}
