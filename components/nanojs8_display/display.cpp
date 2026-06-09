/*
 * display.cpp — NanoJS8 v0.7 display abstraction implementation
 * ===============================================================
 * See display.h for the public API and rationale.
 *
 * Architecture:
 *   - SPI bus on SPI2_HOST, shared eventually with SD card (not LoRa —
 *     Layer 2 keeps LoRa held in reset until we explicitly use it)
 *   - Panel IO via esp_lcd_new_panel_io_spi (DC=GPIO 11, CS=GPIO 12)
 *   - Panel driver via esp_lcd_new_panel_st7789 (no hardware reset pin)
 *   - LilyGO-specific quirk: invert_color(true) — without this, all
 *     colors are inverted (red shows as cyan, etc.)
 *   - Default orientation: landscape via swap_xy + mirror
 *
 * Text drawing strategy:
 *   For each glyph, we allocate an 8×16 RGB565 buffer on the stack,
 *   fill it from the bitmap font, and blit it via
 *   esp_lcd_panel_draw_bitmap. Glyphs are tiny (256 bytes each) so
 *   stack allocation is fine. For multi-character strings we still
 *   blit one character at a time — slower than a single batched
 *   transfer but vastly simpler, and Layer 3's banner doesn't need
 *   raw speed.
 *
 * License: GPL-3.0
 */

#include "display.h"
#include "platform_tdeck.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static const char* TAG = "display";

// Pull in the font data table (defined in font_8x16.c)
extern "C" const uint8_t nanojs8_font_8x16[95][16];

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

// Panel handle — opaque pointer kept across the lifetime of the app
esp_lcd_panel_handle_t s_panel = nullptr;
esp_lcd_panel_io_handle_t s_panel_io = nullptr;

// Font metrics — constants for this single embedded font
constexpr int FONT_WIDTH    = 8;
constexpr int FONT_HEIGHT   = 16;
constexpr int FONT_ADVANCE  = 8;   // monospaced, no inter-character gap
constexpr int FONT_FIRST    = 0x20;
constexpr int FONT_LAST     = 0x7E;

// Display dimensions after our default landscape rotation
constexpr int DISP_W = NANOJS8_DISPLAY_WIDTH;   // 320
constexpr int DISP_H = NANOJS8_DISPLAY_HEIGHT;  // 240

// SPI parameters (verified from LovyanGFX T-Deck config — same hardware)
constexpr spi_host_device_t SPI_HOST_USED = SPI2_HOST;
constexpr int SPI_CLOCK_HZ_WRITE = 40 * 1000 * 1000;   // 40 MHz

// Maximum SPI transfer size. We size for one full screen worth of pixels
// (320 * 240 * 2 = 153,600 bytes). The driver may allocate this from
// internal DMA-capable RAM, which is precious — so cap at the largest
// transfer we'll actually need for now. Font glyphs are tiny (256 bytes)
// and rect fills can be chunked. Use one row of the screen (320 * 2 = 640
// bytes) plus a fudge factor.
constexpr int SPI_MAX_TRANSFER_BYTES = 4096;

