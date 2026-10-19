#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "save_state_file.hpp"

bool initialize_audio();
void shutdown_audio();
void queue_audio_samples(int16_t* samples, size_t sample_count, double volume);
namespace ultramodern { struct audio_queue_state_t; enum class AudioActivity; }
ultramodern::audio_queue_state_t audio_queue_state();
void set_audio_activity(ultramodern::AudioActivity activity);
void set_frequency(uint32_t frequency);

namespace jfc::audio {
struct OutputDevices {
    uint64_t revision = 0;
    std::vector<std::string> names;
    std::string selected, active, error;
    bool fallback = false;
};
uint64_t output_revision();
OutputDevices output_devices();
bool select_output(const std::string& name);
void refresh_outputs();
void test_output(double volume);
}

namespace jfc::state {

class AudioPlaybackPause {
public:
    AudioPlaybackPause();
    ~AudioPlaybackPause();
    AudioPlaybackPause(const AudioPlaybackPause&) = delete;
    AudioPlaybackPause& operator=(const AudioPlaybackPause&) = delete;
};

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
