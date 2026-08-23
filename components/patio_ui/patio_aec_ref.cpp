#include "esphome/core/defines.h"

#include "patio_aec_ref.h"

#ifdef PATIO_AEC

#include "esphome/components/ring_buffer/ring_buffer.h"

#include <cstring>
#include <memory>

namespace esphome::patio_ui {

// ~0.5 s of 16 kHz mono 16-bit reference audio. Large enough to absorb the
// phase difference between the speaker's write chunks and the mic's read chunks
// without underrunning mid-playback. Allocated PSRAM-first (RingBuffer default)
// so it does not eat scarce internal RAM.
static const size_t REF_RING_BYTES = 16000;

static std::unique_ptr<ring_buffer::RingBuffer> g_ref_ring;

static ring_buffer::RingBuffer *ref_ring() {
  if (g_ref_ring == nullptr) {
    g_ref_ring = ring_buffer::RingBuffer::create(REF_RING_BYTES);
  }
  return g_ref_ring.get();
}

void aec_ref_push(const int16_t *data, size_t samples) {
  ring_buffer::RingBuffer *r = ref_ring();
  if (r == nullptr)
    return;
  r->write_without_replacement((const void *) data, samples * sizeof(int16_t), 0);
}

size_t aec_ref_pull(int16_t *out, size_t samples) {
  const size_t want = samples * sizeof(int16_t);
  size_t got = 0;
  ring_buffer::RingBuffer *r = ref_ring();
  if (r != nullptr)
    got = r->read((void *) out, want, 0);
  if (got < want)
    std::memset(((uint8_t *) out) + got, 0, want - got);
  return got / sizeof(int16_t);
}

}  // namespace esphome::patio_ui

#endif  // PATIO_AEC
