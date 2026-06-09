/*
 * mailbox.c — NanoJS8 v0.7 L7.11g.2 NVS-persistent mailbox
 * =========================================================
 *
 * See mailbox.h for the API contract.
 *
 * Internal design notes:
 *
 *   - Flat array, NOT a ring. Slot index is meaningless for ordering;
 *     iteration is ordered by entry.id when callers want newest-first.
 *     EMPTY slots are skipped. delete() can leave holes anywhere; the
 *     next add() finds the first EMPTY slot via linear scan (fast at
 *     16 slots).
 *
 *   - id is allocated from a monotonically increasing counter, wrapped
 *     to uint16_t (~65k entries before wrap — decades). The counter is
 *     persisted in the blob header alongside the slots; on first boot
 *     it starts at 1 (id=0 is reserved as a "no id" sentinel for
 *     future API extensions).
 *
 *   - NVS blob layout is one fixed-size struct: header (count, head,
 *     next_id, magic, version) + slots[16]. We always write the whole
 *     blob on any change. With ~10 ops/day and NVS wear leveling, the
 *     write rate doesn't approach internal-flash endurance limits.
 *
 *   - Eviction policy when full: DELIVERED → READ → STORE → UNREAD,
 *     oldest-first within each tier (lowest id). Implemented as four
 *     linear scans; trivial at 16 slots.
 *
 *   - The cached counts (s_count, s_count_unread) are maintained
 *     under the lock but read without it. The values are uint32_t so
 *     reads are atomic on ESP32-S3; they can be off by ±1 if a
 *     concurrent operation is in flight, which is fine for the UI
 *     badge.
 *
 * License: GPL-3.0
 */

#include "mailbox.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"      // L7.16: persist worker latency log
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"  // L7.16: persist worker task
#include "nvs.h"
#include "nvs_flash.h"
#include "time_source.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mailbox";

// ── NVS layout ───────────────────────────────────────────────────────

// Namespace separate from the config component's "nanojs8" namespace.
#define NVS_NAMESPACE   "nj8_inbox"
// Single blob key for the entire mailbox. Keeping the key short (≤15
// chars, NVS limit) is mandatory.
#define NVS_KEY_BLOB    "blob"

// Magic identifies the blob layout. ASCII "NJ8M" little-endian: this
// makes corrupted/truncated blobs easy to spot in a hex dump.
#define BLOB_MAGIC      0x4D384A4Eu    // 'NJ8M'

// Bump on any layout change to nanojs8_mailbox_entry_t or this header.
// Old blobs with mismatched versions are discarded at load and a
// fresh empty array is initialised.
#define BLOB_VERSION    1u

typedef struct {
    uint32_t                  magic;
    uint16_t                  version;
    uint16_t                  next_id;        // counter for id allocation
    uint16_t                  count_total;    // cached non-EMPTY slots
    uint16_t                  count_unread;   // cached UNREAD slots
    nanojs8_mailbox_entry_t   slots[NANOJS8_MAILBOX_MAX];
} nanojs8_mailbox_blob_t;

// Compile-time sanity check on blob size — the implementation
// silently breaks if natural alignment ever shifts. 144 B/entry × 16
// + 12 B header = 2316 B. NVS partition is 24 KB (per partitions.csv),
// well within bounds for a single blob.
_Static_assert(sizeof(nanojs8_mailbox_entry_t) == 144,
               "Mailbox entry size changed — bump BLOB_VERSION and "
               "review NVS migration");
_Static_assert(sizeof(nanojs8_mailbox_blob_t) == 12 + 16 * 144,
               "Mailbox blob header padded — review struct layout");

// ── State ────────────────────────────────────────────────────────────

static nanojs8_mailbox_entry_t  s_slots[NANOJS8_MAILBOX_MAX];
static uint16_t                 s_next_id;
static SemaphoreHandle_t        s_mutex;
static bool                     s_initialized;
static nvs_handle_t             s_nvs_handle;
static bool                     s_nvs_open;

// Cached counts (race-tolerant for read-without-lock; see header).
static atomic_uint              s_count_total;
static atomic_uint              s_count_unread;

