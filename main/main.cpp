// NanoJS8 — main.cpp (Phase 3a)
//
// Boot sequence:
//   1. NVS init
//   2. M5Cardputer init (display + keyboard enabled)
//   3. Boot diagnostics to ESP_LOG (chip, DRAM, SD)
//   4. Mount SD (informational in Phase 3a; required from Phase 4)
//   5. Splash screen for ~1.2 sec
//   6. Load config from NVS (first-boot writes defaults; v1/v2→v3 migrate)
//   7. Optional radio_service::start() if config.radio_autostart is true
//   8. Register console commands (radio start/stop/status, ptt on/off)
//   9. Hand off to the screen router (pinned to core 0) which renders
//      the active screen and processes keyboard input forever.
//
// Phase 3a USB topology:
//   Console = UART0 on GPIO 1 (TX) / GPIO 2 (RX), accessed via Grove
//             port and an external USB-UART cable. This frees the
//             USB-C port for OTG host duty.
//   USB host = USB-C port driven by the OTG controller; powered hub
//              attached for the DigiRig + radio combination.
//
// References:
//   - Mini-FT8 main.cpp — UART0 console pattern + USB host install order
//   - Build Specification §2.3 — pin map
//   - Build Specification §11 — phase delivery plan

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_console.h"
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
#include "radio_service.h"
#include "power_manager.h"

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

// ---------------------------------------------------------------------------
// Console commands (Phase 3a — radio start/stop/status, ptt on/off)
// ---------------------------------------------------------------------------
//
// The console runs on UART0 (GPIO 1 TX, GPIO 2 RX via Grove). Operators
// connect a USB-UART cable to the Grove port to see logs and issue
// commands while the USB-C port is occupied by the OTG host stack
// talking to the DigiRig.

