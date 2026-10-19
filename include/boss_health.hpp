#pragma once

#include <cstdint>
#include <array>

namespace jfc::boss {
struct Target {
    const char* name = nullptr;
    int remaining = 0;
    int maximum = 0;
    bool vulnerable = false;
    bool destroyed = false;
};
void set_half_health(bool enabled);
void set_target_hud(bool enabled);
void set_weak_point_effect(bool enabled);
void set_mantis_final_vulnerability(bool enabled);
void set_double_pistol_damage(bool enabled);
void set_phase_ammo_refill(bool enabled);
void invalidate_target() noexcept;
Target current_target(uint8_t* rdram, uint32_t player);
struct WeakPoint {
    std::array<float,3> minimum{}, maximum{};
    uint32_t identity = 0;
};
struct WeakPoints {
    std::array<WeakPoint,11> regions{};
    unsigned count = 0;
};
WeakPoints weak_points(uint8_t* rdram, uint32_t object);
}
