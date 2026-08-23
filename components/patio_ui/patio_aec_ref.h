#pragma once

#include "esphome/core/defines.h"

#ifdef PATIO_AEC

#include <cstddef>
#include <cstdint>

namespace esphome::patio_ui {

// Far-end reference channel for acoustic echo cancellation (AEC).
//
// Single-producer / single-consumer: the AW88298 speaker's drain task pushes a
// copy of every PCM chunk it hands to the codec (aec_ref_push); the ES7210
// mic's AEC task pulls the matching number of samples to feed esp-sr's AEC as
// the "what the speaker is playing" reference (aec_ref_pull). When the speaker
// is idle the ring simply drains and the mic pulls silence, so AEC becomes a
// passthrough. Both run on core 1 (different tasks); the backing FreeRTOS byte
// ring buffer is SPSC-safe. Mono 16 kHz 16-bit throughout.

// Push `samples` mono int16 samples of speaker PCM (non-blocking; drops on the
// rare full-buffer case, which cannot happen during steady playback since the
// mic drains at the same I2S clock rate).
void aec_ref_push(const int16_t *data, size_t samples);

// Pull `samples` mono int16 reference samples into `out`, zero-filling any
// shortfall (speaker idle / brief underrun). Returns the count of REAL
// (non-zero-filled) samples, for underrun diagnostics.
size_t aec_ref_pull(int16_t *out, size_t samples);

}  // namespace esphome::patio_ui

#endif  // PATIO_AEC