// ── L7.16 deferred persist worker ────────────────────────────────────
//
// nvs_set_blob() disables the SPI cache mid-write. PSRAM lives on the
// same octal-SPI bus, so any task with a PSRAM-located stack will trip
// the IDF's esp_task_stack_is_sane_cache_disabled() assertion if it
// runs while the cache is down. js8sync was moved to a PSRAM stack in
// L7.14-fix7 (to free DRAM headroom); the first time it had to commit
// an inbound MSG to the mailbox INBOX (KD8PGB/P → W5DMH MSG end-to-end
// path), the assertion fired and the device rebooted.
//
// Fix: keep the in-memory slot update synchronous (so UI screens see
// the new entry immediately) but defer the NVS persist to a worker
// task with a DRAM stack. Each public mutator that previously called
// persist_locked() now calls request_persist_locked() instead, which
// gives a binary semaphore. The worker takes the semaphore, re-takes
// the mutex, calls persist_locked() under the mutex (so it sees a
// consistent snapshot), and loops. Multiple rapid updates coalesce
// to one persist if the worker is already running.
//
// Stack: 4 KB DRAM is comfortable for nvs_set_blob (blob size 2316 B
// plus NVS internals). Created with xTaskCreatePinnedToCore (NOT
// *WithCaps) so the stack lands in internal RAM, immune to the cache-
// disable hazard.
//
// Priority: 2 (below UI=5, well below js8sync). Worker should never
// preempt the time-critical decode/UI paths.
//
// Pinned to Core 0 (where main_task runs) to keep Core 1 free for
// js8sync's heavy LDPC work.
#define PERSIST_WORKER_STACK_BYTES   4096
#define PERSIST_WORKER_PRIORITY      2
#define PERSIST_WORKER_CORE          0
static SemaphoreHandle_t        s_persist_signal;
static TaskHandle_t             s_persist_worker_handle;

// ── Small helpers ────────────────────────────────────────────────────

// Truncating, NUL-guaranteed string copy.
static void safe_strncpy(char *dst, size_t dst_n, const char *src)
{
    if (dst_n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = dst_n - 1;
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// In-place ASCII uppercase. JS8 convention: callsigns are uppercase,
// '@' (group prefix) and '/' (portable suffix) pass through unchanged.
static void uppercase_inplace(char *s)
{
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        *s = (char)toupper(c);
    }
}

// Recompute cached counts by full scan. Called after load_from_nvs()
// (where we trust the blob header but verify) and after persist_locked()
// (to ensure header values match what we actually wrote).
static void recount_locked(uint16_t *out_total, uint16_t *out_unread)
{
    uint16_t total = 0;
    uint16_t unread = 0;
    for (uint32_t i = 0; i < NANOJS8_MAILBOX_MAX; ++i) {
        if (s_slots[i].type != NANOJS8_MAILBOX_TYPE_EMPTY) {
            ++total;
            if (s_slots[i].type == NANOJS8_MAILBOX_TYPE_UNREAD) ++unread;
        }
    }
    if (out_total)  *out_total  = total;
    if (out_unread) *out_unread = unread;
    atomic_store(&s_count_total,  (unsigned)total);
    atomic_store(&s_count_unread, (unsigned)unread);
}

// ── NVS load + save ──────────────────────────────────────────────────

// Read the blob from NVS into the in-memory slots. Called once at
// init() under the lock. On any failure (key missing, magic wrong,
// version wrong, size wrong) the slots are zeroed and we log the
// reason — no error escapes to the caller. First boot after firmware
// update is the most common "failure": ESP_ERR_NVS_NOT_FOUND.
static void load_from_nvs_locked(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;   // id=0 reserved

    if (!s_nvs_open) {
        ESP_LOGW(TAG, "load: NVS not open — starting empty");
        return;
    }

    nanojs8_mailbox_blob_t blob;
    size_t blob_size = sizeof(blob);
    esp_err_t err = nvs_get_blob(s_nvs_handle, NVS_KEY_BLOB, &blob, &blob_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "load: no prior blob — starting empty (first boot)");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load: nvs_get_blob failed (%s) — starting empty",
                 esp_err_to_name(err));
        return;
    }
    if (blob_size != sizeof(blob)) {
        ESP_LOGW(TAG, "load: blob size %u != expected %u "
                      "— layout change? starting empty",
                 (unsigned)blob_size, (unsigned)sizeof(blob));
        return;
    }
    if (blob.magic != BLOB_MAGIC) {
        ESP_LOGW(TAG, "load: bad magic 0x%08" PRIx32 " "
                      "(want 0x%08" PRIx32 ") — corrupt? starting empty",
                 (uint32_t)blob.magic, (uint32_t)BLOB_MAGIC);
        return;
    }
    if (blob.version != BLOB_VERSION) {
        ESP_LOGW(TAG, "load: blob version %u != current %u "
                      "— firmware upgrade? starting empty",
                 (unsigned)blob.version, (unsigned)BLOB_VERSION);
        return;
    }

    // All checks passed. Copy slots in; trust next_id; recount as a
    // belt-and-suspenders consistency check.
    memcpy(s_slots, blob.slots, sizeof(s_slots));
    s_next_id = blob.next_id == 0 ? 1 : blob.next_id;

    uint16_t total, unread;
    recount_locked(&total, &unread);
    ESP_LOGI(TAG, "load: %u entries (%u unread) restored from NVS "
                  "(next_id=%u)",
             (unsigned)total, (unsigned)unread, (unsigned)s_next_id);

    if (total != blob.count_total || unread != blob.count_unread) {
        ESP_LOGW(TAG, "load: header counts (total=%u unread=%u) didn't "
                      "match recount (total=%u unread=%u) — using recount, "
                      "next persist will fix the header",
                 (unsigned)blob.count_total, (unsigned)blob.count_unread,
                 (unsigned)total, (unsigned)unread);
    }
}

