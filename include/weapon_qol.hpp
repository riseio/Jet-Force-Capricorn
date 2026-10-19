#pragma once
#include <array>
#include <cstdint>

namespace jfc::weapons {
void set_shotgun_upgrade(bool enabled);
void set_flamethrower_upgrade(bool enabled);
void set_shocker_upgrade(bool enabled);
struct Arc { uint32_t beam = 0, source = 0, target = 0; };
struct Chain {
    uint32_t root = 0, owner = 0;
    std::array<uint8_t, 44> spawn{};
    std::array<Arc, 3> arcs{};
};

struct State {
    std::array<uint32_t, 0x800000 / 128> flame_burns{};
    std::array<uint8_t, 16> flame_half_round{};
    std::array<Chain, 4> chains{};
};
const State& state();
void restore_state(const State& state) noexcept;
void validate_state(const State& state);
}
