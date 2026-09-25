#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace jfc::modern_input {

constexpr float stick_range = 80.0f;
constexpr float native_manual_aim_deadzone = 40.0f;
constexpr float manual_aim_center_to_edge_vi = 45.0f;
constexpr float manual_aim_max_elapsed_vi = 8.0f;
constexpr float mouse_scale = 2.0f;
constexpr float stick_yaw_degrees_per_second = 240.0f;
constexpr float stick_pitch_degrees_per_second = 180.0f;
constexpr float mouse_degrees_per_composed_unit = 0.075f;

constexpr int32_t direct_pitch_limit = 0x3F00;
constexpr float acceleration_rate = 0.04f;
constexpr float acceleration_limit = 2.0f;
constexpr int32_t movement_button_threshold = 8;
constexpr uint16_t a = 0x8000;
constexpr uint16_t b = 0x4000;

constexpr uint16_t interact = 0x0040;
constexpr uint16_t c_right = 0x0001;
constexpr uint16_t c_left = 0x0002;
constexpr uint16_t c_down = 0x0004;
constexpr uint16_t c_up = 0x0008;

struct Aim {
    float x;
    float y;
};

struct ManualAimBounds {
    int32_t horizontal;
    int32_t vertical;
};

struct GameplayAxes {
    int32_t movement_x;
    int32_t movement_raw_x;
    int32_t movement_y;
    int32_t movement_raw_y;
    int32_t control_x;
    int32_t control_raw_x;
    int32_t control_y;
    int32_t control_raw_y;
    int32_t isolated_l2_horizontal_look;
    int32_t isolated_l2_vertical_look;
    uint16_t native_movement_buttons;
    uint16_t isolated_l2_movement_buttons;
    bool apply_isolated_l2_movement;
};

inline float from_sdl_vertical(float axis, bool inverted = false) {

    return inverted ? -axis : axis;
}

inline float mouse_gain(float x, float y, bool acceleration) {
    if (!acceleration) {
        return mouse_scale;
    }
    return mouse_scale * (1.0f + std::min(
        std::hypot(x, y) * acceleration_rate,
        acceleration_limit));
}

inline Aim compose(
    float stick_x,
    float stick_y,
    float mouse_x,
    float mouse_y,
    bool acceleration
) {
    const float gain = mouse_gain(mouse_x, mouse_y, acceleration);
    return {
        std::clamp(stick_x * stick_range + mouse_x * gain,
                   -stick_range, stick_range),
        std::clamp(stick_y * stick_range + mouse_y * gain,
                   -stick_range, stick_range),
    };
}

inline int32_t manual_aim_axis(float axis) {
    if (!std::isfinite(axis)) {
        return 0;
    }
    const float magnitude = std::clamp(std::abs(axis), 0.0f, stick_range);
    if (magnitude == 0.0f) {
        return 0;
    }

    const float normalized = magnitude / stick_range;
    const float active_range = stick_range - native_manual_aim_deadzone;
    const int32_t mapped = static_cast<int32_t>(native_manual_aim_deadzone) +
        std::max(1, static_cast<int32_t>(std::ceil(
            normalized * normalized * active_range)));
    return std::signbit(axis) ? -mapped : mapped;
}

inline ManualAimBounds visible_manual_aim_bounds(
    int32_t native_horizontal_bound,
    int32_t native_vertical_bound,
    float output_aspect_ratio,
    float vertical_fov_degrees
) {
    constexpr double pi = 3.14159265358979323846;
    constexpr double angle_units_per_radian = 65536.0 / (2.0 * pi);
    if (native_horizontal_bound <= 0 || native_vertical_bound <= 0 ||
        !std::isfinite(output_aspect_ratio) || output_aspect_ratio <= 0.0f ||
        !std::isfinite(vertical_fov_degrees) || vertical_fov_degrees <= 0.0f) {
        return {native_horizontal_bound, native_vertical_bound};
    }

    const double half_vertical = std::clamp(
        static_cast<double>(vertical_fov_degrees), 1.0, 90.0) * pi / 360.0;
    const double aspect = std::clamp(
        static_cast<double>(output_aspect_ratio), 1.0, 32.0 / 9.0);
    const double half_horizontal = std::atan(std::tan(half_vertical) * aspect);
    return {
        static_cast<int32_t>(std::lround(
            half_horizontal * angle_units_per_radian)),
        static_cast<int32_t>(std::lround(
            half_vertical * angle_units_per_radian)),
    };
}