// Write the in-memory state to NVS as a single blob. Must be called
// with the mutex held. Returns ESP_OK or an NVS error code.
static esp_err_t persist_locked(void)
{
    if (!s_nvs_open) {
        ESP_LOGW(TAG, "persist: NVS not open — in-memory only");
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t total, unread;
    recount_locked(&total, &unread);

    nanojs8_mailbox_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic        = BLOB_MAGIC;
    blob.version      = BLOB_VERSION;
    blob.next_id      = s_next_id;
    blob.count_total  = total;
    blob.count_unread = unread;
    memcpy(blob.slots, s_slots, sizeof(s_slots));

    esp_err_t err = nvs_set_blob(s_nvs_handle, NVS_KEY_BLOB,
                                  &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist: nvs_set_blob failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist: nvs_commit failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

// L7.16: signal the persist worker that the in-memory state has
// changed and needs persisting. Caller MUST hold s_mutex (matches the
// existing persist_locked() contract). Returns nothing — the give is
// best-effort; if the semaphore is already at its cap of 1, the
// existing pending give covers the new dirty state too (binary
// semaphores coalesce, which is exactly what we want).
//
// This function replaces every former direct call to persist_locked()
// from public mutators (add_entry, transition, delete). The actual
// NVS write happens in persist_worker_task() below.
static void request_persist_locked(void)
{
    if (s_persist_signal != NULL) {
        // Returns pdFALSE if already at max (1). That's fine — the
        // pending give will result in a persist that sees our update,
        // since we hold the mutex and the worker takes it before
        // calling persist_locked. No data lost.
        (void)xSemaphoreGive(s_persist_signal);
    }
    // If signal not yet created (init still running), the in-memory
    // state is already updated — we just won't persist this update.
    // Init's own load_from_nvs_locked path is read-only, so this case
    // is only reachable via a pathological caller racing init.
}

// L7.16 persist worker. Runs on a DRAM stack so nvs_set_blob's cache-
// disable can't trip the stack-sanity assertion.
static void persist_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "persist worker task entered (DRAM stack, core %d, "
                  "prio %d, stack %u B)",
             PERSIST_WORKER_CORE, PERSIST_WORKER_PRIORITY,
             (unsigned)PERSIST_WORKER_STACK_BYTES);
    for (;;) {
        // Wait indefinitely for a dirty signal. Coalesces multiple
        // updates (binary semaphore caps at 1) so we don't re-persist
        // every micro-change if updates burst in.
        if (xSemaphoreTake(s_persist_signal, portMAX_DELAY) != pdTRUE) {
            // Should never happen with portMAX_DELAY; log and back off.
            ESP_LOGW(TAG, "persist worker: signal take failed unexpectedly");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Take the mailbox mutex so persist_locked sees a consistent
        // snapshot. The 500 ms cap is well above the typical mutex
        // hold time (a few ms for add/transition/delete) — if we
        // can't get it in 500 ms something is seriously wrong, log
        // and retry on the next signal.
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "persist worker: mutex timeout — will retry "
                          "on next signal (in-memory state intact, "
                          "NVS skipped this round)");
            continue;
        }

        const int64_t t0_us = esp_timer_get_time();
        const esp_err_t err = persist_locked();
        const uint32_t took_ms =
            (uint32_t)((esp_timer_get_time() - t0_us) / 1000);
        xSemaphoreGive(s_mutex);

        if (err == ESP_OK) {
            ESP_LOGD(TAG, "persist worker: NVS commit ok in %" PRIu32
                          " ms (total=%u unread=%u)",
                     took_ms,
                     (unsigned)atomic_load(&s_count_total),
                     (unsigned)atomic_load(&s_count_unread));
        } else {
            ESP_LOGW(TAG, "persist worker: NVS commit failed (%s) "
                          "after %" PRIu32 " ms — in-memory state "
                          "intact, will retry on next change",
                     esp_err_to_name(err), took_ms);
        }
    }
}

