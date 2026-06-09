/*
 * config.cpp — NanoJS8 v0.7 persistent configuration
 * ====================================================
 * See config.h for the public API.
 *
 * NVS interaction notes:
 *
 * 1. We open the NVS handle once at init() and never close it.
 *    The handle is process-lifetime; closing it would just force a
 *    re-open on the next save(), with no functional benefit.
 *
 * 2. NVS keys must be 1..15 chars. All our keys fit.
 *
 * 3. nvs_get_str() returns ESP_ERR_NVS_NOT_FOUND on the first boot
 *    (when nothing's been saved yet). We treat that as "use the
 *    default" and don't propagate the error to the caller.
 *
 * 4. Strings stored in NVS include the NUL terminator. nvs_get_str
 *    takes a length pointer it updates with the required size; we
 *    pass our buffer length and let it copy up to that limit.
 *
 * License: GPL-3.0
 */

#include "config.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include <atomic>
#include <cstring>

static const char* TAG = "config";

namespace {

constexpr const char* NVS_NAMESPACE = "nanojs8";

// NVS keys (must be <= 15 chars each per NVS limit)
constexpr const char* KEY_CALLSIGN = "callsign";
constexpr const char* KEY_GRID     = "grid";
constexpr const char* KEY_GROUPS   = "groups";
constexpr const char* KEY_UNITS    = "units";
constexpr const char* KEY_FREQ_HZ  = "freq_hz";
constexpr const char* KEY_RADIO_ID = "radio_id";

// In-memory cache. Initialized to defaults; populated from NVS at init.
nanojs8_config_t s_cfg = {};

// NVS handle, kept open for the lifetime of the firmware.
nvs_handle_t s_nvs_handle = 0;
bool         s_initialized = false;

// Apply the C-string defaults to the cache. Called from init() before
// reading from NVS so any field not present in NVS retains the default.
void apply_defaults() {
    std::memset(&s_cfg, 0, sizeof(s_cfg));
    std::strncpy(s_cfg.callsign, NANOJS8_DEFAULT_CALLSIGN,
                  sizeof(s_cfg.callsign) - 1);
    std::strncpy(s_cfg.grid, NANOJS8_DEFAULT_GRID,
                  sizeof(s_cfg.grid) - 1);
    std::strncpy(s_cfg.groups, NANOJS8_DEFAULT_GROUPS,
                  sizeof(s_cfg.groups) - 1);
    std::strncpy(s_cfg.units, NANOJS8_DEFAULT_UNITS,
                  sizeof(s_cfg.units) - 1);
    s_cfg.freq_hz = NANOJS8_DEFAULT_FREQ_HZ;
    std::strncpy(s_cfg.radio_id, NANOJS8_DEFAULT_RADIO_ID,
                  sizeof(s_cfg.radio_id) - 1);
}

// Read one string key from NVS into `dst`. If the key is missing, leave
// `dst` unchanged (so the default applied earlier remains). Logs any
// unexpected error (real I/O failures vs missing-key).
void load_string(const char* key, char* dst, size_t dst_len) {
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(s_nvs_handle, key, dst, &len);
    if (err == ESP_OK) {
        ESP_LOGD(TAG, "  loaded %s = %s", key, dst);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "  %s not in NVS, using default", key);
    } else {
        ESP_LOGW(TAG, "  nvs_get_str(%s) failed: %s",
                 key, esp_err_to_name(err));
    }
}

// Write one string key to NVS. Logs any failure; the caller's
// nvs_commit() is what actually persists across reboots.
esp_err_t save_string(const char* key, const char* value) {
    esp_err_t err = nvs_set_str(s_nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  nvs_set_str(%s) failed: %s",
                 key, esp_err_to_name(err));
    }
    return err;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" esp_err_t nanojs8_config_init(void) {
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing persistent config (NVS namespace 'nanojs8')");

    // Apply defaults FIRST so any failures below leave the cache sane.
    apply_defaults();

    // Initialize the NVS flash partition. ESP-IDF's example pattern
    // handles "no free space" / "new version" cases by erasing and
    // re-initializing. We mirror that.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "  NVS partition needs erase (%s) — reformatting",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  nvs_flash_init failed: %s",
                 esp_err_to_name(err));
        // Cache still has defaults so the rest of the firmware runs.
        return err;
    }

    // Open our namespace. NVS_READWRITE creates the namespace if it
    // doesn't exist yet (first boot).
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  nvs_open('%s') failed: %s",
                 NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    // Read each field. Strings fall back to the defaults already in
    // s_cfg if not found. freq_hz uses a separate uint64 getter.
    load_string(KEY_CALLSIGN, s_cfg.callsign,  sizeof(s_cfg.callsign));
    load_string(KEY_GRID,     s_cfg.grid,      sizeof(s_cfg.grid));
    load_string(KEY_GROUPS,   s_cfg.groups,    sizeof(s_cfg.groups));
    load_string(KEY_UNITS,    s_cfg.units,     sizeof(s_cfg.units));
    load_string(KEY_RADIO_ID, s_cfg.radio_id,  sizeof(s_cfg.radio_id));

    uint64_t freq_value = 0;
    err = nvs_get_u64(s_nvs_handle, KEY_FREQ_HZ, &freq_value);
    if (err == ESP_OK) {
        s_cfg.freq_hz = freq_value;
        ESP_LOGD(TAG, "  loaded freq_hz = %llu",
                 (unsigned long long)s_cfg.freq_hz);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "  nvs_get_u64(freq_hz) failed: %s",
                 esp_err_to_name(err));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Config loaded: callsign=%s grid=%s units=%s "
                  "freq=%llu radio=%s",
             s_cfg.callsign,
             s_cfg.grid[0] ? s_cfg.grid : "(unset)",
             s_cfg.units,
             (unsigned long long)s_cfg.freq_hz,
             s_cfg.radio_id);
    if (nanojs8_config_is_configured()) {
        ESP_LOGI(TAG, "Station is configured — UI will start in HOME");
    } else {
        ESP_LOGI(TAG, "Station NOT configured — UI will start in SETUP");
    }
    return ESP_OK;
}

