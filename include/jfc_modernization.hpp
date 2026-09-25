#pragma once

#include <filesystem>
#include <cstdint>
#include <array>

namespace jfc::modernization {

enum class FinalTribalRequirement {
    Original = 100,
    Percent70 = 70,
    Percent50 = 50,
    Percent25 = 25,
};

enum class SprintMode {
    Off,
    Hold,
    Toggle,
};

enum class ShipPartScanner {
    Off,
    Audio,
    Visual,
    AudioVisual,
};

bool initialize_save_metadata(const std::filesystem::path& directory);

using SettingsOpenRequest = void (*)();
void set_settings_open_request(SettingsOpenRequest request);
void set_invulnerable_tribals(bool enabled);
void set_persistent_tribal_rescues(bool enabled);
void set_final_tribal_requirement(FinalTribalRequirement requirement);
void set_skip_seen_landings(bool enabled);
void set_quick_character_switching(bool enabled);
void set_skip_seen_character_travel(bool enabled);
void set_sprint_mode(SprintMode mode);
void set_double_pistol_recharge(bool enabled);
void set_double_ammo_pickups(bool enabled);
void set_unlimited_lives(bool enabled);
void set_mission_complete_refill(bool enabled);
void set_double_enemy_drops(bool enabled);
void set_fast_throws(bool enabled);
void set_halo_health(bool enabled);
void set_persistent_dropped_pickups(bool enabled);
void set_ship_part_scanner(ShipPartScanner mode);
void set_guidance_text(bool enabled);
void sample_actions(bool sprint, bool quick_switch);

void sample_weapon_actions(uint16_t actions);

constexpr bool is_quick_throwable(unsigned weapon) {
    return weapon >= 8 && weapon <= 14 && weapon != 13;
}
struct WeaponShortcuts {
    std::array<uint8_t, 4> slots{};
    uint8_t throwable = 8;
};
WeaponShortcuts weapon_shortcuts(uint8_t* rdram, unsigned character);

bool is_tribal_actor_type(unsigned int actor_type);
int required_tribal_count(int canonical_total, FinalTribalRequirement requirement);
float advance_sprint_multiplier(float current, bool active, float frame_delta);

}