// ── Slot search / allocation ────────────────────────────────────────

// Locate the slot index for a given id. Returns -1 if not found.
// Caller must hold the mutex.
static int find_slot_by_id_locked(uint16_t id)
{
    if (id == 0) return -1;  // 0 = sentinel
    for (int i = 0; i < NANOJS8_MAILBOX_MAX; ++i) {
        if (s_slots[i].type != NANOJS8_MAILBOX_TYPE_EMPTY &&
            s_slots[i].id == id) {
            return i;
        }
    }
    return -1;
}

// Find the oldest (lowest id) non-empty slot of the given type, or
// -1 if no slot of that type exists. Caller must hold the mutex.
static int find_oldest_of_type_locked(nanojs8_mailbox_type_t type)
{
    int      best_idx = -1;
    uint16_t best_id  = UINT16_MAX;
    for (int i = 0; i < NANOJS8_MAILBOX_MAX; ++i) {
        if (s_slots[i].type == (uint8_t)type) {
            if (s_slots[i].id < best_id) {
                best_id  = s_slots[i].id;
                best_idx = i;
            }
        }
    }
    return best_idx;
}

// Pick the slot to use for a new entry: first prefer an EMPTY slot;
// otherwise evict by policy (DELIVERED → READ → STORE → UNREAD).
// Returns slot index in [0, NANOJS8_MAILBOX_MAX); never fails because
// at minimum one of those four eviction tiers must contain something
// when no EMPTY slot exists. Caller must hold the mutex.
static int pick_slot_for_new_locked(void)
{
    for (int i = 0; i < NANOJS8_MAILBOX_MAX; ++i) {
        if (s_slots[i].type == NANOJS8_MAILBOX_TYPE_EMPTY) {
            return i;
        }
    }
    // Full ring — eviction tiers in order of least-disruptive-first.
    int idx;
    if ((idx = find_oldest_of_type_locked(NANOJS8_MAILBOX_TYPE_DELIVERED)) >= 0) {
        ESP_LOGI(TAG, "ring full — evicting DELIVERED id=%u",
                 (unsigned)s_slots[idx].id);
        return idx;
    }
    if ((idx = find_oldest_of_type_locked(NANOJS8_MAILBOX_TYPE_READ)) >= 0) {
        ESP_LOGI(TAG, "ring full — evicting READ id=%u",
                 (unsigned)s_slots[idx].id);
        return idx;
    }
    if ((idx = find_oldest_of_type_locked(NANOJS8_MAILBOX_TYPE_STORE)) >= 0) {
        ESP_LOGW(TAG, "ring full — evicting STORE id=%u "
                      "(undelivered store-and-forward lost)",
                 (unsigned)s_slots[idx].id);
        return idx;
    }
    if ((idx = find_oldest_of_type_locked(NANOJS8_MAILBOX_TYPE_UNREAD)) >= 0) {
        ESP_LOGW(TAG, "ring full — evicting UNREAD id=%u "
                      "(operator never saw this message)",
                 (unsigned)s_slots[idx].id);
        return idx;
    }
    // Unreachable: count == MAX implies at least one of the above
    // returned a hit. Return 0 defensively to avoid undefined behaviour.
    ESP_LOGE(TAG, "pick_slot_for_new: unreachable — full ring with "
                  "no evictable slots; using index 0");
    return 0;
}

