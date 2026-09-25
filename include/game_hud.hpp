#pragma once

#include "hud_model.hpp"

namespace jfc::hud {

struct Counters {
    int mizar_tokens = 0;
    Tribals room_tribals;
    bool tokens_valid = false;
    bool tribals_valid = false;
};

struct AreaPickups {
    uint16_t ship_parts = 0;
    uint32_t keys = 0;
    uint8_t floyd_parts = 0;
    bool valid = false;
};

void invalidate_metadata() noexcept;
Health read_health(uint8_t* rdram, uint32_t player_object, int capacity);
Counters read_counters(uint8_t* rdram, uint32_t character_save);
AreaPickups read_area_pickups(uint8_t* rdram, uint32_t character_save);
uint32_t read_tribal_icon(uint8_t* rdram, int frame = 0);
uint32_t read_weapon_icon(uint8_t* rdram, unsigned weapon);
void set_modern_health(bool enabled);
void set_room_counters(bool enabled);
void set_area_pickups(bool enabled);
void set_weapon_shortcuts(bool enabled);

}
