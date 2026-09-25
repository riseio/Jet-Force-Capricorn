#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include "save_state_file.hpp"

bool initialize_audio();
void shutdown_audio();
void queue_audio_samples(int16_t* samples, size_t sample_count, double volume);
size_t frames_remaining();
void set_frequency(uint32_t frequency);

namespace jfc::state {

class AudioPause {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    AudioPause();
    ~AudioPause();
    Bytes capture() const;
    void prepare(std::span<const uint8_t> saved);
    void commit() noexcept;
};
}