// Allocate the next id, wrapping past 0. Caller must hold the mutex.
static uint16_t alloc_id_locked(void)
{
    uint16_t id = s_next_id;
    if (++s_next_id == 0) {
        s_next_id = 1;     // skip the 0 sentinel on wrap
        ESP_LOGW(TAG, "next_id wrapped past 65535");
    }
    return id;
}

// ── Lifecycle ────────────────────────────────────────────────────────

esp_err_t nanojs8_mailbox_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "init: failed to create mutex");
        return ESP_FAIL;
    }

    // Open the NVS handle. nvs_flash_init() is the config component's
    // responsibility (it runs earlier in main); we just open our
    // namespace here. If the handle fails to open we still come up
    // — operation degrades to in-memory only (warn-logged).
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err == ESP_OK) {
        s_nvs_open = true;
    } else {
        ESP_LOGW(TAG, "init: nvs_open('%s') failed: %s — "
                      "running without persistence",
                 NVS_NAMESPACE, esp_err_to_name(err));
        s_nvs_open = false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    load_from_nvs_locked();
    xSemaphoreGive(s_mutex);

    // L7.16: deferred-persist worker. Created AFTER load_from_nvs_locked
    // so the worker can't fire spuriously during boot-time NVS reads;
    // created BEFORE s_initialized=true so all public mutators (which
    // gate on s_initialized) see a ready worker on their first call.
    s_persist_signal = xSemaphoreCreateBinary();
    if (!s_persist_signal) {
        ESP_LOGE(TAG, "init: failed to create persist signal — "
                      "running in-memory-only (no NVS persistence)");
        // Fall through: in-memory mailbox still works; updates will
        // not survive reboot. The request_persist_locked() helper
        // null-checks the signal handle so calls remain safe.
    } else {
        BaseType_t rc = xTaskCreatePinnedToCore(
            persist_worker_task,
            "mailbox_persist",
            PERSIST_WORKER_STACK_BYTES,
            NULL,
            PERSIST_WORKER_PRIORITY,
            &s_persist_worker_handle,
            PERSIST_WORKER_CORE);
        if (rc != pdPASS) {
            ESP_LOGE(TAG, "init: xTaskCreatePinnedToCore for persist "
                          "worker failed (rc=%d) — running in-memory-"
                          "only", (int)rc);
            vSemaphoreDelete(s_persist_signal);
            s_persist_signal = NULL;
            s_persist_worker_handle = NULL;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Mailbox ready: capacity=%u, footprint=%u B (BSS) + "
                  "%u B (NVS blob), %u entries restored",
             (unsigned)NANOJS8_MAILBOX_MAX,
             (unsigned)sizeof(s_slots),
             (unsigned)sizeof(nanojs8_mailbox_blob_t),
             (unsigned)atomic_load(&s_count_total));
    return ESP_OK;
}

// ── Insertion ────────────────────────────────────────────────────────

// Shared body for add_unread/add_store. Validates inputs, picks a
// slot, fills it, persists. Caller fills in type/snr/freq via the
// `template` parameter; from_call/to_call/body come from arguments.
static esp_err_t add_entry(nanojs8_mailbox_type_t  type,
                            const char             *from_call,
                            const char             *to_call,
                            const char             *body,
                            uint32_t                freq_hz,
                            int8_t                  snr_db)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "add: not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    if (!from_call || from_call[0] == '\0') {
        ESP_LOGW(TAG, "add: empty from_call — refusing");
        return ESP_ERR_INVALID_ARG;
    }
    if (!to_call || to_call[0] == '\0') {
        ESP_LOGW(TAG, "add: empty to_call — refusing");
        return ESP_ERR_INVALID_ARG;
    }
    if (!body || body[0] == '\0') {
        ESP_LOGW(TAG, "add: empty body — refusing");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "add: mutex timeout");
        return ESP_FAIL;
    }

    const int slot_idx = pick_slot_for_new_locked();
    nanojs8_mailbox_entry_t *slot = &s_slots[slot_idx];

    // Zero the slot first so we don't leak stray bytes from a prior
    // (evicted) entry into the new one's string fields.
    memset(slot, 0, sizeof(*slot));

    slot->id   = alloc_id_locked();
    slot->type = (uint8_t)type;
    slot->utc_seconds_today = nanojs8_time_is_set()
                                 ? nanojs8_time_seconds_today() : 0;
    slot->freq_hz = freq_hz;
    slot->snr_db  = snr_db;

    safe_strncpy(slot->from_call, sizeof(slot->from_call), from_call);
    uppercase_inplace(slot->from_call);
    safe_strncpy(slot->to_call, sizeof(slot->to_call), to_call);
    uppercase_inplace(slot->to_call);
    safe_strncpy(slot->body, sizeof(slot->body), body);
    // body preserved verbatim — case-sensitive (could be a payload
    // hash, GPS coords, free text, etc.)

    request_persist_locked();  // L7.16: defer NVS write to worker
    recount_locked(NULL, NULL);  // refresh atomic caches

    ESP_LOGI(TAG, "Added %s id=%u from=%s to=%s body_len=%u "
                  "(total=%u unread=%u, persist=queued)",
             type == NANOJS8_MAILBOX_TYPE_UNREAD ? "UNREAD" : "STORE",
             (unsigned)slot->id,
             slot->from_call, slot->to_call,
             (unsigned)strlen(slot->body),
             (unsigned)atomic_load(&s_count_total),
             (unsigned)atomic_load(&s_count_unread));

    xSemaphoreGive(s_mutex);
    return ESP_OK;  // in-memory add succeeded even if persist failed
}

