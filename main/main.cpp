// NanoJS8 — main.cpp
//
// Phase 0 deliverable: boot to a splash screen on the Cardputer ADV's 240x135
// ST7789, displaying:
//   - "NanoJS8 v0.0.1"
//   - ESP32-S3 chip revision
//   - Free internal DRAM
//   - SD-card mount status
//   - Build timestamp
//
// The same information is also logged via ESP_LOG so the developer running
// `idf.py monitor` sees identical content over serial.
//
// This file is intentionally small. Phase 1 onwards will replace this monolith
// with a screen-router that calls into nanojs8_ui/.
//
// References:
//   - Mini-FT8 main.cpp (AG6AQ/N6HAN) — SD pin numbers and mount sequence
//   - M5Stack Cardputer-Adv hardware spec — display orientation, pin map
//   - ESP-IDF v5.5.4 docs — heap_caps_get_free_size, esp_chip_info

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

// SD-card stack (lifted from Mini-FT8, exact same pin numbers per
// Build Specification §2.3).
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

// M5Stack display + keyboard + power. Driver init via M5Unified::begin().
#include <M5Cardputer.h>

#include "version.h"

static const char* TAG = "nanojs8";

// ----------------------------------------------------------------------------
// SD-card mount — pins and sequence per Mini-FT8 (verified-working) and
// Build Specification §2.3 pin map.
// ----------------------------------------------------------------------------
#define PIN_NUM_MISO GPIO_NUM_39
#define PIN_NUM_MOSI GPIO_NUM_14
#define PIN_NUM_CLK  GPIO_NUM_40
#define PIN_NUM_CS   GPIO_NUM_12

static sdmmc_card_t* g_sd_card    = nullptr;
static bool          g_sd_mounted = false;

// Mount the microSD card at /sdcard. Returns ESP_OK on success.
// Leaves g_sd_mounted and g_sd_card set as side effects so later code can
// query state without re-mounting.
static esp_err_t mount_sd_spi() {
    const char* mount_point = "/sdcard";

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = PIN_NUM_MOSI;
    bus_cfg.miso_io_num     = PIN_NUM_MISO;
    bus_cfg.sclk_io_num     = PIN_NUM_CLK;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = 4000;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SPI2_HOST;

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;     // Never auto-format user data.
    mount_config.max_files              = 5;
    mount_config.allocation_unit_size   = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 5000;  // Conservative — Phase 4 may bump to 20 MHz.

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config,
                                  &mount_config, &g_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_sdspi_mount failed: %s (this is OK on Phase 0 "
                      "if no card inserted)", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        g_sd_card    = nullptr;
        g_sd_mounted = false;
        return ret;
    }

    g_sd_mounted = true;
    ESP_LOGI(TAG, "SD card mounted: %lluMB, sector=%u",
             ((uint64_t)g_sd_card->csd.capacity * g_sd_card->csd.sector_size) / (1024 * 1024),
             g_sd_card->csd.sector_size);
    return ESP_OK;
}

