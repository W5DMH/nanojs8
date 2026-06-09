/*
 * config.h — NanoJS8 v0.7 persistent configuration (Layer 6b.1)
 * ==============================================================
 * NVS-backed storage for operator configuration: callsign, grid,
 * groups, units, frequency, radio profile. Modeled after MicroJS8's
 * Config dataclass and saved-as-TOML scheme — same field semantics,
 * same defaults, but persisted to ESP-IDF's NVS partition instead.
 *
 * Storage layout:
 *   NVS namespace "nanojs8" with one key per field. Each field is a
 *   string EXCEPT freq_hz which is a uint64. Keys are kept short (≤15
 *   chars) per NVS conventions.
 *
 * Concurrency model:
 *   Single-writer: only the UI's commit_edit() path writes. Readers
 *   (UI render, radio component) read via nanojs8_config_get() which
 *   returns a pointer to the in-memory cache. The cache is filled at
 *   init() time and updated on every successful set() + save().
 *
 *   The cached struct is NOT re-read from NVS after init, so any
 *   external NVS edits won't be visible until reboot. Acceptable for
 *   an appliance — config changes go through the Setup screen.
 *
 * Defaults & first-boot detection:
 *   nanojs8_config_is_configured() returns true once the operator
 *   has set callsign != "N0CALL" AND grid is non-empty. The UI uses
 *   this to decide whether to auto-enter the Setup screen at boot.
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

// Sizes chosen to match MicroJS8's validation limits with a small
// margin for NUL terminator.
//   * Callsign: 3..10 chars per ham regex. 12 bytes (10 + slash + NUL).
//   * Grid: 4 or 6 chars (Maidenhead). 8 bytes for safety.
//   * Groups: comma-separated list with @ prefix. 64 bytes covers
//     ~8 typical groups.
//   * Units: "miles" or "km". 8 bytes.
//   * Radio id: e.g. "digirig-rts-only". 24 bytes.
#define NANOJS8_CONFIG_CALLSIGN_LEN  12
#define NANOJS8_CONFIG_GRID_LEN      8
#define NANOJS8_CONFIG_GROUPS_LEN    64
#define NANOJS8_CONFIG_UNITS_LEN     8
#define NANOJS8_CONFIG_RADIO_ID_LEN  24

// Sentinel values for "unconfigured". Match MicroJS8 for compatibility
// with operators who read the on-screen UI and expect identical text.
#define NANOJS8_DEFAULT_CALLSIGN   "N0CALL"
#define NANOJS8_DEFAULT_GRID       ""
#define NANOJS8_DEFAULT_GROUPS     ""
#define NANOJS8_DEFAULT_UNITS      "miles"

// Default frequency: JS8 standard heartbeat on 40m. Same as MicroJS8.
#define NANOJS8_DEFAULT_FREQ_HZ    7078000ULL

// Default radio profile: the safest one (no CAT). Operator picks a
// CAT-capable profile in Setup once they know which radio they have.
#define NANOJS8_DEFAULT_RADIO_ID   "digirig-rts-only"

// Configuration record. Held in RAM after nanojs8_config_init().
typedef struct {
    char     callsign[NANOJS8_CONFIG_CALLSIGN_LEN];   // e.g. "W5DMH"
    char     grid[NANOJS8_CONFIG_GRID_LEN];           // e.g. "EM89"
    char     groups[NANOJS8_CONFIG_GROUPS_LEN];       // "@CQ,@DX"
    char     units[NANOJS8_CONFIG_UNITS_LEN];         // "miles"|"km"
    uint64_t freq_hz;                                  // 7078000
    char     radio_id[NANOJS8_CONFIG_RADIO_ID_LEN];   // "digirig-rts-only"
} nanojs8_config_t;

// Initialize NVS and load any saved config. On first boot (no values
// stored yet), the defaults above are used. Idempotent — safe to call
// multiple times; subsequent calls are no-ops.
//
// Returns ESP_OK on success. On NVS init failure (corrupt partition,
// out of space), logs the error and returns the underlying esp_err.
// In that case the in-memory cache still has defaults, so the rest
// of the firmware continues to run with a sensible config.
esp_err_t nanojs8_config_init(void);

// Returns a pointer to the in-memory config cache. The pointer is
// stable across the firmware's lifetime — same address every call.
// Thread-safe to read (the struct is updated atomically via set()).
//
// Do NOT modify the struct through this pointer. Use nanojs8_config_set()
// to make changes.
const nanojs8_config_t* nanojs8_config_get(void);

// Replace the in-memory cache with the contents of `new_cfg`. Does
// NOT persist to NVS — call nanojs8_config_save() to do that.
//
// Returns ESP_ERR_INVALID_ARG if new_cfg is NULL.
esp_err_t nanojs8_config_set(const nanojs8_config_t* new_cfg);

// Persist the current in-memory cache to NVS. Returns ESP_OK on
// success or the underlying nvs error. Safe to call repeatedly; NVS
// handles the wear-levelling internally.
esp_err_t nanojs8_config_save(void);

// Convenience: returns true if both callsign != "N0CALL" AND grid is
// non-empty. The Setup screen auto-entry check at boot uses this.
//
// The threshold mirrors MicroJS8's tx_allowed predicate — without
// callsign + grid we can't operate, so the UI forces the operator
// through Setup before showing anything else.
bool nanojs8_config_is_configured(void);

// L7.11f: parse the comma-separated groups list (e.g. "@CQ,@DX") into
// individual entries. Used by the COMPOSE screen's HEARD/groups picker.
//
// out_groups must point to an array of `max` char buffers, each at
// least `entry_len` bytes long. Trailing/leading whitespace is
// trimmed from each entry. Empty entries (e.g. trailing comma) are
// skipped.
//
// Returns the number of groups copied. May be less than the total
// number of groups in config.groups if `max` was reached. Returns 0
// if config.groups is empty or NULL pointers given.
//
// Group entries are NOT validated for the leading '@' — the operator
// can configure whatever they like in SETUP. The compose UI will
// distinguish them visually but the wire format is whatever the
// operator typed.
uint32_t nanojs8_config_groups_enumerate(char (*out_groups)[NANOJS8_CONFIG_GROUPS_LEN],
                                          uint32_t max);

#ifdef __cplusplus
}
#endif