esp_err_t nanojs8_mailbox_add_unread(const char *from_call,
                                      const char *to_call,
                                      const char *body,
                                      uint32_t    freq_hz,
                                      int8_t      snr_db)
{
    return add_entry(NANOJS8_MAILBOX_TYPE_UNREAD,
                     from_call, to_call, body, freq_hz, snr_db);
}

esp_err_t nanojs8_mailbox_add_store(const char *from_call,
                                     const char *to_call,
                                     const char *body)
{
    return add_entry(NANOJS8_MAILBOX_TYPE_STORE,
                     from_call, to_call, body,
                     /*freq_hz=*/0, NANOJS8_MAILBOX_SNR_NA);
}

// ── State transitions ───────────────────────────────────────────────

// Shared body for mark_read/mark_delivered. expected_type guards
// the transition (mark_read on a STORE entry returns INVALID_STATE).
static esp_err_t transition(uint16_t                id,
                             nanojs8_mailbox_type_t  expected_type,
                             nanojs8_mailbox_type_t  new_type,
                             const char             *op_name)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: mutex timeout", op_name);
        return ESP_FAIL;
    }

    const int idx = find_slot_by_id_locked(id);
    esp_err_t result;
    if (idx < 0) {
        ESP_LOGW(TAG, "%s: id=%u not found", op_name, (unsigned)id);
        result = ESP_ERR_NOT_FOUND;
    } else if (s_slots[idx].type != (uint8_t)expected_type) {
        ESP_LOGW(TAG, "%s: id=%u has type=%u (expected %u) — refusing",
                 op_name, (unsigned)id,
                 (unsigned)s_slots[idx].type, (unsigned)expected_type);
        result = ESP_ERR_INVALID_STATE;
    } else {
        s_slots[idx].type = (uint8_t)new_type;
        request_persist_locked();  // L7.16: defer NVS write to worker
        recount_locked(NULL, NULL);
        ESP_LOGI(TAG, "%s id=%u (persist=queued)",
                 op_name, (unsigned)id);
        result = ESP_OK;
    }

    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t nanojs8_mailbox_mark_read(uint16_t id)
{
    return transition(id,
                      NANOJS8_MAILBOX_TYPE_UNREAD,
                      NANOJS8_MAILBOX_TYPE_READ,
                      "mark_read");
}

esp_err_t nanojs8_mailbox_mark_delivered(uint16_t id)
{
    return transition(id,
                      NANOJS8_MAILBOX_TYPE_STORE,
                      NANOJS8_MAILBOX_TYPE_DELIVERED,
                      "mark_delivered");
}