// ----------------------------------------------------------------------------
// Splash screen render.
// Cardputer ADV display is 240x135 landscape; M5GFX handles rotation via
// the M5.Display object exposed by M5Cardputer.
// ----------------------------------------------------------------------------
static void render_splash(uint32_t free_dram_kb, bool sd_ok,
                          uint32_t chip_rev_major, uint32_t chip_rev_minor) {
    auto& d = M5Cardputer.Display;

    d.setRotation(1);                  // Landscape: USB-C on the right.
    d.fillScreen(TFT_BLACK);

    // Title bar — green.
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(8, 8);
    d.printf("NanoJS8 v%s", NANOJS8_VERSION);

    // Phase indicator — small text under the title.
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(8, 30);
    d.printf("%s", NANOJS8_VERSION_PHASE);

    // Diagnostic block — white text, single-pixel font.
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(8, 48);
    d.printf("Chip:      ESP32-S3 rev v%u.%u",
             (unsigned)chip_rev_major, (unsigned)chip_rev_minor);
    d.setCursor(8, 60);
    d.printf("Free DRAM: %lu KB", (unsigned long)free_dram_kb);

    // SD status — colored by result so it's eye-catching.
    d.setCursor(8, 72);
    if (sd_ok) {
        d.setTextColor(TFT_GREEN, TFT_BLACK);
        d.printf("SD:        Present");
    } else {
        d.setTextColor(TFT_RED, TFT_BLACK);
        d.printf("SD:        Not Found");
    }

    // Build timestamp — bottom of screen, dim.
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(8, 110);
    d.printf("Built %s %s", NANOJS8_BUILD_DATE, NANOJS8_BUILD_TIME);

    // Footer marker so we can visually distinguish Phase 0 from later phases.
    d.setCursor(8, 122);
    d.setTextColor(TFT_DARKGREEN, TFT_BLACK);
    d.printf("[Phase 0 boot diagnostic]");
}

// ----------------------------------------------------------------------------
// app_main — ESP-IDF entry point.
// ----------------------------------------------------------------------------
extern "C" void app_main(void) {
    // Pre-display log — visible only via `idf.py monitor`.
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " NanoJS8 v%s  -  %s", NANOJS8_VERSION, NANOJS8_VERSION_PHASE);
    ESP_LOGI(TAG, " Built %s %s", NANOJS8_BUILD_DATE, NANOJS8_BUILD_TIME);
    ESP_LOGI(TAG, "================================================");

    // ------------------------------------------------------------------------
    // NVS init. M5Unified::begin() can read NVS for board calibration so we
    // must initialize NVS first. Pattern lifted from Mini-FT8.
    // ------------------------------------------------------------------------
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase; reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // ------------------------------------------------------------------------
    // M5Unified init — brings up display, keyboard, power management.
    // Cardputer ADV requires the .external_imu = false config (no IMU on this
    // SKU); leaving defaults is correct.
    // ------------------------------------------------------------------------
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, false);  // false = don't enable keyboard yet — Phase 1.

    // ------------------------------------------------------------------------
    // Chip + memory snapshot.
    // ------------------------------------------------------------------------
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t free_dram_kb = free_dram / 1024;

    ESP_LOGI(TAG, "Chip: ESP32-S3 rev v%u.%u, %d cores, %s%s%s",
             (unsigned)(chip_info.revision / 100),
             (unsigned)(chip_info.revision % 100),
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
             (chip_info.features & CHIP_FEATURE_BT)       ? "BT/"   : "",
             (chip_info.features & CHIP_FEATURE_BLE)      ? "BLE"   : "");



    ESP_LOGI(TAG, "Free DRAM: %lu bytes (%lu KB)",
             (unsigned long)free_dram, (unsigned long)free_dram_kb);

    // ------------------------------------------------------------------------
    // SD-card mount attempt. Failure is non-fatal in Phase 0 — we just show
    // "Not Found" on screen. Phase 4+ will treat absent SD as a hard error.
    // ------------------------------------------------------------------------
    esp_err_t sd_err = mount_sd_spi();
    bool sd_ok = (sd_err == ESP_OK) && g_sd_mounted;
    ESP_LOGI(TAG, "SD status: %s", sd_ok ? "PRESENT" : "NOT FOUND");

    // ------------------------------------------------------------------------
    // Render splash. Stays on screen forever in Phase 0 — there's no main loop
    // doing anything else.
    // ------------------------------------------------------------------------
    render_splash(free_dram_kb, sd_ok,
                  chip_info.revision / 100,
                  chip_info.revision % 100);

    ESP_LOGI(TAG, "Splash rendered. Phase 0 boot complete.");
    ESP_LOGI(TAG, "Definition-of-done: DRAM >= 200 KB and screen visible.");

    // Idle loop. Phase 1 replaces this with the screen-router task.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
