#pragma once

namespace jfc::camera {
inline constexpr double default_fov = 60.0;
inline constexpr double default_distance = 100.0;
inline constexpr double min_distance = 50.0;
inline constexpr double max_distance = 500.0;
inline constexpr double default_height = 0.0;
inline constexpr double min_height = -100.0;
inline constexpr double max_height = 200.0;
void set_fov(double degrees);
void set_distance(double percent);
void set_height(double units);
}