// Convert RGB565 from host-native (little-endian on ESP32-S3) to the
// big-endian word order the ST7789 expects.
static inline uint16_t rgb565_swap(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_display_init(void) {
    using namespace nanojs8::platform::tdeck;
    esp_err_t err;

    ESP_LOGI(TAG, "Display init starting...");

    // ---- Step 1: initialize the SPI bus ----
    // Per ESP-IDF docs, we initialize the bus before allocating the panel
    // IO device. Pins come from the platform_tdeck namespace.
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = PIN_SPI_MOSI;        // GPIO 41
    bus_cfg.miso_io_num = PIN_SPI_MISO;        // GPIO 38
    bus_cfg.sclk_io_num = PIN_SPI_SCK;         // GPIO 40
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = SPI_MAX_TRANSFER_BYTES;
    bus_cfg.flags = 0;

    err = spi_bus_initialize(SPI_HOST_USED, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  SPI2_HOST initialized (MOSI=%d MISO=%d SCK=%d, %d Hz)",
             PIN_SPI_MOSI, PIN_SPI_MISO, PIN_SPI_SCK, SPI_CLOCK_HZ_WRITE);

    // ---- Step 2: create the panel IO device ----
    // SPI mode 0, 40 MHz write clock, DC on GPIO 11, CS on GPIO 12.
    // lcd_cmd_bits/lcd_param_bits = 8 are ST7789 defaults.
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_TFT_CS;           // GPIO 12
    io_cfg.dc_gpio_num = PIN_TFT_DC;           // GPIO 11
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = SPI_CLOCK_HZ_WRITE;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.on_color_trans_done = nullptr;
    io_cfg.user_ctx = nullptr;
    io_cfg.flags.dc_low_on_data = 0;
    io_cfg.flags.octal_mode = 0;
    io_cfg.flags.sio_mode = 0;
    io_cfg.flags.lsb_first = 0;
    io_cfg.flags.cs_high_active = 0;

    err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI_HOST_USED, &io_cfg, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  Panel IO created (CS=%d DC=%d)", PIN_TFT_CS, PIN_TFT_DC);

    // ---- Step 3: create the ST7789 panel driver ----
    // No hardware reset pin (T-Deck doesn't wire one to the S3); the
    // panel handles reset via software command.
    //
    // rgb_ele_order: ST7789 hardware default is BGR, but combined with
    // our pre-byte-swapped RGB565 color values, BGR produces a net
    // red↔blue swap. After empirical testing on the T-Deck panel, RGB
    // ordering gives correct colors (cyan looks cyan, yellow looks
    // yellow, red looks red).
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = -1;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.flags.reset_active_high = 0;
    panel_cfg.vendor_config = nullptr;

    err = esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "  ST7789 panel driver attached");

    // ---- Step 4: bring the panel up ----
    // Reset, init, set inversion, set orientation, turn the display on.
    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // CRITICAL: LilyGO T-Deck quirk. Without inversion, all colors render
    // inverted (white→black, red→cyan, etc). This is a hardware/panel
    // wiring choice baked into the T-Deck PCB, NOT a software bug.
    err = esp_lcd_panel_invert_color(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "invert_color failed: %s", esp_err_to_name(err));
        return err;
    }

    // Landscape orientation: 320 wide × 240 tall.
    // ST7789 native is 240x320 portrait; swap_xy makes it landscape.
    // Empirical: T-Deck physical mounting needs mirror(true, false) to
    // get the top of the screen at the top edge (right-side-up text).
    // Previously tried mirror(false, true) — that gave upside-down text.
    err = esp_lcd_panel_swap_xy(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "swap_xy failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_mirror(s_panel, true, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mirror failed: %s", esp_err_to_name(err));
        return err;
    }

    // Turn the display on (some panels default to off after init)
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disp_on_off failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "  Panel initialized (320x240 landscape, invert=true)");

    // Clear screen to black so callers start from a known state
    err = nanojs8_display_clear(NANOJS8_COLOR_BLACK);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial clear failed: %s (continuing)",
                 esp_err_to_name(err));
        // Non-fatal — display is alive even if clear didn't work
    }

    ESP_LOGI(TAG, "Display init OK");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_display_fill_rect(int x, int y, int w, int h,
                                                uint16_t color) {
    if (!s_panel) return ESP_ERR_INVALID_STATE;

    // Clip to screen bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= DISP_W || y >= DISP_H) return ESP_OK;     // entirely off-screen
    if (x + w > DISP_W) w = DISP_W - x;
    if (y + h > DISP_H) h = DISP_H - y;
    if (w <= 0 || h <= 0) return ESP_OK;

    // Fill row-by-row to keep buffer small. One row = 320 pixels * 2 bytes
    // = 640 bytes max; well within SPI_MAX_TRANSFER_BYTES.
    // For efficiency we batch up to FILL_ROWS rows per blit when possible.
    constexpr int FILL_ROWS = 4;  // 320 * 2 * 4 = 2560 bytes per chunk
    const uint16_t color_be = rgb565_swap(color);

    static uint16_t row_buf[DISP_W * FILL_ROWS];
    // Fill the buffer with the color (only need to fill once since color
    // doesn't change between rows). Use stdlib for speed.
    for (int i = 0; i < DISP_W * FILL_ROWS; ++i) {
        row_buf[i] = color_be;
    }

    int rows_remaining = h;
    int cur_y = y;
    while (rows_remaining > 0) {
        int chunk = rows_remaining > FILL_ROWS ? FILL_ROWS : rows_remaining;
        // draw_bitmap signature: (panel, x_start, y_start, x_end, y_end, data)
        // Coordinates exclusive on x_end/y_end per ESP-IDF convention.
        esp_err_t err = esp_lcd_panel_draw_bitmap(
            s_panel, x, cur_y, x + w, cur_y + chunk, row_buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fill_rect draw_bitmap failed at y=%d: %s",
                     cur_y, esp_err_to_name(err));
            return err;
        }
        cur_y += chunk;
        rows_remaining -= chunk;
    }
    return ESP_OK;
}