static int cmd_radio(int argc, char** argv) {
    if (argc < 2) {
        std::printf("Usage: radio start | radio stop | radio status\n");
        return 1;
    }
    if (std::strcmp(argv[1], "start") == 0) {
        const esp_err_t err = nanojs8::radio::start();
        if (err == ESP_OK) {
            std::printf("Radio service starting...\n");
            return 0;
        }
        std::printf("Radio start failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (std::strcmp(argv[1], "stop") == 0) {
        const esp_err_t err = nanojs8::radio::stop();
        std::printf("Radio stop: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (std::strcmp(argv[1], "status") == 0) {
        nanojs8::radio::Snapshot snap;
        nanojs8::radio::snapshot(&snap);
        const char* st = "?";
        switch (snap.status) {
            case nanojs8::radio::Status::IDLE:        st = "IDLE";        break;
            case nanojs8::radio::Status::ENUMERATING: st = "ENUMERATING"; break;
            case nanojs8::radio::Status::CONNECTED:   st = "CONNECTED";   break;
            case nanojs8::radio::Status::ERROR:       st = "ERROR";       break;
        }
        std::printf("status:     %s\n", st);
        std::printf("profile:    %s (%s)\n", snap.profile_id, snap.display_name);
        std::printf("status_txt: %s\n", snap.status_text);
        std::printf("ptt:        %s\n", snap.ptt_active ? "ASSERTED" : "released");
        std::printf("freq:       %lu Hz (supported=%d)\n",
                    (unsigned long)snap.freq_hz, (int)snap.supports_freq);
        std::printf("rx_frames:  %lu  rx_overruns: %lu\n",
                    (unsigned long)snap.rx_frames_total,
                    (unsigned long)snap.rx_overruns);
        {
            const int bat_pct = M5Cardputer.Power.getBatteryLevel();
            const int bat_mv  = M5Cardputer.Power.getBatteryVoltage();
            std::printf("battery:    %d%% (%d mV)%s\n", bat_pct, bat_mv,
                        bat_mv > 0 && bat_mv < 3600 ? "  *** LOW ***" : "");
        }
        return 0;
    }
    std::printf("Unknown subcommand: %s\n", argv[1]);
    return 1;
}

static int cmd_ptt(int argc, char** argv) {
    if (argc < 2) {
        std::printf("Usage: ptt on | ptt off\n");
        return 1;
    }
    if (std::strcmp(argv[1], "on") == 0) {
        const esp_err_t err = nanojs8::radio::ptt_on();
        std::printf("ptt on: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (std::strcmp(argv[1], "off") == 0) {
        const esp_err_t err = nanojs8::radio::ptt_off();
        std::printf("ptt off: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    std::printf("Unknown subcommand: %s\n", argv[1]);
    return 1;
}

// charge [on|off] — enter/exit charge mode. No arg toggles.
static int cmd_charge(int argc, char** argv) {
    if (argc < 2) {
        // Toggle.
        if (nanojs8::power::in_charge_mode()) {
            nanojs8::power::exit_charge_mode();
            std::printf("charge mode: OFF\n");
        } else {
            nanojs8::power::enter_charge_mode();
            std::printf("charge mode: ON (screen off; any key or `charge off` to exit)\n");
        }
        return 0;
    }
    if (std::strcmp(argv[1], "on") == 0) {
        nanojs8::power::enter_charge_mode();
        std::printf("charge mode: ON (screen off; any key or `charge off` to exit)\n");
        return 0;
    }
    if (std::strcmp(argv[1], "off") == 0) {
        nanojs8::power::exit_charge_mode();
        std::printf("charge mode: OFF\n");
        return 0;
    }
    std::printf("Usage: charge | charge on | charge off\n");
    return 1;
}

// power — show/adjust power settings.
static int cmd_power(int argc, char** argv) {
    if (argc < 2) {
        nanojs8::power::Snapshot ps;
        nanojs8::power::snapshot(&ps);
        const nanojs8::power::Settings& s = nanojs8::power::settings();
        const char* lvl = "NORMAL";
        if (ps.level == nanojs8::power::Level::LOW)      lvl = "LOW";
        if (ps.level == nanojs8::power::Level::CRITICAL) lvl = "CRITICAL";
        std::printf("battery:     %d%% (%d mV) [%s]\n", ps.battery_pct, ps.battery_mv, lvl);
        std::printf("charge_mode: %s\n", ps.in_charge_mode ? "ON" : "off");
        std::printf("screen:      %d (0=full 1=dim 2=blank 3=charge)\n", (int)ps.screen_state);
        std::printf("idle:        %us\n", (unsigned)ps.idle_sec);
        std::printf("idle_dim:    %us  idle_off: %us  dim_bright: %u%%\n",
                    (unsigned)s.idle_dim_sec, (unsigned)s.idle_off_sec,
                    (unsigned)s.dim_brightness);
        std::printf("Set: power dim <sec> | power off <sec> | power bright <pct>  (0 disables timeout)\n");
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "dim") == 0) {
        nanojs8::power::set_idle_dim_sec((uint16_t)atoi(argv[2]));
        std::printf("idle_dim set to %s s (saved)\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "off") == 0) {
        nanojs8::power::set_idle_off_sec((uint16_t)atoi(argv[2]));
        std::printf("idle_off set to %s s (saved)\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "bright") == 0) {
        nanojs8::power::set_dim_brightness((uint8_t)atoi(argv[2]));
        std::printf("dim_brightness set to %s%% (saved)\n", argv[2]);
        return 0;
    }
    std::printf("Usage: power | power dim <sec> | power off <sec> | power bright <pct>\n");
    return 1;
}

static void register_console_commands() {
    const esp_console_cmd_t cmd_radio_def = {
        .command  = "radio",
        .help     = "Radio service control: radio start | radio stop | radio status",
        .hint     = nullptr,
        .func     = &cmd_radio,
        .argtable = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_radio_def));

    const esp_console_cmd_t cmd_ptt_def = {
        .command  = "ptt",
        .help     = "PTT control: ptt on | ptt off",
        .hint     = nullptr,
        .func     = &cmd_ptt,
        .argtable = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_ptt_def));

    const esp_console_cmd_t cmd_charge_def = {
        .command  = "charge",
        .help     = "Charge mode (screen off for faster charging): charge | charge on | charge off",
        .hint     = nullptr,
        .func     = &cmd_charge,
        .argtable = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_charge_def));

    const esp_console_cmd_t cmd_power_def = {
        .command  = "power",
        .help     = "Power status/settings: power | power dim <sec> | power off <sec> | power bright <pct>",
        .hint     = nullptr,
        .func     = &cmd_power,
        .argtable = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_power_def));
}

// Console task — runs the REPL in its own task so app_main can return.
// The REPL uses UART0 (the console we configured in sdkconfig).
static void start_console_repl() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt          = "nanojs8> ";
    repl_cfg.max_cmdline_length = 128;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    // The pin / baud values are picked up from sdkconfig
    // (CONFIG_ESP_CONSOLE_UART_TX_GPIO, CONFIG_ESP_CONSOLE_UART_RX_GPIO,
    //  CONFIG_ESP_CONSOLE_UART_BAUDRATE). We don't override them here.

    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart failed: %s", esp_err_to_name(err));
        return;
    }

    register_console_commands();

    esp_console_register_help_command();
    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_start_repl failed: %s", esp_err_to_name(err));
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

    // Battery diagnostics. Critical for Phase 3a power debugging: a plain
    // OTG cable means the DigiRig is powered from this battery, and if it
    // sags under load the DigiRig brown-outs mid-enumeration. Voltage is
    // the useful number — under ~3.6 V the device is nearly empty and
    // can't reliably supply a USB peripheral.
    //
    // Note: Cardputer ADV hardware can't read charge current or charging
    // state (per M5 docs), only level (%) and voltage (mV).
    {
        const int bat_pct = M5Cardputer.Power.getBatteryLevel();
        const int bat_mv  = M5Cardputer.Power.getBatteryVoltage();
        ESP_LOGI(TAG, "Battery: %d%% (%d mV)%s", bat_pct, bat_mv,
                 bat_mv > 0 && bat_mv < 3600
                   ? "  *** LOW — USB host may brown out ***" : "");
    }

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

    // Phase 3.5: initialize the power management subsystem. Must come
    // after M5Cardputer.begin() (needs display + battery drivers) and
    // after config load (reads idle/dim settings from NVS). Starts the
    // battery monitor task and idle-screen management.
    nanojs8::power::init();

    // Phase 3a: optionally auto-start the radio service per NVS flag.
    // Defaults OFF so a fresh boot behaves like Phase 0/1/2.
    if (nanojs8::config::current().radio_autostart) {
        ESP_LOGI(TAG, "radio_autostart=on; starting radio service");
        const esp_err_t rerr = nanojs8::radio::start();
        if (rerr != ESP_OK) {
            ESP_LOGW(TAG, "radio_service::start at boot returned: %s "
                          "(operator can retry with `radio start`)",
                     esp_err_to_name(rerr));
        }
    } else {
        ESP_LOGI(TAG, "radio_autostart=off; use `radio start` to enable");
    }

    // Start the serial console REPL. Lives on UART0 / Grove. Operators
    // connect a USB-UART cable to issue `radio start`, `ptt on`, etc.
    start_console_repl();

    // Hand off to the UI task on core 0 (where ESP-IDF's main_task also
    // runs by default; matches Mini-FT8's pinning). Stack 8 KB is
    // comfortable: M5GFX::print() chains use modest stack.
    xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, nullptr, 5, nullptr, 0);

    ESP_LOGI(TAG, "Main task exiting; UI task is running");
    // app_main returns; FreeRTOS keeps ui_task alive.
}
