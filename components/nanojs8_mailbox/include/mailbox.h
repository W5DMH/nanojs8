/*
 * mailbox.h — NanoJS8 v0.7 L7.11g.2 NVS-persistent mailbox
 * ===================================================================
 *
 * Local typed message store for the JS8 mailbox protocol. Replaces
 * the L7.11f in-memory-only ring with an NVS-persistent flat array
 * that survives reboots, paired with an extended entry struct that
 * carries the metadata needed by the upcoming L7.11g sub-layers:
 *
 *   g.3 — INBOX + INBOX_DETAIL UI screens
 *   g.4 — MSG verb store + auto-ACK (4-deep TX FIFO)
 *   g.5 — MSG TO: store-and-forward (STORE rows for other callsigns)
 *   g.6 — QUERY MSGS / QUERY MSG <id> response
 *
 * Entry lifecycle:
 *
 *   UNREAD   — we received a MSG addressed to our callsign. New from
 *              the air, not yet displayed. Created by add_unread().
 *
 *   READ     — operator viewed it in INBOX_DETAIL. Created by
 *              mark_read(id). UI keeps it visible but dim.
 *
 *   STORE    — someone sent us "MSG TO:CALL body"; we hold it until
 *              CALL queries us via QUERY MSGS. Created by
 *              add_store(). FROM = the originator; TO = the
 *              destination we're holding it for.
 *
 *   DELIVERED — a STORE entry was successfully transmitted to TO via
 *              QUERY MSG <id> reply. Kept around so the operator can
 *              see what was delivered; first to be evicted when the
 *              ring fills.
 *
 *   EMPTY    — internal sentinel for an unused slot. Not exposed via
 *              snapshot() / find_*() (they skip EMPTY slots).
 *
 * Storage: flat array of NANOJS8_MAILBOX_MAX entries in static BSS
 * (~2.4 KB), backed by a single NVS blob in namespace "nj8_inbox".
 * Every state change writes the whole blob — simple and atomic. With
 * ~10 ops/day and ESP-IDF NVS wear leveling, internal-flash endurance
 * is comfortably in the decades.
 *
 * Thread safety: a FreeRTOS mutex protects the array. NVS commits run
 * inside the lock — slow (1-3 ms per write) but the mailbox isn't on
 * any hot path so this is acceptable.
 *
 * License: GPL-3.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Sizing ───────────────────────────────────────────────────────────

// Max entries held in memory and on flash. 16 slots = 2304 B static
// BSS + ~2316 B NVS blob. Sized for a busy emergency-relay shift.
#define NANOJS8_MAILBOX_MAX        16

// JS8 callsigns are at most 11 chars (including /P, /M suffixes); 16
// is plenty including NUL. Also covers group addresses like @ALLCALL.
#define NANOJS8_MAILBOX_CALL_LEN   16

// Body length matches MicroJS8's mailbox model. 100 chars = ~7 JS8
// frames; longer messages are split at COMPOSE time.
#define NANOJS8_MAILBOX_BODY_LEN  100

// Sentinel value for snr_db when SNR doesn't apply (STORE entries
// were never decoded by us — the originator transmitted them to us
// in a single MSG TO: frame whose SNR we DO know, but we record it
// against the STORE entry as the originator-link SNR, and use this
// sentinel when SNR is genuinely unknown). 127 is outside any
// physically meaningful JS8 SNR range (max ~+30 dB), so unambiguous.
#define NANOJS8_MAILBOX_SNR_NA    127

// ── Types ────────────────────────────────────────────────────────────

// Entry lifecycle state. Values are persisted in NVS — DO NOT
// renumber existing entries; add new ones at the end.
typedef enum {
    NANOJS8_MAILBOX_TYPE_EMPTY     = 0,
    NANOJS8_MAILBOX_TYPE_UNREAD    = 1,
    NANOJS8_MAILBOX_TYPE_READ      = 2,
    NANOJS8_MAILBOX_TYPE_STORE     = 3,
    NANOJS8_MAILBOX_TYPE_DELIVERED = 4,
} nanojs8_mailbox_type_t;

// One mailbox entry. Field order is chosen so the struct lays out
// without padding on ESP32-S3 (natural alignment for u32/u16). Total
// size: 4 + 4 + 2 + 1 + 1 + 16 + 16 + 100 = 144 bytes.
//
// CRITICAL: this struct is serialised verbatim into the NVS blob. If
// the layout ever changes (field added/removed/reordered/resized),
// bump NANOJS8_MAILBOX_BLOB_VERSION in mailbox.c so older blobs are
// rejected and the operator starts fresh rather than crashing on a
// stale read.
typedef struct {
    uint32_t  utc_seconds_today;                      // 0..3   seconds-of-day at create-time (display only; rolls at midnight)
    uint32_t  freq_hz;                                // 4..7   audio Hz at decode-time (or 0 for STORE/COMPOSE)
    uint16_t  id;                                     // 8..9   stable per-entry identifier; QUERY MSG <id> uses this
    uint8_t   type;                                   // 10     nanojs8_mailbox_type_t
    int8_t    snr_db;                                 // 11     RX SNR for UNREAD; SNR_NA for STORE
    char      from_call[NANOJS8_MAILBOX_CALL_LEN];    // 12..27
    char      to_call  [NANOJS8_MAILBOX_CALL_LEN];    // 28..43
    char      body     [NANOJS8_MAILBOX_BODY_LEN];    // 44..143
} nanojs8_mailbox_entry_t;

// ── Lifecycle ────────────────────────────────────────────────────────

// Initialize the mailbox. Idempotent. Loads any persisted entries
// from NVS namespace "nj8_inbox" into the in-memory array. If no
// prior data exists (first boot after firmware update) or the blob
// is corrupt / wrong version, the in-memory array starts empty —
// no error returned for that case, just an INFO/WARN log.
//
// Returns ESP_OK on success; ESP_FAIL on mutex-create or NVS-open
// failure (rare; out-of-memory or partition-corruption territory).
esp_err_t nanojs8_mailbox_init(void);

// ── Insertion ────────────────────────────────────────────────────────

// Add an UNREAD entry — we received a MSG addressed to us. Allocates
// the next id, stamps utc_seconds_today from nanojs8_time, and
// persists. Uppercases callsigns on copy.
//
// If the ring is full, the eviction policy runs:
//   DELIVERED → READ → STORE → UNREAD (oldest-first within each tier).
//
// Returns ESP_OK on success; ESP_ERR_INVALID_ARG for empty
// from_call / body; ESP_ERR_INVALID_STATE if init() hasn't run;
// ESP_FAIL on persist failure (entry is still added in-memory; the
// next successful save will pick it up).
esp_err_t nanojs8_mailbox_add_unread(const char *from_call,
                                      const char *to_call,
                                      const char *body,
                                      uint32_t    freq_hz,
                                      int8_t      snr_db);

// Add a STORE entry — someone asked us to hold this message for
// to_call. Same allocation/eviction rules as add_unread, but snr_db
// is set to SNR_NA and freq_hz to 0.
esp_err_t nanojs8_mailbox_add_store(const char *from_call,
                                     const char *to_call,
                                     const char *body);

// ── State transitions ───────────────────────────────────────────────

// Change an entry's type. mark_read promotes UNREAD → READ; mark_
// delivered promotes STORE → DELIVERED. Persists.
//
// Returns ESP_OK; ESP_ERR_NOT_FOUND if no entry with that id exists;
// ESP_ERR_INVALID_STATE if the current type doesn't permit the
// transition (e.g. mark_read on a STORE entry — no-op, returns
// ESP_ERR_INVALID_STATE rather than silently doing the wrong thing).
esp_err_t nanojs8_mailbox_mark_read(uint16_t id);
esp_err_t nanojs8_mailbox_mark_delivered(uint16_t id);

// Delete an entry outright. Sets the slot to EMPTY and persists.
// Used by the INBOX UI's Del key.
esp_err_t nanojs8_mailbox_delete(uint16_t id);

// ── Queries ──────────────────────────────────────────────────────────

// Look up by id. Returns true and copies into *out if found; false
// (out untouched) if no such id. The returned copy is safe to use
// after the call without holding any lock.
bool nanojs8_mailbox_find_by_id(uint16_t                  id,
                                 nanojs8_mailbox_entry_t *out);

// Find all STORE entries addressed to to_call. Copies up to max
// entries into out[], OLDEST-first (by ascending id). Returns the
// number copied. Used by L7.11g.6's QUERY MSGS handler — the JS8
// mailbox protocol delivers oldest stored messages first (FIFO).
uint32_t nanojs8_mailbox_find_holding_for(const char              *to_call,
                                           nanojs8_mailbox_entry_t *out,
                                           uint32_t                 max);

// Total count of non-EMPTY entries. Cheap; no mutex (atomic read of
// cached value — racy by at most ±1, fine for UI badge).
uint32_t nanojs8_mailbox_count(void);

// Count of UNREAD entries specifically. Same caveat as count(). Used
// by the INBOX badge ("3 unread / 12 total").
uint32_t nanojs8_mailbox_count_unread(void);

// Copy up to `max` entries into out[], newest-first by id. Skips
// EMPTY slots. Returns the number copied. Snapshot is consistent
// (taken under the lock) but stale by the time the caller uses it.
uint32_t nanojs8_mailbox_snapshot(nanojs8_mailbox_entry_t *out,
                                   uint32_t                 max);

#ifdef __cplusplus
}
#endif
