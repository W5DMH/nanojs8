/*
 * grid_math.h — Maidenhead locator → lat/lon, great-circle distance + bearing
 * ===========================================================================
 * Maidenhead Locator System (used by amateur radio):
 *   - 4 or 6 char alphanumeric: AA NN aa
 *   - First pair (AA, 'A'-'R'): field, 20° lon × 10° lat
 *   - Second pair (NN, '0'-'9'): square, 2° lon × 1° lat
 *   - Third pair (aa, 'a'-'x'): subsquare, 5' lon × 2.5' lat
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a 4- or 6-character grid square (e.g. "EN83" or "EN83IH") into
 * the latitude/longitude of the grid's centre, in decimal degrees.
 *
 * Returns true on success. On failure (bad length or out-of-range chars)
 * returns false and leaves *lat / *lon untouched.
 *
 * Case-insensitive on the letter pairs. Whitespace is not stripped —
 * caller is responsible for handing in a clean grid string.
 */
bool nanojs8_grid_to_latlon(const char *grid, double *lat, double *lon);

/**
 * Compute the great-circle distance (in statute miles) and initial
 * compass bearing (0-359 degrees, 0 = true north) from `grid_from` to
 * `grid_to`.
 *
 * Either grid may be 4 or 6 chars. Returns true on success; on failure
 * (either grid invalid) returns false and *out_miles / *out_bearing
 * are not modified.
 */
bool nanojs8_grid_distance_bearing(const char *grid_from,
                                    const char *grid_to,
                                    double  *out_miles,
                                    int     *out_bearing);

#ifdef __cplusplus
}
#endif
