#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace jfc::display_options {

constexpr float scale_far_plane(float original, int percent) {
    return original * static_cast<float>(std::clamp(percent, 100, 400)) / 100.0f;
}

inline int16_t blend_fog(int16_t original, int16_t disabled, int strength) {
    const int clamped = std::clamp(strength, 0, 100);
    return static_cast<int16_t>(std::lround(
        disabled + (original - disabled) * (clamped / 100.0)));
}

struct FogRange {
    int16_t near;
    int16_t far;
};

inline FogRange adjust_fog(int16_t near, int16_t far, int strength) {
    FogRange result{
        blend_fog(near, 1018, strength),
        blend_fog(far, 1023, strength),
    };
    result.far = std::clamp<int16_t>(result.far, 5, 1023);
    result.near = std::clamp<int16_t>(result.near, 0, result.far - 5);
    return result;
}

}
