#pragma once

namespace jfc::camera {
inline constexpr double default_fov = 60.0;
inline constexpr double default_distance = 100.0;
void set_fov(double degrees);
void set_distance(double percent);
}