inline int32_t manual_aim_target_step_limit(
    int32_t visible_half_angle,
    float composed_axis,
    float elapsed_vi,
    double sensitivity_scale
) {
    if (visible_half_angle <= 0 || !std::isfinite(composed_axis) ||
        !std::isfinite(elapsed_vi) || elapsed_vi <= 0.0f ||
        !std::isfinite(sensitivity_scale) || sensitivity_scale <= 0.0) {
        return 0;
    }

    const double magnitude = std::clamp(
        static_cast<double>(std::abs(composed_axis) / stick_range), 0.0, 1.0);
    if (magnitude == 0.0) {
        return 0;
    }
    const double ticks = std::clamp(
        static_cast<double>(elapsed_vi), 0.0,
        static_cast<double>(manual_aim_max_elapsed_vi));
    const double step = static_cast<double>(visible_half_angle) * magnitude *
        ticks * sensitivity_scale /
        static_cast<double>(manual_aim_center_to_edge_vi);
    return std::max(1, static_cast<int32_t>(std::ceil(std::min(
        step, static_cast<double>(std::numeric_limits<int32_t>::max())))));
}

inline double scoped_look_scale(bool active, float zoom) {
    return active && std::isfinite(zoom) && zoom >= 1.0f && zoom <= 5.0f
        ? 1.0 / static_cast<double>(zoom) : 1.0;
}

inline int32_t direct_look_angle_delta(
    float stick_axis,
    float composed_mouse_delta,
    float elapsed_vi,
    double sensitivity_scale,
    bool vertical
) {
    constexpr double angle_units_per_degree = 65536.0 / 360.0;
    constexpr double vi_per_second = 60.0;
    if (!std::isfinite(stick_axis) ||
        !std::isfinite(composed_mouse_delta) ||
        !std::isfinite(elapsed_vi) || elapsed_vi <= 0.0f ||
        !std::isfinite(sensitivity_scale) || sensitivity_scale <= 0.0) {
        return 0;
    }

    const double ticks = std::clamp(
        static_cast<double>(elapsed_vi), 0.0,
        static_cast<double>(manual_aim_max_elapsed_vi));
    const double stick_degrees_per_second = vertical
        ? static_cast<double>(stick_pitch_degrees_per_second)
        : static_cast<double>(stick_yaw_degrees_per_second);
    const double stick_degrees = -std::clamp(
        static_cast<double>(stick_axis), -1.0, 1.0) *
        stick_degrees_per_second * ticks / vi_per_second;
    const double mouse_degrees =
        -static_cast<double>(composed_mouse_delta) *
        static_cast<double>(mouse_degrees_per_composed_unit);
    return static_cast<int32_t>(std::clamp(
        std::llround((stick_degrees + mouse_degrees) *
            angle_units_per_degree * sensitivity_scale),
        static_cast<long long>(std::numeric_limits<int16_t>::min()),
        static_cast<long long>(std::numeric_limits<int16_t>::max())));
}

inline uint16_t aim_movement_buttons(int32_t x, int32_t y) {
    uint16_t buttons = 0;
    if (x < -movement_button_threshold) {
        buttons |= c_left;
    }
    else if (x > movement_button_threshold) {
        buttons |= c_right;
    }
    if (y < -movement_button_threshold) {
        buttons |= c_down;
    }
    else if (y > movement_button_threshold) {
        buttons |= c_up;
    }
    return buttons;
}

inline GameplayAxes gameplay_axes(
    int32_t left_x,
    int32_t left_raw_x,
    int32_t left_y,
    int32_t left_raw_y,
    int32_t right_x,
    int32_t right_y,
    bool physical_fine_aim
) {
    const uint16_t movement = aim_movement_buttons(left_x, left_y);
    if (physical_fine_aim) {
        return {
            left_x,
            left_raw_x,
            left_y,
            left_raw_y,
            right_x,
            right_x,
            right_y,
            right_y,
            0,
            0,
            movement,
            0,
            false,
        };
    }

    return {
        left_x,
        left_raw_x,
        left_y,
        left_raw_y,
        0,
        0,
        0,
        0,
        right_x,
        right_y,
        0,
        movement,
        true,
    };
}

}
