// NanoJS8 — CAT (Computer Aided Transceiver) control for (tr)uSDX
//
// Implements the small subset of the Kenwood TS-480 CAT protocol that
// the (tr)uSDX emulates, over its single CH340 CDC serial port.
//
// Phase 3b-step-1 scope (CAT control only, no audio streaming):
//   - PTT:        TX0; (transmit) / RX; (receive)        [the must-have]
//   - Frequency:  FA;  query -> 11-digit Hz              [HOME display]
//   - Mode:       MD;  query -> 1..5 (LSB/USB/CW/FM/AM)  [HOME display]
//   - ID:         ID;  -> "020" handshake at connect     [sanity check]
//
// Audio-over-CAT (UA1;/US;) is deferred to Phase 3b-step-2.
//
// Protocol notes (from the (tr)uSDX CAT spec, firmware 2.00t+):
//   - 38400 or 115200 baud, 8N1, no flow control.
//   - DTR should be HIGH, RTS LOW on RX (RTS may be HIGH to key PTT,
//     but we use CAT TX;/RX; for PTT, not the RTS line).
//   - Commands are ASCII, terminated by ';'. Responses likewise.
//   - FA returns "FA" + 11 ASCII digits (Hz) + ";".
//   - IF returns a fixed-width status string (freq + mode among others).
//   - MD returns "MD" + one digit + ";".
//   - ID returns "ID020;" for the TS-480 emulation.
//
// Design: this module wraps a CdcAcmDevice* (opened via the VCP umbrella
// by cat_ptt.cpp / the radio service). It is NOT thread-safe by itself;
// the radio service serializes access from a single CAT task.

#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_err.h"
#include "usb/cdc_host_types.h"   // for cdc_acm_data_callback_t

namespace nanojs8 {
namespace radio {
namespace cat {

// Decoded radio mode (TS-480 MD values 1..5).
enum class Mode : uint8_t {
    UNKNOWN = 0,
    LSB     = 1,
    USB     = 2,
    CW      = 3,
    FM      = 4,
    AM      = 5,
};

const char* mode_name(Mode m);

// Snapshot of CAT-derived radio state, refreshed by the slow poll.
struct CatState {
    bool     valid;          // true once we've had at least one good IF/FA reply
    uint64_t freq_hz;        // last known VFO-A frequency, Hz
    Mode     mode;           // last known mode
    bool     transmitting;   // our last commanded PTT state
};

// ---------------------------------------------------------------------------
// Opaque handle. The radio service owns one CAT session bound to the
// (tr)uSDX's CDC device. We keep the device pointer type-erased here so
// this header doesn't drag in the cdc_acm headers; the .cpp casts it.
// ---------------------------------------------------------------------------
typedef void* CatDeviceHandle;   // really a CdcAcmDevice*

// Bind a CAT session to an already-open CDC device. Sends ID; to verify
// the (tr)uSDX responds with the expected TS-480 id (020). Returns
// ESP_OK on a good handshake, ESP_ERR_INVALID_RESPONSE if the id is
// wrong/absent (caller may choose to proceed anyway), or a transport
// error.
esp_err_t bind(CatDeviceHandle dev);

// Release the CAT session (does not close the underlying CDC device;
// the radio service owns that lifecycle).
void unbind(void);

// PTT control via CAT. tx=true sends TX0; (transmit), tx=false sends RX;.
esp_err_t set_ptt(bool tx);

// Poll the radio for current frequency + mode via IF; and update the
// internal CatState. Called on the slow (~2 s) cadence by the radio
// service. Cheap at 115200 baud. Returns ESP_OK if a parseable reply
// arrived.
esp_err_t poll_status(void);

// Lock-free read of the latest CAT state (safe from the UI task for the
// HOME frequency/mode display).
void get_state(CatState* out);

// Returns the CDC RX data callback this module uses to accumulate CAT
// reply bytes. The radio service wires this into the Ch34x device config
// (dev_cfg.data_cb) when it opens the (tr)uSDX port, so replies flow into
// the CAT parser.
cdc_acm_data_callback_t get_rx_callback(void);

} // namespace cat
} // namespace radio
} // namespace nanojs8
