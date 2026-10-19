#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jfc::native_data {
struct IconBounds { int left, right, bottom, top; bool operator==(const IconBounds&) const = default; };
struct KeyIcon { unsigned flag; int variant; bool operator==(const KeyIcon&) const = default; };
struct PickupMask {
    uint32_t keys = 0;
    uint16_t parts = 0;
    uint8_t floyd = 0;
    void merge(PickupMask other) { keys |= other.keys; parts |= other.parts; floyd |= other.floyd; }
    bool operator==(const PickupMask&) const = default;
};
enum class RewardSelector : uint8_t { None, Dialogue, Chest };
struct ActorPickup {
    PickupMask direct;
    RewardSelector selector = RewardSelector::None;
    bool operator==(const ActorPickup&) const = default;
};
struct Definitions {
    std::array<int,12> boss_thresholds{};
    std::array<int,3> mantis_phase_budgets{};
    std::string pickup_capacity_label;
    std::array<uint8_t,94> tribal_requirement_clause{};
    std::array<IconBounds,12> part_bounds{};
    std::array<IconBounds,9> key_bounds{};
    IconBounds token_bounds{}, floyd_bounds{};
    std::vector<KeyIcon> key_icons;
    std::vector<ActorPickup> actor_pickups;
    std::array<PickupMask,256> dialogue_rewards{}, chest_rewards{};
    uint16_t event_level = 0;
    PickupMask event_reward;
    bool operator==(const Definitions&) const = default;
};

void initialize(std::span<const uint8_t> validated_rom);
const Definitions& get();
}
