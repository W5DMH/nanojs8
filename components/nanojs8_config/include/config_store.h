// NanoJS8 — Config store
//
// Thin, defensive wrapper around ESP-IDF's NVS API. Public surface is
// deliberately small: load once on boot, mutate the in-memory copy via
// setters, save() flushes back to flash.
//
// Threading: the in-memory Config is NOT thread-safe. Phase 1 only
// touches it from the UI task; later phases will need a mutex when
// adding a config-edit path from BLE / serial console.
//
// Error policy: every function returns esp_err_t. Callers (mainly the
// screen router) decide whether a failure is fatal or shows a banner.
// Logging is done here at WARN/ERROR level so the operator always sees
// failures in the serial monitor.

#pragma once

#include "esp_err.h"
#include "config_schema.h"

namespace nanojs8 {
namespace config {

// Load the persisted config from NVS into the in-memory singleton.
//
// First-boot behavior: if no nanojs8 namespace exists in NVS, the
// in-memory config is populated with defaults from config_schema.h and
// immediately written back. Returns ESP_OK in this case — first boot
// is not an error.
//
// Migration: if the on-disk version differs from NANOJS8_CONFIG_VERSION,
// load() performs the migration (Phase 1 has no migrations to perform,
// so a mismatched version triggers a default-and-rewrite).
//
// Must be called exactly once before any get_*() / set_*() / save().
esp_err_t load();

// Flush the in-memory config to NVS. Idempotent — safe to call when
// no fields have changed (no NVS write actually happens if values
// match what's already stored, by virtue of NVS's own deduplication).
esp_err_t save();

// Read-only access to the in-memory Config. Returns a const reference
// rather than a copy — the SETUP screen and future readers iterate
// over fields frequently and we don't want to encourage temporaries.
const Config& current();

// Setters. Each validates the input and returns ESP_OK on success, or
// ESP_ERR_INVALID_ARG on bad input WITHOUT mutating the field. The
// SETUP screen uses the return value to gate Tab-out and surface a
// brief inline error.
//
// Validation rules (Phase 1):
//   - callsign: 3..15 chars, ASCII alphanumeric plus '/' allowed.
//   - grid:     exactly 4 or 6 chars, Maidenhead format
//               (A-R, A-R, 0-9, 0-9, [a-x, a-x]).
//   - radio:    must be one of NANOJS8_RADIO_PROFILES[].
//
// All setters NUL-terminate; the caller's string need not be.
esp_err_t set_callsign(const char* value);
esp_err_t set_grid    (const char* value);
esp_err_t set_radio   (const char* value);

// Validation helpers — exposed so the SETUP screen can re-check a draft
// value at character-insertion time (live red-highlight as the user
// types) without committing to current().
bool valid_callsign(const char* value);
bool valid_grid    (const char* value);
bool valid_radio   (const char* value);

// Convenience: dump the current config to ESP_LOG at INFO level. Used
// by main() at boot and (later) by the DOCTOR screen.
void log_current();

// Convenience: is the current callsign the first-boot placeholder?
// Later phases use this to gate TX and show a "Set your callsign"
// banner.
bool is_default_callsign();

} // namespace config
} // namespace nanojs8
