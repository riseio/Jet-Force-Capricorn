#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace jfc::hud {

struct Health {
    int current_fixed = 0;
    int maximum_fixed = 0;
    int reserve_containers = 0;
    int full_reserves = 0;
    int active_fixed = 0;
    int active_capacity_fixed = 0;
    bool valid = false;
};

inline Health health(int current_fixed, int capacity) {
    if (capacity < 0 || capacity > 12) return {};
    const int container = (capacity + 1) * 256;
    const int maximum = (5 * capacity + 4) * 256;
    const int current = std::clamp(current_fixed, 0, maximum);
    const int reserves = current / container;
    return {current, maximum, 4, reserves, current % container, container, true};
}

inline int active_health_percent(const Health& value) {

    const int drawable = value.active_capacity_fixed - 256;
    if (!value.valid || drawable <= 0) return 0;
    return std::clamp((value.active_fixed * 100 + drawable - 1) / drawable, 0, 100);
}

struct Tribals {
    int total = 0;
    int rescued = 0;
    int missing = 0;
    int lost = 0;
};

inline Tribals tribals(uint16_t room_mask, uint16_t rescued, uint16_t dead) {
    const auto saved = static_cast<uint16_t>(room_mask & rescued);
    const auto missing = static_cast<uint16_t>(room_mask & ~rescued);
    return {std::popcount(room_mask), std::popcount(saved),
        std::popcount(missing),
        std::popcount(static_cast<uint16_t>(missing & dead))};
}

}
