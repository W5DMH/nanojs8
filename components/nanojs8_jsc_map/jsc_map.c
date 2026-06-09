/*
 * jsc_map.c — memory-mapped JSC dictionary loader
 * ====================================================
 * See jsc_map.h for the public API and architecture overview.
 *
 * Implementation summary:
 *   1. Find the `jsc_map` partition by label (set in partitions.csv).
 *   2. Memory-map the whole 6 MB partition via esp_partition_mmap.
 *      The ESP32-S3 MMU maps the flash region into the data cache
 *      virtual address range. Reads are then ordinary loads.
 *   3. Validate the header (magic + version + reasonable counts).
 *   4. Resolve internal pointers:
 *        - offsets[] starts immediately after the header
 *        - pool starts after the offsets[] array
 *   5. Expose nanojs8_jsc_map_word(idx) which does pool[offsets[idx]]
 *      with bounds checking.
 *
 * Threading: init must be called once before any other function. The
 * accessor is reentrant (read-only, no synchronization needed).
 *
 * Error policy: failures during init are non-fatal to the app —
 * we log the failure and the accessor returns NULL on every call.
 * Callers (gfsk8 modem) must handle NULL gracefully or fail their
 * own init.
 */

#include "jsc_map.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "jsc_map";

// ── Binary format constants (must match tools/pack_jsc_map.py) ────────────
//
// Header layout (16 bytes, little-endian, packed):
//   uint32 magic
//   uint32 version
//   uint32 entry_count
//   uint32 string_pool_size
//
// Then: uint32 offsets[entry_count]
// Then: char pool[string_pool_size]
//
// We use a packed struct view onto the mmap region (no copies).

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t pool_size;
} jsc_header_t;

#define JSC_MAP_MAGIC     0x4D43534Au  /* 'JSCM' little-endian */
#define JSC_MAP_VERSION   1u

// Reasonableness limit — refuses to mmap something that claims to be
// a huge file. Our packed file is < 3 MB; we set the cap at 6 MB
// (= partition size) which is the largest legitimate value.
#define JSC_MAP_MAX_TOTAL (6u * 1024u * 1024u)

// ── Runtime state ──────────────────────────────────────────────────────────

static const jsc_header_t *s_header   = NULL;
static const uint32_t     *s_offsets  = NULL;
static const char         *s_pool     = NULL;
static esp_partition_mmap_handle_t s_mmap_handle = 0;
static bool                s_initialized = false;

// ── Public API ─────────────────────────────────────────────────────────────

esp_err_t nanojs8_jsc_map_init(void) {
    if (s_initialized) return ESP_OK;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "jsc_map");
    if (part == NULL) {
        ESP_LOGE(TAG, "Partition 'jsc_map' not found — check partitions.csv");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Partition 'jsc_map' located: offset=0x%06" PRIx32
                  " size=%" PRIu32 " B",
             (uint32_t)part->address, (uint32_t)part->size);

    if (part->size < sizeof(jsc_header_t)) {
        ESP_LOGE(TAG, "Partition too small (%" PRIu32 " B) — needs at least %u",
                 (uint32_t)part->size, (unsigned)sizeof(jsc_header_t));
        return ESP_ERR_INVALID_SIZE;
    }

    // mmap the whole partition. Returns a pointer in the data-cache
    // address range (0x3F000000-0x3FFFFFFF on ESP32-S3 family). The
    // MMU keeps the mapping live until we (or shutdown) unmap.
    const void *mapped = NULL;
    esp_err_t err = esp_partition_mmap(
        part, /* src_offset */ 0, /* size */ part->size,
        ESP_PARTITION_MMAP_DATA, &mapped, &s_mmap_handle);
    if (err != ESP_OK || mapped == NULL) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s",
                 esp_err_to_name(err));
        return (err != ESP_OK) ? err : ESP_ERR_NO_MEM;
    }

    const jsc_header_t *hdr = (const jsc_header_t *)mapped;

    // Validate header. Magic + version are sentinels — wrong values mean
    // the partition wasn't flashed or was overwritten.
    if (hdr->magic != JSC_MAP_MAGIC) {
        ESP_LOGE(TAG, "Bad magic: got 0x%08" PRIx32 " expected 0x%08" PRIx32
                      " (jsc_map.bin probably not flashed)",
                 hdr->magic, (uint32_t)JSC_MAP_MAGIC);
        esp_partition_munmap(s_mmap_handle);
        s_mmap_handle = 0;
        return ESP_ERR_INVALID_VERSION;
    }
    if (hdr->version != JSC_MAP_VERSION) {
        ESP_LOGE(TAG, "Version mismatch: got %" PRIu32 " expected %u",
                 hdr->version, JSC_MAP_VERSION);
        esp_partition_munmap(s_mmap_handle);
        s_mmap_handle = 0;
        return ESP_ERR_INVALID_VERSION;
    }

    // Compute expected layout size and verify it fits in the partition.
    // Note: 64-bit arithmetic to avoid overflow on pathological inputs.
    uint64_t expected_size = (uint64_t)sizeof(jsc_header_t)
                              + (uint64_t)hdr->entry_count * sizeof(uint32_t)
                              + (uint64_t)hdr->pool_size;
    if (expected_size > part->size) {
        ESP_LOGE(TAG, "Header claims %llu B but partition is %" PRIu32 " B",
                 (unsigned long long)expected_size, (uint32_t)part->size);
        esp_partition_munmap(s_mmap_handle);
        s_mmap_handle = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    if (expected_size > JSC_MAP_MAX_TOTAL) {
        ESP_LOGE(TAG, "Header claims unreasonable total %llu B",
                 (unsigned long long)expected_size);
        esp_partition_munmap(s_mmap_handle);
        s_mmap_handle = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    // Stash pointers. Layout: header @ mapped, offsets after header,
    // pool after offsets. All within the same memory-mapped region.
    s_header  = hdr;
    s_offsets = (const uint32_t *)((const uint8_t *)mapped + sizeof(jsc_header_t));
    s_pool    = (const char *)(s_offsets + hdr->entry_count);
    s_initialized = true;

    ESP_LOGI(TAG, "Loaded: %" PRIu32 " entries, pool=%" PRIu32 " B, "
                  "mmap'd @ %p (%llu B total)",
             hdr->entry_count, hdr->pool_size, mapped,
             (unsigned long long)expected_size);

    // Sanity log: first and last word + a few in the middle. Confirms
    // pointers are resolving correctly without committing to specific
    // values (which vary if upstream JSC data ever changes).
    if (hdr->entry_count >= 4) {
        ESP_LOGI(TAG, "Sanity: [0]='%s' [25]='%s' [%" PRIu32 "]='%s'",
                 nanojs8_jsc_map_word(0),
                 nanojs8_jsc_map_word(25),
                 hdr->entry_count - 1,
                 nanojs8_jsc_map_word(hdr->entry_count - 1));
    }

    return ESP_OK;
}

uint32_t nanojs8_jsc_map_count(void) {
    return s_initialized ? s_header->entry_count : 0;
}

uint32_t nanojs8_jsc_map_pool_size(void) {
    return s_initialized ? s_header->pool_size : 0;
}

const char *nanojs8_jsc_map_word(uint32_t idx) {
    if (!s_initialized) return NULL;
    if (idx >= s_header->entry_count) return NULL;

    uint32_t off = s_offsets[idx];
    // Defensive bounds check on the offset itself — guards against a
    // mis-flashed/corrupt partition that passes the header check but
    // has bad offsets.
    if (off >= s_header->pool_size) return NULL;
    return s_pool + off;
}
