/*
 * jsc_map.h — memory-mapped JSC dictionary loader (L7.4a)
 * ==========================================================
 * The JS8 source codec needs a 262,144-entry word dictionary
 * (JSC::map). Including it inline would consume ~3 MB of the app
 * partition. Instead we place it in a dedicated `jsc_map` partition
 * (6 MB) and memory-map it at boot via esp_partition_mmap.
 *
 * Provisioning: the `jsc_map.bin` is built at compile time from
 * the upstream JSC_map.cpp source by tools/pack_jsc_map.py, and
 * flashed into the partition by esptool (CMake handles both).
 *
 * Runtime use:
 *   ESP_ERROR_CHECK(nanojs8_jsc_map_init());
 *   const char *word = nanojs8_jsc_map_word(idx);  // O(1), points into mmap region
 *
 * The returned pointers are stable for the lifetime of the
 * application. The MMU keeps the 6 MB partition mapped in the
 * data cache region; reads are at near-RAM speed.
 *
 * Memory cost: 0 bytes of RAM. The dictionary lives in flash,
 * accessed via the ESP32-S3's instruction/data cache.
 *
 * License: GPL-3.0 (NanoJS8). The dictionary data itself is
 * derived from JS8Call-improved (also GPL-3.0).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the JSC map loader.
 *
 * Looks up the `jsc_map` partition, validates the header (magic and
 * version), and memory-maps the entire partition into the data
 * address space. Idempotent — safe to call multiple times; subsequent
 * calls are no-ops.
 *
 * Returns:
 *   ESP_OK                    success
 *   ESP_ERR_NOT_FOUND         no `jsc_map` partition (partitions.csv issue)
 *   ESP_ERR_INVALID_VERSION   wrong magic or version in the partition data
 *                              (typically means jsc_map.bin wasn't flashed)
 *   ESP_ERR_NO_MEM            mmap failed
 *   other esp_err_t           esp_partition_* error pass-through
 */
esp_err_t nanojs8_jsc_map_init(void);

/**
 * Number of entries in the loaded map. Returns 0 before init.
 */
uint32_t nanojs8_jsc_map_count(void);

/**
 * Total size of the string pool in bytes (sum of all string lengths +
 * one NUL per string). Returns 0 before init.
 */
uint32_t nanojs8_jsc_map_pool_size(void);

/**
 * Look up a dictionary word by its array position.
 *
 * Returns a NUL-terminated string pointer into the memory-mapped
 * region. Valid until shutdown; do NOT free or modify.
 *
 * Returns NULL if:
 *   - init has not been called successfully
 *   - idx >= entry_count
 *
 * O(1) — two cached flash reads (offset table + pool byte).
 */
const char *nanojs8_jsc_map_word(uint32_t idx);

#ifdef __cplusplus
}
#endif