esp_err_t nanojs8_mailbox_delete(uint16_t id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "delete: mutex timeout");
        return ESP_FAIL;
    }

    const int idx = find_slot_by_id_locked(id);
    esp_err_t result;
    if (idx < 0) {
        ESP_LOGW(TAG, "delete: id=%u not found", (unsigned)id);
        result = ESP_ERR_NOT_FOUND;
    } else {
        memset(&s_slots[idx], 0, sizeof(s_slots[idx]));
        // type = EMPTY = 0 after memset
        request_persist_locked();  // L7.16: defer NVS write to worker
        recount_locked(NULL, NULL);
        ESP_LOGI(TAG, "delete id=%u (persist=queued)",
                 (unsigned)id);
        result = ESP_OK;
    }

    xSemaphoreGive(s_mutex);
    return result;
}

// ── Queries ──────────────────────────────────────────────────────────

bool nanojs8_mailbox_find_by_id(uint16_t                  id,
                                 nanojs8_mailbox_entry_t *out)
{
    if (!s_initialized || !out) return false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "find_by_id: mutex timeout");
        return false;
    }
    bool found = false;
    const int idx = find_slot_by_id_locked(id);
    if (idx >= 0) {
        *out = s_slots[idx];
        found = true;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

// Helper: compare two entries by id, descending (newest first). For
// uint16 wrap-around we'd need modular comparison, but with ≤16
// entries currently held the difference is always small and direct
// comparison is correct.
static int cmp_by_id_desc(const void *a, const void *b)
{
    const nanojs8_mailbox_entry_t *ea = a;
    const nanojs8_mailbox_entry_t *eb = b;
    if (ea->id > eb->id) return -1;
    if (ea->id < eb->id) return  1;
    return 0;
}

// L7.11g.6: oldest-first comparator. Used by find_holding_for() so
// the JS8 mailbox protocol delivers OLDEST stored messages first
// (FIFO semantics — matches MicroJS8's list_holding_for behavior
// and JS8Call's QUERY MSGS reply convention).
static int cmp_by_id_asc(const void *a, const void *b)
{
    const nanojs8_mailbox_entry_t *ea = a;
    const nanojs8_mailbox_entry_t *eb = b;
    if (ea->id < eb->id) return -1;
    if (ea->id > eb->id) return  1;
    return 0;
}

uint32_t nanojs8_mailbox_find_holding_for(const char              *to_call,
                                           nanojs8_mailbox_entry_t *out,
                                           uint32_t                 max)
{
    if (!s_initialized || !out || max == 0 || !to_call) return 0;

    // Uppercase the input on the stack for case-insensitive match.
    char want[NANOJS8_MAILBOX_CALL_LEN];
    safe_strncpy(want, sizeof(want), to_call);
    uppercase_inplace(want);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "find_holding_for: mutex timeout");
        return 0;
    }

    uint32_t n = 0;
    for (int i = 0; i < NANOJS8_MAILBOX_MAX && n < max; ++i) {
        if (s_slots[i].type == NANOJS8_MAILBOX_TYPE_STORE &&
            strcmp(s_slots[i].to_call, want) == 0) {
            out[n++] = s_slots[i];
        }
    }
    xSemaphoreGive(s_mutex);

    // L7.11g.6: sort OLDEST-first (ascending id) so the QUERY MSGS
    // handler delivers messages FIFO. This matches MicroJS8's
    // list_holding_for() semantics and avoids starving older entries.
    if (n > 1) {
        qsort(out, n, sizeof(out[0]), cmp_by_id_asc);
    }
    return n;
}

uint32_t nanojs8_mailbox_count(void)
{
    return atomic_load(&s_count_total);
}

uint32_t nanojs8_mailbox_count_unread(void)
{
    return atomic_load(&s_count_unread);
}

uint32_t nanojs8_mailbox_snapshot(nanojs8_mailbox_entry_t *out,
                                   uint32_t                 max)
{
    if (!s_initialized || !out || max == 0) return 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "snapshot: mutex timeout");
        return 0;
    }

    uint32_t n = 0;
    for (int i = 0; i < NANOJS8_MAILBOX_MAX && n < max; ++i) {
        if (s_slots[i].type != NANOJS8_MAILBOX_TYPE_EMPTY) {
            out[n++] = s_slots[i];
        }
    }
    xSemaphoreGive(s_mutex);

    if (n > 1) {
        qsort(out, n, sizeof(out[0]), cmp_by_id_desc);
    }
    return n;
}
