/*
 * usb_serial.h — NanoJS8 v0.7 USB serial subsystem (Layer 5b)
 * ============================================================
 * Drives a CP2102-based USB-to-UART bridge connected via the same physical
 * USB-C port as the audio adapter. For the DigiRig Mobile, this is the
 * CP2102 that sits behind the internal USB hub alongside the CM108 audio
 * codec — the two devices share one USB-C cable to the T-Deck.
 *
 * Hardware path:
 *   T-Deck USB-C ── Y-cable ─┬── External USB-C charger (power)
 *                            └── DigiRig Mobile (data)
 *                                ├── internal USB2412 hub
 *                                ├── CM108 audio  (see audio.h)
 *                                └── CP2102 serial (handled HERE)
 *
 * What this component does:
 *   - Registers the CP210x VCP driver with the cdc_acm_host stack
 *   - Auto-opens the CP2102 when it enumerates through the hub
 *   - Provides line-coding control (baud rate, parity, stop bits)
 *   - Exposes byte-stream write and a callback-based RX path
 *   - Provides DTR/RTS control for hardware PTT
 *   - Recovers automatically when the device is unplugged and reconnected
 *
 * What this component does NOT do:
 *   - No CAT protocol parsing (radio-specific; lives in Layer 6+)
 *   - No PTT timing/holdoff state machine (lives in the modem layer)
 *   - No multi-port support — one CP2102 at a time
 *
 * RX architecture — callback model:
 *   CAT replies from a radio are asynchronous and event-driven. Polling
 *   from the main loop adds latency and complicates buffering. Instead,
 *   a single RX callback is registered via nanojs8_serial_set_rx_callback().
 *   The CDC-ACM driver invokes it from its own internal task whenever
 *   bytes arrive. The callback runs OFF the main loop; consumers MUST be
 *   thread-safe (use queues or atomics if state is shared).
 *
 * PTT line semantics on the DigiRig Mobile:
 *   - The DigiRig has TWO PTT paths:
 *     1. Hardware PTT keyed off the audio Right channel (VOX-style, for
 *        rigs like (tr)uSDX that lack CAT). This is wholly independent
 *        of the serial port — handled by the audio adapter's hardware.
 *     2. CAT/serial PTT via the CP2102's hardware control lines. Most
 *        modern HF rigs (Icom, Yaesu, Elecraft, Xiegu) use this.
 *   - For (2), the radio expects either RTS or DTR to assert during TX,
 *     depending on the CAT software configuration. Our default is RTS;
 *     change with nanojs8_serial_ptt_line_set().
 *
 * License: GPL-3.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Default baud rate at startup. 9600 8N1 is the most common ham CAT setting
// (Icom CI-V default, Yaesu's default for many rigs). Most other radios at
// 4800, 19200, 38400, 57600, or 115200 can be switched after start via
// nanojs8_serial_set_line().
#define NANOJS8_SERIAL_DEFAULT_BAUD 9600

// Internal CDC-ACM ring buffer sizes. 256 bytes per direction is plenty for
// CAT traffic (commands are typically 5-20 bytes, replies under 50).
#define NANOJS8_SERIAL_TX_BUFFER 256
#define NANOJS8_SERIAL_RX_BUFFER 256

// Connection timeout when waiting for the device to enumerate after install.
// Long enough to cover the DigiRig's hub-enumeration delay (~1.4s in our
// probe), short enough that we re-try quickly if the device isn't present.
#define NANOJS8_SERIAL_CONN_TIMEOUT_MS 5000

// Stop bits values for nanojs8_serial_set_line().
// Mirrors the underlying cdc_acm_line_coding_t.bCharFormat encoding.
#define NANOJS8_SERIAL_STOP_1   0
#define NANOJS8_SERIAL_STOP_1_5 1
#define NANOJS8_SERIAL_STOP_2   2

// Parity values for nanojs8_serial_set_line().
// Mirrors the underlying cdc_acm_line_coding_t.bParityType encoding.
#define NANOJS8_SERIAL_PARITY_NONE  0
#define NANOJS8_SERIAL_PARITY_ODD   1
#define NANOJS8_SERIAL_PARITY_EVEN  2
#define NANOJS8_SERIAL_PARITY_MARK  3
#define NANOJS8_SERIAL_PARITY_SPACE 4

// Hardware line for PTT signaling. Default is RTS per the v0.7 spec.
typedef enum {
    NANOJS8_SERIAL_PTT_RTS = 0,
    NANOJS8_SERIAL_PTT_DTR = 1,
} nanojs8_serial_ptt_line_t;

// Subsystem state. The status reflects the CP2102 specifically — the
// audio side has its own status (see audio.h).
typedef enum {
    NANOJS8_SERIAL_STATUS_UNAVAILABLE = 0,  // No CP2102 connected
    NANOJS8_SERIAL_STATUS_OPENING,           // VCP::open() in flight
    NANOJS8_SERIAL_STATUS_READY,             // Device open, line coding set
    NANOJS8_SERIAL_STATUS_ERROR,             // Error; subsystem will retry
} nanojs8_serial_status_t;

// Information about the currently-open device. Fields are zero when no
// device is connected. Thread-safe to query at any time.
typedef struct {
    nanojs8_serial_status_t status;
    uint32_t baud_rate;
    uint8_t  data_bits;
    uint8_t  stop_bits;     // NANOJS8_SERIAL_STOP_*
    uint8_t  parity;        // NANOJS8_SERIAL_PARITY_*
    bool     dtr_active;
    bool     rts_active;
    nanojs8_serial_ptt_line_t ptt_line;
    uint16_t vid;
    uint16_t pid;
} nanojs8_serial_info_t;

// Callback type for incoming bytes. Returns true if the bytes have been
// consumed (no further processing by the driver); false if the driver
// should treat them as unconsumed. For nearly all CAT use cases, return
// true. `arg` is the user pointer passed to nanojs8_serial_set_rx_callback.
//
// IMPORTANT: this callback fires from the CDC-ACM driver's internal task,
// NOT the main loop. Do not call long-running or blocking functions here.
// If your handler needs to do nontrivial work, push the bytes into a
// FreeRTOS queue and process them in your own task.
typedef bool (*nanojs8_serial_rx_cb_t)(const uint8_t *data, size_t len,
                                        void *arg);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Initialize the USB serial subsystem. Installs the CDC-ACM host driver,
// registers the CP210x VCP driver with the VCP service, and spawns a
// worker task that polls VCP::open() until a CP2102 is found.
//
// MUST be called AFTER nanojs8_audio_start() — both subsystems share the
// USB host library, and audio.cpp is the one that installs it.
//
// Idempotent. Returns ESP_OK on success or if already started.
esp_err_t nanojs8_serial_start(void);

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// Populate `info` with the current state. Safe to call before/after start
// and whether or not a device is connected.
void nanojs8_serial_get_info(nanojs8_serial_info_t *info);

// Total bytes transmitted since boot (across reconnects).
uint64_t nanojs8_serial_tx_bytes_total(void);

// Total bytes received since boot (across reconnects).
uint64_t nanojs8_serial_rx_bytes_total(void);

// ---------------------------------------------------------------------------
// Line configuration
// ---------------------------------------------------------------------------

// Change baud rate. Common values: 4800, 9600, 19200, 38400, 57600, 115200.
// Returns ESP_OK on success, ESP_ERR_INVALID_STATE if not connected.
// Safe to call multiple times — the change takes effect immediately.
esp_err_t nanojs8_serial_set_baud(uint32_t baud);

// Set the full line coding in one call. Use the NANOJS8_SERIAL_STOP_*
// and NANOJS8_SERIAL_PARITY_* macros for clarity.
esp_err_t nanojs8_serial_set_line(uint32_t baud, uint8_t data_bits,
                                   uint8_t parity, uint8_t stop_bits);

// ---------------------------------------------------------------------------
// Data I/O
// ---------------------------------------------------------------------------

// Write `len` bytes to the serial port. Blocks up to `timeout_ms` for
// the bytes to flush from the host-side ring buffer. Returns ESP_OK on
// success, ESP_ERR_INVALID_STATE if not connected, ESP_ERR_TIMEOUT on
// timeout.
esp_err_t nanojs8_serial_write(const uint8_t *data, size_t len,
                                uint32_t timeout_ms);

// Register a callback to be invoked when bytes arrive from the radio.
// Pass NULL to unregister. See the note above about thread context.
// `arg` is passed through to the callback unmodified.
esp_err_t nanojs8_serial_set_rx_callback(nanojs8_serial_rx_cb_t cb,
                                          void *arg);

// ---------------------------------------------------------------------------
// Hardware control lines (DTR, RTS, PTT)
// ---------------------------------------------------------------------------

// Set DTR line state. Returns ESP_OK on success, ESP_ERR_INVALID_STATE
// if not connected.
esp_err_t nanojs8_serial_set_dtr(bool active);

// Set RTS line state.
esp_err_t nanojs8_serial_set_rts(bool active);

// Select which line PTT operates on. Default is RTS. The change takes
// effect on the NEXT call to nanojs8_serial_ptt_set(); existing line
// states are not modified.
void nanojs8_serial_ptt_line_set(nanojs8_serial_ptt_line_t line);

// Toggle PTT. Internally asserts/deasserts the line selected via
// nanojs8_serial_ptt_line_set(). Returns ESP_OK on success.
esp_err_t nanojs8_serial_ptt_set(bool transmitting);

#ifdef __cplusplus
}
#endif