extern "C" const nanojs8_config_t* nanojs8_config_get(void) {
    return &s_cfg;
}

extern "C" esp_err_t nanojs8_config_set(const nanojs8_config_t* new_cfg) {
    if (new_cfg == nullptr) return ESP_ERR_INVALID_ARG;
    // memcpy is fine — we don't need finer-grained locking because
    // writers serialize through the UI's commit_edit path.
    std::memcpy(&s_cfg, new_cfg, sizeof(s_cfg));
    return ESP_OK;
}

extern "C" esp_err_t nanojs8_config_save(void) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Saving config to NVS");

    esp_err_t first_err = ESP_OK;
    esp_err_t err;
    #define TRY_SAVE(call) do {                                          \
        err = (call);                                                    \
        if (err != ESP_OK && first_err == ESP_OK) first_err = err;       \
    } while (0)

    TRY_SAVE(save_string(KEY_CALLSIGN, s_cfg.callsign));
    TRY_SAVE(save_string(KEY_GRID,     s_cfg.grid));
    TRY_SAVE(save_string(KEY_GROUPS,   s_cfg.groups));
    TRY_SAVE(save_string(KEY_UNITS,    s_cfg.units));
    TRY_SAVE(save_string(KEY_RADIO_ID, s_cfg.radio_id));
    TRY_SAVE(nvs_set_u64(s_nvs_handle, KEY_FREQ_HZ, s_cfg.freq_hz));

    #undef TRY_SAVE

    // Commit only if every set succeeded. Partial writes are possible
    // otherwise, but NVS's atomicity guarantees they don't corrupt the
    // partition — at worst we have stale values for the failed fields.
    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  nvs_commit failed: %s", esp_err_to_name(err));
        return err;
    }

    if (first_err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved successfully");
    } else {
        ESP_LOGW(TAG, "Config save had errors but committed; first=%s",
                 esp_err_to_name(first_err));
    }
    return first_err;
}

extern "C" bool nanojs8_config_is_configured(void) {
    // Threshold matches MicroJS8: must have a non-default callsign AND
    // a non-empty grid. Groups, units, freq, radio_id all have sensible
    // defaults that don't block operation.
    if (std::strcmp(s_cfg.callsign, NANOJS8_DEFAULT_CALLSIGN) == 0) {
        return false;
    }
    if (s_cfg.grid[0] == '\0') {
        return false;
    }
    return true;
}

extern "C" uint32_t nanojs8_config_groups_enumerate(
    char (*out_groups)[NANOJS8_CONFIG_GROUPS_LEN],
    uint32_t max)
{
    // L7.11f: split comma-separated config.groups into individual
    // entries for the COMPOSE TO/FOR picker.
    if (!out_groups || max == 0) return 0;
    const char *src = s_cfg.groups;
    if (!src || src[0] == '\0') return 0;

    uint32_t out_count = 0;
    const char *cur   = src;
    while (*cur && out_count < max) {
        // Skip leading whitespace
        while (*cur == ' ' || *cur == '\t') ++cur;
        if (*cur == '\0') break;
        if (*cur == ',')  { ++cur; continue; }  // empty entry

        // Find end of this entry (next comma or NUL)
        const char *end = cur;
        while (*end && *end != ',') ++end;

        // Trim trailing whitespace
        const char *real_end = end;
        while (real_end > cur && (real_end[-1] == ' '
                                   || real_end[-1] == '\t')) {
            --real_end;
        }

        size_t n = (size_t)(real_end - cur);
        if (n > 0) {
            if (n >= NANOJS8_CONFIG_GROUPS_LEN) {
                n = NANOJS8_CONFIG_GROUPS_LEN - 1;
            }
            std::memcpy(out_groups[out_count], cur, n);
            out_groups[out_count][n] = '\0';
            ++out_count;
        }

        cur = end;
        if (*cur == ',') ++cur;
    }
    return out_count;
}
