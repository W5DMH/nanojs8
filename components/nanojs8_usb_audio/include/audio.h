/*
 * audio.h — NanoJS8 v0.7 USB audio subsystem (Layer 5)
 * ======================================================
 * Drives a USB Audio Class 1.0 device connected to the T-Deck via the
 * Y-cable + external charger. Discovers the device's supported sample
 * rates / channels / bit resolution at enumeration and picks the best
 * fit for JS8 audio I/O.
 *
 * Production target (Layer 5b): DigiRig Mobile, which contains a USB
 * hub bridging a CM108-based audio codec and a CP2102 serial bridge.
 * The CM108 is functionally identical to the CM119 we used for the
 * sustain test — both expose 48 kHz/44.1 kHz at 16-bit, with stereo
 * TX only and mono RX only. The discovery logic picks the highest
 * preferred rate; channel count is dictated by what the device offers.
 *
 * Also works with bare CM108/CM119 adapters (Syba etc.) for development.
 *
 * Hardware path:
 *   T-Deck USB-C ── Y-cable ─┬── External USB-C charger (power)
 *                            └── DigiRig Mobile (data)
 *                                ├── internal USB2412 hub
 *                                ├── CM108 audio  (handled by THIS file)
 *                                └── CP2102 serial (see usb_serial.h)
 *
 * Why PERIODIC_OUT FIFO bias is required:
 *   The CM108/CM119 family declares wMaxPacketSize=200 for its stereo TX
 *   endpoint (48 kHz × 2 ch × 2 bytes = 192 B/ms + protocol overhead).
 *   With the default BALANCED bias, the ESP32-S3 USB host periodic-OUT
 *   FIFO caps at 128 bytes/microframe and rejects the endpoint. The
 *   PERIODIC_OUT bias raises this cap enough to accept 200-byte EPs.
 *   See sdkconfig.defaults for the full explanation.
 *
 * Threading model:
 *   - USB Host library task (priority 5) drives the host stack
 *   - UAC driver task (priority 5) handles class events
 *   - Internal event dispatcher task drains the event queue and opens devices
 *   - Callers read/write from any task via the public API
 *
 * Device semantics:
 *   - A physical audio device with both mic AND speaker is treated by the
 *     UAC driver as TWO logical devices: one for RX (microphone) and one
 *     for TX (speaker). We track each handle separately.
 *   - Audio frames are interleaved PCM at the chosen sample rate. Read/write
 *     work in bytes; convert to samples using the per-frame byte size
 *     (channels × bytes_per_sample).
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

// Preferred sample rates for JS8 audio, in order of preference.
// JS8's modem traditionally runs at 12000 Hz, but the CM108/CM119 family
// only supports 44100 / 48000 natively. We pick the highest supported rate
// in this list, then downsample in software if the modem needs 12 kHz.
#define NANOJS8_AUDIO_PREFERRED_RATES { 48000, 44100, 32000, 16000, 8000 }

// Layer 5 fixed parameters. The modem will set its own preferences in
// Layer 6+; for now we negotiate the most compatible fit.
#define NANOJS8_AUDIO_BITS         16
#define NANOJS8_AUDIO_CHANNELS_RX  1   // try mono first; fall back to stereo
#define NANOJS8_AUDIO_CHANNELS_TX  1

// Status of an audio stream direction. Use nanojs8_audio_rx_status() and
// nanojs8_audio_tx_status() to query.
typedef enum {
    NANOJS8_AUDIO_STATUS_UNAVAILABLE = 0,  // No device connected for this direction
    NANOJS8_AUDIO_STATUS_WAITING,          // Device opened but stream not yet started
    NANOJS8_AUDIO_STATUS_READY,            // Stream active, read/write available
    NANOJS8_AUDIO_STATUS_ERROR,            // Persistent error; subsystem will retry
} nanojs8_audio_status_t;

// Information about an active stream. Populated once a device is open and
// streaming; fields are zero / NULL before that.
typedef struct {
    nanojs8_audio_status_t status;
    uint32_t sample_rate;      // Hz (e.g. 48000)
    uint8_t  channels;         // 1 = mono, 2 = stereo
    uint8_t  bit_resolution;   // 16 for our purposes
    uint16_t vid;              // Vendor ID
    uint16_t pid;              // Product ID
} nanojs8_audio_stream_info_t;

// Start the USB audio subsystem. Installs the USB Host library and the UAC
// class driver, spawns background tasks. Returns ESP_OK once the install
// completes; actual device discovery happens asynchronously as devices
// are plugged in.
//
// Idempotent — safe to call multiple times. The Y-cable + external charger
// is REQUIRED to power both the T-Deck and the audio adapter; without it,
// the CM119 won't enumerate.
esp_err_t nanojs8_audio_start(void);

// Query the current status / parameters of each stream direction. Either
// info pointer may be NULL if the caller only wants one. Thread-safe.
void nanojs8_audio_rx_info(nanojs8_audio_stream_info_t *info);
void nanojs8_audio_tx_info(nanojs8_audio_stream_info_t *info);

// Read up to `size` bytes of captured audio (microphone) into `data`.
// Blocks up to `timeout_ms` for data to arrive. Returns the actual bytes
// read in `*bytes_read`. Returns ESP_OK on success, ESP_ERR_INVALID_STATE
// if the RX stream isn't ready, ESP_ERR_TIMEOUT if no data within timeout.
//
// Sample format: interleaved PCM at the rate / channels / bit resolution
// reported by nanojs8_audio_rx_info(). At 48 kHz / 1 channel / 16-bit,
// each second of audio is 96000 bytes (48000 samples × 2 bytes).
esp_err_t nanojs8_audio_read(uint8_t *data, uint32_t size,
                              uint32_t *bytes_read, uint32_t timeout_ms);

// Write `size` bytes of PCM audio (speaker) from `data`. Non-blocking —
// data is copied into an internal ring buffer and transmitted by the USB
// driver asynchronously. Returns ESP_OK if queued, ESP_ERR_INVALID_STATE
// if the TX stream isn't ready, ESP_FAIL if the ring buffer is full and
// the call cannot enqueue more data.
//
// Sample format: same as read — interleaved PCM at the negotiated rate.
esp_err_t nanojs8_audio_write(const uint8_t *data, uint32_t size,
                               uint32_t timeout_ms);

// Total samples received on the RX stream since boot. Useful for the
// status display and as a liveness signal.
uint64_t nanojs8_audio_rx_samples_total(void);

// Total samples transmitted on the TX stream since boot.
uint64_t nanojs8_audio_tx_samples_total(void);

// Peak absolute sample value in the most recent RX read (16-bit signed,
// range 0..32767). Updated by the loopback path. Useful for a level meter.
uint16_t nanojs8_audio_rx_peak(void);

#ifdef __cplusplus
}
#endif