extern "C" esp_err_t nanojs8_display_clear(uint16_t color) {
    return nanojs8_display_fill_rect(0, 0, DISP_W, DISP_H, color);
}

extern "C" int nanojs8_display_draw_char(int x, int y, char ch,
                                          uint16_t fg, uint16_t bg) {
    if (!s_panel) return 0;
    if (x >= DISP_W || y >= DISP_H) return FONT_ADVANCE;  // off-screen, skip
    if (x + FONT_WIDTH <= 0 || y + FONT_HEIGHT <= 0) return FONT_ADVANCE;

    // Look up glyph data. Characters outside the printable range render
    // as a small filled square so the operator notices.
    const uint8_t* glyph;
    static const uint8_t fallback_glyph[16] = {
        0x00, 0x00, 0x00, 0x00, 0x7E, 0x7E, 0x7E, 0x7E,
        0x7E, 0x7E, 0x7E, 0x7E, 0x00, 0x00, 0x00, 0x00,
    };
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST) {
        glyph = fallback_glyph;
    } else {
        glyph = nanojs8_font_8x16[(unsigned char)ch - FONT_FIRST];
    }

    // Build an 8×16 RGB565 bitmap of this glyph. Bytes are pre-swapped
    // to ST7789's expected order.
    uint16_t glyph_buf[FONT_WIDTH * FONT_HEIGHT];
    const uint16_t fg_be = rgb565_swap(fg);
    const uint16_t bg_be = rgb565_swap(bg);

    for (int row = 0; row < FONT_HEIGHT; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; ++col) {
            // bit 7 = leftmost pixel
            bool lit = (bits & (0x80 >> col)) != 0;
            glyph_buf[row * FONT_WIDTH + col] = lit ? fg_be : bg_be;
        }
    }

    // Clip if partially off-screen on right/bottom. Use the bitmap's
    // natural size and let esp_lcd_panel_draw_bitmap handle the rest, but
    // clamp x_end / y_end to screen bounds for safety.
    int x_end = x + FONT_WIDTH;
    int y_end = y + FONT_HEIGHT;
    if (x_end > DISP_W) x_end = DISP_W;
    if (y_end > DISP_H) y_end = DISP_H;

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel, x, y, x_end, y_end, glyph_buf);
    if (err != ESP_OK) {
        // Don't log per-character; this would flood the log on text-heavy
        // screens. The caller can detect bad state via a failed init.
        return FONT_ADVANCE;
    }
    return FONT_ADVANCE;
}

extern "C" int nanojs8_display_draw_text(int x, int y, const char* str,
                                          uint16_t fg, uint16_t bg) {
    if (!str) return 0;
    int cur_x = x;
    while (*str) {
        if (cur_x >= DISP_W) break;  // past right edge, stop early
        cur_x += nanojs8_display_draw_char(cur_x, y, *str++, fg, bg);
    }
    return cur_x - x;
}

extern "C" int nanojs8_display_text_width(const char* str) {
    if (!str) return 0;
    int count = 0;
    while (*str++) ++count;
    return count * FONT_ADVANCE;
}

extern "C" int nanojs8_display_font_height(void) { return FONT_HEIGHT; }
extern "C" int nanojs8_display_font_advance(void) { return FONT_ADVANCE; }
