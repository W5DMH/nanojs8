// NanoJS8 — main.cpp (Phase 1)
//
// Boot sequence:
//   1. NVS init
//   2. M5Cardputer init (display + keyboard enabled)
//   3. Boot diagnostics to ESP_LOG (chip, DRAM, SD)
//   4. Mount SD (informational in Phase 1; required from Phase 4)
//   5. Splash screen for ~1.2 sec
//   6. Load config from NVS (first-boot writes defaults)
//   7. Hand off to the screen router (pinned to core 0) which renders
//      the SETUP screen and processes keyboard input forever.
//
// References:
//   - Mini-FT8 main.cpp (AG6AQ/N6HAN) — SD-mount sequence and pin map
//   - Build Specification §2.3 — pin map
//   - Build Specification §11 — phase delivery plan

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include <M5Cardputer.h>

#include "version.h"
#include "config_store.h"
#include "screen_router.h"

static const char* TAG = "nanojs8";

// SD-card pins per Build Specification §2.3 / Mini-FT8 verified working.
#define PIN_NUM_MISO GPIO_NUM_39
#define PIN_NUM_MOSI GPIO_NUM_14
#define PIN_NUM_CLK  GPIO_NUM_40
#define PIN_NUM_CS   GPIO_NUM_12

static sdmmc_card_t* g_sd_card    = nullptr;
static bool          g_sd_mounted = false;

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
    mount_config.format_if_mount_failed = false;
    mount_config.max_files              = 5;
    mount_config.allocation_unit_size   = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 5000;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config,
                                  &mount_config, &g_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_sdspi_mount failed: %s (OK if no card)",
                 esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        g_sd_card    = nullptr;
        g_sd_mounted = false;
        return ret;
    }

    g_sd_mounted = true;
    ESP_LOGI(TAG, "SD mounted: %lluMB sector=%u",
             ((uint64_t)g_sd_card->csd.capacity * g_sd_card->csd.sector_size) / (1024 * 1024),
             g_sd_card->csd.sector_size);
    return ESP_OK;
}

// Splash screen — same content as Phase 0 but auto-dismisses after ~1.2s.
// Visible for long enough that the operator sees the firmware version
// and DRAM/SD status before SETUP takes over.
static void render_splash(uint32_t free_dram_kb, bool sd_ok,
                          uint32_t chip_rev_major, uint32_t chip_rev_minor) {
    auto& d = M5Cardputer.Display;
    d.setRotation(1);
    d.fillScreen(TFT_BLACK);

    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(8, 8);
    d.printf("NanoJS8 v%s", NANOJS8_VERSION);

    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setTextSize(1);
    d.setCursor(8, 30);
    d.printf("%s", NANOJS8_VERSION_PHASE);

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(8, 48);
    d.printf("Chip:      ESP32-S3 rev v%u.%u",
             (unsigned)chip_rev_major, (unsigned)chip_rev_minor);
    d.setCursor(8, 60);
    d.printf("Free DRAM: %lu KB", (unsigned long)free_dram_kb);

    d.setCursor(8, 72);
    if (sd_ok) {
        d.setTextColor(TFT_GREEN, TFT_BLACK);
        d.printf("SD:        Present");
    } else {
        d.setTextColor(TFT_RED, TFT_BLACK);
        d.printf("SD:        Not Found");
    }

    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(8, 110);
    d.printf("Built %s %s", NANOJS8_BUILD_DATE, NANOJS8_BUILD_TIME);
    d.setCursor(8, 122);
    d.setTextColor(TFT_DARKGREEN, TFT_BLACK);
    d.printf("[loading...]");
}

// UI task — pinned to core 0. Renders the active screen and processes
// keyboard input at ~20 Hz. Lives forever; no return.
static void ui_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "UI task started on core %d", xPortGetCoreID());

    nanojs8::ui::router_init();

    // Boot-screen selection (Phase 2):
    //   - Callsign is still the "NOCALL" placeholder → SETUP, so the
    //     operator gets the config screen immediately.
    //   - Otherwise → HOME, the at-a-glance status view.
    // Matches MicroJS8's "configure first, operate after" bias.
    const bool need_setup = nanojs8::config::is_default_callsign();
    const nanojs8::ui::ScreenId initial =
        need_setup ? nanojs8::ui::ScreenId::SETUP
                   : nanojs8::ui::ScreenId::HOME;
    ESP_LOGI(TAG, "Boot screen: %s (callsign %s)",
             need_setup ? "SETUP" : "HOME",
             need_setup ? "is default placeholder" : "is configured");
    nanojs8::ui::router_set_screen(initial);

    const TickType_t tick_period = pdMS_TO_TICKS(50);  // 20 Hz
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        nanojs8::ui::router_tick();
        vTaskDelayUntil(&last_wake, tick_period);
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " NanoJS8 v%s  -  %s", NANOJS8_VERSION, NANOJS8_VERSION_PHASE);
    ESP_LOGI(TAG, " Built %s %s", NANOJS8_BUILD_DATE, NANOJS8_BUILD_TIME);
    ESP_LOGI(TAG, "================================================");

    // NVS first — needed by both M5Unified and our config_store.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase; reformatting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // M5Unified + display + keyboard. Phase 1 enables the keyboard
    // (true) — Phase 0 had it false because nothing read it.
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    // Diagnostics.
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const uint32_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t free_dram_kb = free_dram / 1024;
    ESP_LOGI(TAG, "Chip: ESP32-S3 rev v%u.%u, %d cores",
             (unsigned)(chip_info.revision / 100),
             (unsigned)(chip_info.revision % 100),
             chip_info.cores);
    ESP_LOGI(TAG, "Free DRAM: %lu bytes (%lu KB)",
             (unsigned long)free_dram, (unsigned long)free_dram_kb);

    // SD mount attempt — non-fatal in Phase 1.
    const esp_err_t sd_err = mount_sd_spi();
    const bool sd_ok = (sd_err == ESP_OK) && g_sd_mounted;
    ESP_LOGI(TAG, "SD status: %s", sd_ok ? "PRESENT" : "NOT FOUND");

    // Load persistent config (first-boot writes defaults).
    ESP_ERROR_CHECK_WITHOUT_ABORT(nanojs8::config::load());
    nanojs8::config::log_current();
    if (nanojs8::config::is_default_callsign()) {
        ESP_LOGW(TAG, "Callsign is the default placeholder; operator must set on SETUP screen");
    }

    // Splash for ~1.2 sec.
    render_splash(free_dram_kb, sd_ok,
                  chip_info.revision / 100,
                  chip_info.revision % 100);
    vTaskDelay(pdMS_TO_TICKS(1200));

    // Hand off to the UI task on core 0 (where ESP-IDF's main_task also
    // runs by default; matches Mini-FT8's pinning). Stack 8 KB is
    // comfortable: M5GFX::print() chains use modest stack.
    xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, nullptr, 5, nullptr, 0);

    ESP_LOGI(TAG, "Main task exiting; UI task is running");
    // app_main returns; FreeRTOS keeps ui_task alive.
}
