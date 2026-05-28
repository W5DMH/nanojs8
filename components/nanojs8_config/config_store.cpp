// NanoJS8 — Config store implementation.
//
// All NVS access is contained in this file. The rest of the project
// touches config only through the public API in config_store.h.

#include "config_store.h"

#include <cctype>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

// Radio profile catalog. The order here is the order the SETUP screen's
// "Radio" menu cycles through (Up/Down arrows). When adding a profile,
// also bump NANOJS8_CONFIG_VERSION in config_schema.h if any persisted
// data layout changes alongside it.
const char* const NANOJS8_RADIO_PROFILES[] = {
    "qdx",
    "g90_digirig",
    "digirig_unknown",
};
const size_t NANOJS8_RADIO_PROFILES_COUNT =
    sizeof(NANOJS8_RADIO_PROFILES) / sizeof(NANOJS8_RADIO_PROFILES[0]);

namespace nanojs8 {
namespace config {

static const char* TAG = "cfg";

// NVS namespace name. ESP-IDF's NVS API limits this to 15 chars + NUL.
// "nanojs8" leaves 7 chars of headroom for future per-screen sub-namespaces
// if Phase 2+ wants them.
static const char* NVS_NAMESPACE = "nanojs8";

// NVS key names. All deliberately short — NVS keys are 15 chars max.
// "v" is intentional: it's used in every load() and a single byte saves
// log noise when dumping the namespace contents.
static const char* KEY_VERSION   = "v";
static const char* KEY_CALLSIGN  = "call";
static const char* KEY_GRID      = "grid";
static const char* KEY_RADIO     = "radio";
static const char* KEY_GROUPS    = "groups";
static const char* KEY_AUTOSTART = "rauto";   // v3+: radio autostart bool






// In-memory singleton. Mutated by set_*() and save(); read via current().
// Initialized to zero, populated by load() at boot.
static Config s_current = {};

// Helper: safely copy a C string into a fixed-size buffer with NUL
// termination guaranteed. Returns true if the source fit, false if it
// was truncated (validators upstream should already have caught
// over-length input, but this is defense in depth).
static bool safe_strcpy(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0 || !src) {
        return false;
    }
    const size_t src_len = std::strlen(src);
    if (src_len >= dst_size) {
        std::memcpy(dst, src, dst_size - 1);
        dst[dst_size - 1] = '\0';
        return false;
    }
    std::memcpy(dst, src, src_len + 1);  // include NUL
    return true;
}

// Helper: populate s_current with defaults from config_schema.h.
static void apply_defaults() {
    s_current.version = NANOJS8_CONFIG_VERSION;
    safe_strcpy(s_current.callsign, sizeof(s_current.callsign), NANOJS8_DEFAULT_CALLSIGN);
    safe_strcpy(s_current.grid,     sizeof(s_current.grid),     NANOJS8_DEFAULT_GRID);
    safe_strcpy(s_current.radio,    sizeof(s_current.radio),    NANOJS8_DEFAULT_RADIO);
    safe_strcpy(s_current.groups,   sizeof(s_current.groups),   NANOJS8_DEFAULT_GROUPS);
    s_current.radio_autostart = NANOJS8_DEFAULT_RADIO_AUTOSTART;
}

// -------------------------------------------------------------------------
// Validation
// -------------------------------------------------------------------------

bool valid_callsign(const char* value) {
    if (!value) {
        return false;
    }
    const size_t len = std::strlen(value);
    if (len < 3 || len >= NANOJS8_CALLSIGN_MAXLEN) {
        return false;
    }
    // Allow uppercase ASCII letters, digits, and '/' (for /P, /M, /MM, /AM,
    // and country-prefix-prepended calls like DL/W5DMH/P). Lowercase is
    // accepted at input time and upper-cased on commit by set_callsign().
    for (size_t i = 0; i < len; ++i) {
        const char c = value[i];
        const bool is_alnum = (c >= '0' && c <= '9') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z');
        if (!is_alnum && c != '/') {
            return false;
        }
    }
    // Amateur callsigns must contain at least one digit. This excludes
    // letters-only strings (ship/military prefixes that aren't amateur)
    // and catches typos like "WPPCU". Slash-suffix calls (W5DMH/P) and
    // country-prefix-prepended calls (DL/W5DMH/P) pass because the digit
    // requirement is anywhere in the full string.
    bool has_digit = false;
    for (size_t i = 0; i < len; ++i) {
        if (value[i] >= '0' && value[i] <= '9') {
            has_digit = true;
            break;
        }
    }
    return has_digit;
}

bool valid_grid(const char* value) {
    if (!value) {
        return false;
    }
    const size_t len = std::strlen(value);
    // Maidenhead 4-char (field+square) or 6-char (with subsquare).
    if (len != 4 && len != 6) {
        return false;
    }
    // Char 0,1: A..R   (case-insensitive — committed as uppercase by setter)
    // Char 2,3: 0..9
    // Char 4,5: a..x   (case-insensitive — committed as lowercase by setter)
    auto in_range = [](char c, char lo, char hi) {
        return c >= lo && c <= hi;
    };
    auto upper = [](char c) {
        return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
    };
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    };
    if (!in_range(upper(value[0]), 'A', 'R')) return false;
    if (!in_range(upper(value[1]), 'A', 'R')) return false;
    if (!in_range(value[2], '0', '9'))        return false;
    if (!in_range(value[3], '0', '9'))        return false;
    if (len == 6) {
        if (!in_range(lower(value[4]), 'a', 'x')) return false;
        if (!in_range(lower(value[5]), 'a', 'x')) return false;
    }
    return true;
}

