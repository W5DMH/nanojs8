/*
 * grid_math.c — Maidenhead locator parsing + great-circle math
 * ============================================================
 * License: GPL-3.0
 *
 * Reference: https://en.wikipedia.org/wiki/Maidenhead_Locator_System
 * Cross-checked: EN83IH → ~ 43.29°N, 83.34°W (Detroit, MI metro area).
 */

#include "grid_math.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

// ── Maidenhead → lat/lon ─────────────────────────────────────────────

static inline int field_index(char c)
{
    // 'A'..'R' = 0..17
    c = (char)toupper((unsigned char)c);
    if (c < 'A' || c > 'R') return -1;
    return c - 'A';
}

static inline int digit_index(char c)
{
    if (c < '0' || c > '9') return -1;
    return c - '0';
}

static inline int subsquare_index(char c)
{
    // 'a'..'x' = 0..23 (case-insensitive per ham convention)
    c = (char)tolower((unsigned char)c);
    if (c < 'a' || c > 'x') return -1;
    return c - 'a';
}

bool nanojs8_grid_to_latlon(const char *grid, double *lat, double *lon)
{
    if (!grid || !lat || !lon) return false;

    const size_t len = strlen(grid);
    if (len != 4 && len != 6) return false;

    const int F_lon = field_index(grid[0]);   // 0..17 (each = 20° lon)
    const int F_lat = field_index(grid[1]);   // 0..17 (each = 10° lat)
    const int S_lon = digit_index(grid[2]);   // 0..9  (each = 2°  lon)
    const int S_lat = digit_index(grid[3]);   // 0..9  (each = 1°  lat)
    if (F_lon < 0 || F_lat < 0 || S_lon < 0 || S_lat < 0) return false;

    // SW corner of the *square* (4-char level).
    double LonSW = -180.0 + (double)F_lon * 20.0 + (double)S_lon * 2.0;
    double LatSW =  -90.0 + (double)F_lat * 10.0 + (double)S_lat * 1.0;

    // Default to centre of the 4-char square.
    double sub_lon_w = 2.0;   // width  in deg of the square in lon
    double sub_lat_h = 1.0;   // height in deg
    double lon_off   = sub_lon_w * 0.5;
    double lat_off   = sub_lat_h * 0.5;

    if (len == 6) {
        const int SS_lon = subsquare_index(grid[4]);  // 0..23 (each = 5'   lon = 1/12°)
        const int SS_lat = subsquare_index(grid[5]);  // 0..23 (each = 2.5' lat = 1/24°)
        if (SS_lon < 0 || SS_lat < 0) return false;

        // SW corner of the *subsquare*.
        LonSW += (double)SS_lon * (1.0 / 12.0);
        LatSW += (double)SS_lat * (1.0 / 24.0);

        // Centre of the subsquare.
        lon_off = (1.0 / 12.0) * 0.5;   // half of 5' in degrees
        lat_off = (1.0 / 24.0) * 0.5;   // half of 2.5' in degrees
    }

    *lon = LonSW + lon_off;
    *lat = LatSW + lat_off;
    return true;
}

// ── Great-circle distance + initial bearing ──────────────────────────
//
// Earth mean radius: 6371.0088 km = 3958.7613 statute miles. Using the
// statute mile value directly because our display column is "MI". A
// configurable miles/km switch could come later but the user's config
// already has units=miles.

static const double EARTH_RADIUS_MI = 3958.7613;

static inline double deg2rad(double d) { return d * (M_PI / 180.0); }
static inline double rad2deg(double r) { return r * (180.0 / M_PI); }

bool nanojs8_grid_distance_bearing(const char *grid_from,
                                    const char *grid_to,
                                    double  *out_miles,
                                    int     *out_bearing)
{
    double lat1d, lon1d, lat2d, lon2d;
    if (!nanojs8_grid_to_latlon(grid_from, &lat1d, &lon1d)) return false;
    if (!nanojs8_grid_to_latlon(grid_to,   &lat2d, &lon2d)) return false;

    const double lat1 = deg2rad(lat1d);
    const double lat2 = deg2rad(lat2d);
    const double dlat = deg2rad(lat2d - lat1d);
    const double dlon = deg2rad(lon2d - lon1d);

    // Haversine
    const double sdl = sin(dlat * 0.5);
    const double sdo = sin(dlon * 0.5);
    const double a   = sdl * sdl + cos(lat1) * cos(lat2) * sdo * sdo;
    const double c   = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    const double miles = EARTH_RADIUS_MI * c;

    // Initial bearing (forward azimuth) along the great circle.
    const double y = sin(dlon) * cos(lat2);
    const double x = cos(lat1) * sin(lat2) -
                     sin(lat1) * cos(lat2) * cos(dlon);
    double brg_deg = rad2deg(atan2(y, x));
    if (brg_deg < 0.0) brg_deg += 360.0;
    if (brg_deg >= 360.0) brg_deg -= 360.0;

    if (out_miles)   *out_miles   = miles;
    if (out_bearing) *out_bearing = (int)(brg_deg + 0.5);  // round
    return true;
}
