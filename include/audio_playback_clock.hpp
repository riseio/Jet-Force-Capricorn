#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace jfc::audio {

class PlaybackClock {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr uint64_t output_rate = 48000;
    explicit PlaybackClock(Time now = Clock::now()) : anchor(now) {}
    void set_paused(Time now, bool value) {
        if (paused == value) return;
        elapsed = current(now); anchor = now; paused = value;
    }
    void reset_queue(Time now) { accounted = current(now); fractional = 0; }
    uint64_t advance(Time now, bool consume) {
        const uint64_t total = current(now), delta = total - accounted;
        accounted = total;
        if (!consume) { fractional = 0; return 0; }
        const uint64_t remainder = (delta % 1000000000U) * output_rate + fractional;
        const uint64_t frames = (delta / 1000000000U) * output_rate + remainder / 1000000000U;
        fractional = remainder % 1000000000U;
        return frames;
    }
private:
    Time anchor;
    uint64_t elapsed = 0, accounted = 0, fractional = 0;
    bool paused = false;
    uint64_t current(Time now) const {
        if (now < anchor) throw std::logic_error("Audio playback clock moved backwards");
        const auto value = elapsed + (paused ? 0 : uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - anchor).count()));
        if (value < accounted) throw std::logic_error("Audio playback clock moved backwards");
        return value;
    }
};
}