bool valid_radio(const char* value) {
    if (!value) {
        return false;
    }
    for (size_t i = 0; i < NANOJS8_RADIO_PROFILES_COUNT; ++i) {
        if (std::strcmp(value, NANOJS8_RADIO_PROFILES[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool valid_groups(const char* value) {
    // Empty is valid (operator has no group memberships).
    if (!value || value[0] == '\0') {
        return true;
    }
    const size_t total_len = std::strlen(value);
    if (total_len >= NANOJS8_GROUPS_MAXLEN) {
        return false;
    }
    // Parse comma-separated entries. Each entry must match @[A-Z0-9]{1,9}.
    // @ALLCALL and @HB are rejected (implicit at the protocol layer).
    size_t entry_count   = 0;
    size_t entry_start   = 0;
    bool   parsing_entry = true;
    for (size_t i = 0; i <= total_len; ++i) {
        const char c = (i < total_len) ? value[i] : ',';  // virtual trailing comma
        if (c == ',' || i == total_len) {
            if (!parsing_entry) {
                // Empty entry (",," or trailing ","): reject.
                return false;
            }
            const size_t entry_len = i - entry_start;
            // Minimum 2 chars ("@X"), maximum NANOJS8_GROUP_ENTRY_MAXLEN ("@" + 9).
            if (entry_len < 2 || entry_len > NANOJS8_GROUP_ENTRY_MAXLEN) {
                return false;
            }
            if (value[entry_start] != '@') {
                return false;
            }
            // Body must be 1..9 uppercase alphanumerics.
            for (size_t j = entry_start + 1; j < i; ++j) {
                const char bc = value[j];
                const bool ok = (bc >= 'A' && bc <= 'Z') || (bc >= '0' && bc <= '9');
                if (!ok) {
                    return false;
                }
            }
            // Reject implicit groups.
            if (entry_len == 8 && std::strncmp(&value[entry_start], "@ALLCALL", 8) == 0) {
                return false;
            }
            if (entry_len == 3 && std::strncmp(&value[entry_start], "@HB", 3) == 0) {
                return false;
            }
            entry_count++;
            if (entry_count > NANOJS8_MAX_GROUPS) {
                return false;
            }
            entry_start   = i + 1;
            parsing_entry = false;
        } else {
            parsing_entry = true;
        }
    }
    return true;
}

// -------------------------------------------------------------------------
// Setters
// -------------------------------------------------------------------------

esp_err_t set_callsign(const char* value) {
    if (!valid_callsign(value)) {
        ESP_LOGW(TAG, "set_callsign: rejected %s", value ? value : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    // Canonicalize to uppercase on commit. The validator allowed lowercase
    // as a typing convenience; persisted form is always uppercase.
    char buf[NANOJS8_CALLSIGN_MAXLEN];
    const size_t len = std::strlen(value);
    for (size_t i = 0; i < len; ++i) {
        const char c = value[i];
        buf[i] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
    }
    buf[len] = '\0';
    safe_strcpy(s_current.callsign, sizeof(s_current.callsign), buf);
    return ESP_OK;
}

esp_err_t set_grid(const char* value) {
    if (!valid_grid(value)) {
        ESP_LOGW(TAG, "set_grid: rejected %s", value ? value : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    // Canonicalize: first two upper, last two (if 6-char) lower.
    char buf[NANOJS8_GRID_MAXLEN];
    const size_t len = std::strlen(value);
    for (size_t i = 0; i < len; ++i) {
        const char c = value[i];
        if (i < 2) {
            buf[i] = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
        } else if (i < 4) {
            buf[i] = c;  // digits, no change
        } else {
            buf[i] = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        }
    }
    buf[len] = '\0';
    safe_strcpy(s_current.grid, sizeof(s_current.grid), buf);
    return ESP_OK;
}

esp_err_t set_radio(const char* value) {
    if (!valid_radio(value)) {
        ESP_LOGW(TAG, "set_radio: rejected %s", value ? value : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    safe_strcpy(s_current.radio, sizeof(s_current.radio), value);
    return ESP_OK;
}

esp_err_t set_groups(const char* value) {
    if (!valid_groups(value)) {
        ESP_LOGW(TAG, "set_groups: rejected %s", value ? value : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    // valid_groups already enforces uppercase, so direct copy is correct.
    // (The SETUP screen also uppercases letters at typing time; the
    // validator's strict A-Z check is the canonical guarantee.)
    safe_strcpy(s_current.groups, sizeof(s_current.groups),
                value ? value : "");
    return ESP_OK;
}

esp_err_t set_radio_autostart(bool value) {
    s_current.radio_autostart = value;
    return ESP_OK;
}

// -------------------------------------------------------------------------
// load / save
// -------------------------------------------------------------------------

esp_err_t load() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        // Best-effort: populate defaults in memory so the UI still runs.
        // The next save() will create the namespace.
        apply_defaults();
        return err;
    }

    // Read schema version. If absent, this is first boot.
    uint32_t persisted_version = 0;
    err = nvs_get_u32(handle, KEY_VERSION, &persisted_version);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "First boot — no nanojs8 namespace yet; applying defaults");
        apply_defaults();
        nvs_close(handle);
        // Flush defaults to NVS immediately so subsequent boots find them.
        return save();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_u32(version) failed: %s", esp_err_to_name(err));
        apply_defaults();
        nvs_close(handle);
        return err;
    }

    // Migration handling. Each shape change preserves what it can:
    //   v1 → v3: read CALL/GRID/RADIO, default GROUPS empty and
    //            radio_autostart OFF
    //   v2 → v3: read CALL/GRID/RADIO/GROUPS, default radio_autostart OFF
    // Other version mismatches (future schemas we don't understand,
    // corruption) fall through to a default-and-rewrite.
    bool needs_migration_save = false;
    if (persisted_version == 1 && NANOJS8_CONFIG_VERSION == 3) {
        ESP_LOGI(TAG, "Migrating config v1 → v3 (preserving CALL/GRID/RADIO, "
                      "defaulting GROUPS empty and AUTOSTART off)");
        needs_migration_save = true;
    } else if (persisted_version == 2 && NANOJS8_CONFIG_VERSION == 3) {
        ESP_LOGI(TAG, "Migrating config v2 → v3 (preserving CALL/GRID/RADIO/"
                      "GROUPS, defaulting AUTOSTART off)");
        needs_migration_save = true;
    } else if (persisted_version != NANOJS8_CONFIG_VERSION) {
        ESP_LOGW(TAG, "Config version mismatch (on-disk=%u, code=%u); resetting to defaults",
                 (unsigned)persisted_version, (unsigned)NANOJS8_CONFIG_VERSION);
        apply_defaults();
        nvs_close(handle);
        return save();
    }

    // Read each string field. nvs_get_str needs a length-probe first.
    auto read_str = [&handle](const char* key, char* dst, size_t dst_size,
                              const char* fallback) -> esp_err_t {
        size_t needed = 0;
        esp_err_t e = nvs_get_str(handle, key, nullptr, &needed);
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "key %s not found, using default", key);
            safe_strcpy(dst, dst_size, fallback);
            return ESP_OK;
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "nvs_get_str(%s) probe failed: %s", key, esp_err_to_name(e));
            safe_strcpy(dst, dst_size, fallback);
            return e;
        }
        if (needed > dst_size) {
            ESP_LOGW(TAG, "key %s on-disk len %zu exceeds buffer %zu; using default",
                     key, needed, dst_size);
            safe_strcpy(dst, dst_size, fallback);
            return ESP_OK;
        }
        e = nvs_get_str(handle, key, dst, &needed);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "nvs_get_str(%s) read failed: %s", key, esp_err_to_name(e));
            safe_strcpy(dst, dst_size, fallback);
        }
        return e;
    };

    s_current.version = NANOJS8_CONFIG_VERSION;  // always store the current version
    read_str(KEY_CALLSIGN, s_current.callsign, sizeof(s_current.callsign), NANOJS8_DEFAULT_CALLSIGN);
    read_str(KEY_GRID,     s_current.grid,     sizeof(s_current.grid),     NANOJS8_DEFAULT_GRID);
    read_str(KEY_RADIO,    s_current.radio,    sizeof(s_current.radio),    NANOJS8_DEFAULT_RADIO);
    read_str(KEY_GROUPS,   s_current.groups,   sizeof(s_current.groups),   NANOJS8_DEFAULT_GROUPS);

    // Read radio_autostart. Stored as u8. Missing key → use default
    // (which is also what happens during v1/v2 → v3 migration).
    uint8_t autostart_u8 = NANOJS8_DEFAULT_RADIO_AUTOSTART ? 1 : 0;
    esp_err_t e_auto = nvs_get_u8(handle, KEY_AUTOSTART, &autostart_u8);
    if (e_auto == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "key %s not found, using default (%s)", KEY_AUTOSTART,
                 NANOJS8_DEFAULT_RADIO_AUTOSTART ? "on" : "off");
        autostart_u8 = NANOJS8_DEFAULT_RADIO_AUTOSTART ? 1 : 0;
    } else if (e_auto != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u8(%s) failed: %s — using default",
                 KEY_AUTOSTART, esp_err_to_name(e_auto));
    }
    s_current.radio_autostart = (autostart_u8 != 0);

    nvs_close(handle);

    // Flush after migration so the next boot finds v3 layout. The
    // existing callsign/grid/radio/groups values are preserved; only
    // the version field and the new keys (autostart, and groups if
    // migrating from v1) are written.
    if (needs_migration_save) {
        return save();
    }
    return ESP_OK;
}

esp_err_t save() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Use a chain helper so we capture the first failure without leaking
    // the handle. NVS is forgiving about partial commits — uncommitted
    // changes are dropped on close — so on error we just close without
    // committing.
    err = nvs_set_u32(handle, KEY_VERSION, s_current.version);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_CALLSIGN, s_current.callsign);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_GRID,     s_current.grid);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_RADIO,    s_current.radio);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_GROUPS,   s_current.groups);
    if (err == ESP_OK) err = nvs_set_u8 (handle, KEY_AUTOSTART, s_current.radio_autostart ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_set failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_commit failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Config saved (call=%s grid=%s radio=%s groups=%s autostart=%s)",
             s_current.callsign, s_current.grid, s_current.radio,
             s_current.groups[0] ? s_current.groups : "(none)",
             s_current.radio_autostart ? "on" : "off");
    return ESP_OK;
}

// -------------------------------------------------------------------------
// Read-only accessor + utilities
// -------------------------------------------------------------------------

const Config& current() {
    return s_current;
}

void log_current() {
    ESP_LOGI(TAG, "Current config (v%u):", (unsigned)s_current.version);
    ESP_LOGI(TAG, "  callsign:  %s", s_current.callsign);
    ESP_LOGI(TAG, "  grid:      %s", s_current.grid);
    ESP_LOGI(TAG, "  radio:     %s", s_current.radio);
    ESP_LOGI(TAG, "  groups:    %s", s_current.groups[0] ? s_current.groups : "(none)");
    ESP_LOGI(TAG, "  autostart: %s", s_current.radio_autostart ? "on" : "off");
}

bool is_default_callsign() {
    return std::strcmp(s_current.callsign, NANOJS8_DEFAULT_CALLSIGN) == 0;
}

} // namespace config
} // namespace nanojs8
