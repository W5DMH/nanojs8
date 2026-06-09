/*
 * display.h — NanoJS8 v0.7 display abstraction (Layer 3)
 * ========================================================
 * Wraps ESP-IDF's esp_lcd_panel_st7789 driver to drive the LilyGO T-Deck's
 * 320x240 ST7789 IPS panel. Provides a minimal text-and-pixel API suitable
 * for Layer 4+ UI work.
 *
 * Why esp_lcd (Espressif native) instead of LovyanGFX:
 *   - Zero third-party dependency / compile risk on ESP-IDF v5.5.4
 *   - Well-maintained, used by Espressif's own examples
 *   - Sufficient for NanoJS8's UI needs (status bar, lists, simple lines)
 *   - If we later need sprites or advanced effects for a specific screen,
 *     LovyanGFX can be added then, with focused scope
 *
 * Hardware:
 *   - ST7789 panel, 320x240, 16bpp RGB565
 *   - SPI2_HOST, 40 MHz write clock, mode 0
 *   - Color invert: TRUE (LilyGO-specific quirk)
 *   - Rotation: landscape (320 wide × 240 tall)
 *   - Pins: GPIO 41 MOSI, 38 MISO, 40 SCK, 12 CS, 11 DC, no hardware reset
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Display dimensions after our default rotation. Constant so callers can
// lay out without querying the panel at runtime.
#define NANOJS8_DISPLAY_WIDTH   320
#define NANOJS8_DISPLAY_HEIGHT  240

// 16-bit RGB565 color constants. Bytes are swapped at draw time because
// ST7789 expects big-endian color words on the SPI bus.
#define NANOJS8_COLOR_BLACK     0x0000
#define NANOJS8_COLOR_WHITE     0xFFFF
#define NANOJS8_COLOR_RED       0xF800
#define NANOJS8_COLOR_GREEN     0x07E0
#define NANOJS8_COLOR_BLUE      0x001F
#define NANOJS8_COLOR_YELLOW    0xFFE0
#define NANOJS8_COLOR_CYAN      0x07FF
#define NANOJS8_COLOR_MAGENTA   0xF81F
#define NANOJS8_COLOR_DARK_GRAY 0x4208   // RGB565 ~25% gray
#define NANOJS8_COLOR_GRAY      0x8410   // RGB565 ~50% gray

// Initialize the display: SPI bus, panel IO, ST7789 driver, panel reset
// and init sequence (including LilyGO's invert-color quirk), default
// rotation to landscape, screen cleared to black.
//
// REQUIRES that the platform layer (Layer 2) has already initialized —
// specifically POWERON pin HIGH and backlight GPIO configured. Without
// that, the panel won't respond.
//
// Returns ESP_OK on success; logs the failure point and returns the
// underlying error otherwise.
esp_err_t nanojs8_display_init(void);

// Fill the entire screen with a single 16-bit color.
esp_err_t nanojs8_display_clear(uint16_t color);

// Fill a rectangular area with a single color. Coordinates are clipped
// to the screen bounds; out-of-bounds rects are silently truncated.
// (x_end, y_end) are EXCLUSIVE per esp_lcd_panel_draw_bitmap convention.
esp_err_t nanojs8_display_fill_rect(int x, int y, int w, int h,
                                     uint16_t color);

// Draw a single ASCII character at pixel (x, y) using our embedded 8x16
// bitmap font. Returns the X pixel advance (always 8 for this font).
// Characters outside the printable ASCII range (< 0x20 or > 0x7E) render
// as a small filled block.
int nanojs8_display_draw_char(int x, int y, char ch,
                               uint16_t fg, uint16_t bg);

// Draw a null-terminated ASCII string at pixel (x, y). Returns the
// total width drawn in pixels. Does no line-wrapping; characters
// past the right edge are clipped.
int nanojs8_display_draw_text(int x, int y, const char* str,
                               uint16_t fg, uint16_t bg);

// Convenience: return text width in pixels for a string in the current
// font. Useful for centering.
int nanojs8_display_text_width(const char* str);

// Font metrics, in case callers want them. Constants for now (one font),
// but exposed as functions to leave room for multi-font support later.
int nanojs8_display_font_height(void);
int nanojs8_display_font_advance(void);

#ifdef __cplusplus
}
#endif
