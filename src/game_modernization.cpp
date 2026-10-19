#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>
#include <utility>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "jfc_modernization.hpp"
#include "weapon_qol.hpp"
#include "ultramodern/audio_activity.hpp"
#include "native_context.hpp"
#include "jfc_enhancements.hpp"
#include "jfc_recomp.h"
#include "native_rom_data.hpp"
#include "metadata_writer.hpp"
#include "save_state_game.hpp"
#include "save_state_codec.hpp"

extern "C" void controlSelectWeapon(uint8_t*, recomp_context*);

namespace {

std::atomic<jfc::modernization::SettingsOpenRequest> settings_open_request{nullptr};

constexpr size_t save_slot_count = 6;
constexpr size_t tribal_level_count = 64;
constexpr size_t scene_key_capacity = 64;
constexpr size_t binding_count = 8;
constexpr std::array<uint8_t, 8> metadata_magic{
    'J', 'F', 'C', 'Q', 'O', 'L', 0, 0};
constexpr uint32_t metadata_version = 6;
constexpr uint64_t legacy_scene_key_bit = UINT64_C(1) << 63;
constexpr uint64_t maximum_scene_identity_key = UINT64_C(1) << 62;
constexpr uint32_t gameplay_pointer_address = 0x800FD7D4U;
constexpr uint32_t current_level_pointer_address = 0x800FB118U;
constexpr uint32_t tribal_table_pointer_address = 0x800FD7B8U;
constexpr uint32_t current_level_address = 0x800A323CU;
constexpr uint32_t current_character_address = 0x800A3240U;
constexpr uint32_t current_region_address = 0x800A3244U;
constexpr uint32_t current_anim_group_address = 0x800A3248U;
constexpr uint32_t transition_mode_address = 0x800A324CU;
constexpr uint32_t next_level_address = 0x800A3250U;
constexpr uint32_t next_anim_group_address = 0x800A3254U;
constexpr uint32_t next_region_address = 0x800A3258U;
constexpr uint32_t next_character_address = 0x800A3260U;
constexpr uint32_t next_mode_address = 0x800A3264U;

constexpr uint32_t frontend_mode_address = 0x800A51B0U;
constexpr uint32_t animseq_state_address = 0x800A74D4U;
constexpr unsigned juno_tawfret_landing_flag = 96;
constexpr unsigned vela_palace_landing_flag = 97;
constexpr size_t canonical_region_count = 31;
constexpr unsigned first_ship_part_flag = 32;
constexpr unsigned last_ship_part_flag = 43;
constexpr unsigned jetpack_flag = 95;
constexpr unsigned all_tribals_flag = 88;
constexpr unsigned quarry_generator_flag = 89;
constexpr uint32_t player_list_address = 0x800F2D0CU;
constexpr uint32_t player_count_address = 0x800F2D10U;
constexpr uint32_t debug_buffer_end_address = 0x800A6384U;
constexpr uint32_t debug_buffer_start = 0x80100960U;
constexpr uint32_t debug_buffer_capacity = 0x800U;
constexpr int scanner_sound_id = 0x16B;

enum class ShipPartRequirement : uint8_t {
    None,
    Jetpack,
    QuarryGenerator,
    AllTribals,
    RouteOnly,
};

enum class ShipPartAcquisition : uint8_t {
    DirectPickup,
    NpcReward,
    TradeReward,
    MinigameReward,
    RaceReward,
};

struct ShipPartObjective {
    uint8_t flag;
    uint8_t characters;
    ShipPartRequirement requirement;
    ShipPartAcquisition acquisition;
    const char* name;
    const char* destination;
    const char* route_requirement;
    const char* action_source;
};

constexpr uint8_t vela_mask = 1U << 0;
constexpr uint8_t juno_mask = 1U << 1;
constexpr uint8_t lupus_mask = 1U << 2;
constexpr uint8_t all_characters = vela_mask | juno_mask | lupus_mask;

constexpr std::array<ShipPartObjective, 12> ship_part_objectives{{
    {32, all_characters, ShipPartRequirement::RouteOnly,
     ShipPartAcquisition::TradeReward,
     "POWER CELL", "WATER RUIN", "EARPLUGS", "IVANA"},
    {33, all_characters, ShipPartRequirement::QuarryGenerator,
     ShipPartAcquisition::MinigameReward,
     "RADAR DISH", "GEM QUARRY", "POWER QUARRY", "MAGNUS"},
    {34, all_characters, ShipPartRequirement::Jetpack,
     ShipPartAcquisition::NpcReward,
     "FIN", "SS ANUBIS PASSAGE", "JETPACK", "PASSAGE NPC"},
    {35, vela_mask, ShipPartRequirement::RouteOnly,
     ShipPartAcquisition::TradeReward,
     "CARGO KEY", "TAWFRET BRIDGE", "RETURN PANTS", "GIMLET"},
    {36, vela_mask, ShipPartRequirement::RouteOnly,
     ShipPartAcquisition::TradeReward,
     "DEFLECTOR", "RITH ESSA MINE", "KEY + DISGUISE", "MINE FARMER"},
    {37, vela_mask | juno_mask, ShipPartRequirement::RouteOnly,
     ShipPartAcquisition::RaceReward,
     "FUSE", "MIZAR PALACE", "WIN 3D RACE", "RACE ENTRANCE"},
    {38, vela_mask, ShipPartRequirement::None,
     ShipPartAcquisition::DirectPickup,
     "VELA HATCH KEY", "GOLDWOOD LODGE", "", ""},
    {39, juno_mask, ShipPartRequirement::None,
     ShipPartAcquisition::DirectPickup,
     "JUNO HATCH KEY", "SPAWNSHIP", "", ""},
    {40, lupus_mask, ShipPartRequirement::Jetpack,
     ShipPartAcquisition::DirectPickup,
     "LUPUS HATCH KEY", "ICHOR BASE", "JETPACK", ""},
    {41, vela_mask, ShipPartRequirement::None,
     ShipPartAcquisition::DirectPickup,
     "NITROGEN TANK", "WATER RUIN", "", ""},
    {42, all_characters, ShipPartRequirement::Jetpack,
     ShipPartAcquisition::DirectPickup,
     "OXYGEN TANK", "ESCHEBONE CORTEX", "JETPACK", ""},
    {43, all_characters, ShipPartRequirement::AllTribals,
     ShipPartAcquisition::NpcReward,
     "STABILIZER", "MIZAR PALACE", "ALL TRIBALS", "KING JEFF"},
}};

struct TrackedShipPart {
    uint8_t level = 0xFFU;
    uint32_t object = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint64_t frame = 0;
};

struct SlotMetadata {
    std::array<uint16_t, tribal_level_count> rescued_tribals{};

    std::array<std::array<uint16_t, tribal_level_count>, canonical_region_count>
        regional_rescues{};

    std::array<uint64_t, scene_key_capacity> seen_landings{};
    std::array<uint64_t, scene_key_capacity> seen_travel{};
    bool final_access_granted = false;

    std::array<std::array<uint8_t, 4>, 3> weapon_slots{};
    std::array<uint8_t, 3> throwables{8,8,8};
};

struct TransitionState {
    int level = -1;
    int character = -1;
    int region = -1;
    int group = -1;
    int mode = -1;
};

struct SequenceRoute {
    TransitionState source{};
    TransitionState destination{};
    int sequence_group = -1;
};

struct GameBinding {
    uint32_t game = 0;
    int slot = -1;
    SlotMetadata metadata{};
};

std::array<SlotMetadata, save_slot_count> slots{};
std::array<GameBinding, binding_count> bindings{};
std::filesystem::path metadata_path;
std::mutex metadata_mutex;

struct TribalReconciliation {
    uint32_t game = 0;
    unsigned region = 0;
    uint64_t revision = 0;
    uint64_t option_revision = 0;
    uint32_t counts = 0;
    int total = 0;
    bool complete = false;
    std::array<uint16_t, tribal_level_count> rescued{}, killed{};
};
std::array<TribalReconciliation, binding_count> tribal_reconciliation{};
uint64_t metadata_revision = 1;
std::atomic<uint64_t> tribal_option_revision{1};
std::atomic_bool invulnerable_tribals{false};
std::atomic_bool persistent_tribal_rescues{true};
std::atomic<jfc::modernization::FinalTribalRequirement> final_requirement{
    jfc::modernization::FinalTribalRequirement::Original};
std::atomic_bool skip_seen_landings{false};
std::atomic_bool quick_character_switching{false};
std::atomic_bool double_pistol_recharge{false};
std::atomic_bool double_ammo_pickups{false};
std::atomic_bool unlimited_lives{true};
std::atomic_bool mission_complete_refill{true};
std::atomic_bool double_enemy_drops{true};
std::atomic_bool double_robot_mission_time{false};
std::atomic_bool hide_companion{false};
std::atomic_bool fast_throws{true};
std::atomic_bool halo_health{false};
std::atomic_bool health_reset_requested{false};

struct HealthRecharge {
    uint32_t object = 0, hit = 0, record = 0;
    int last_health = -1, capacity = 0, elapsed = 0, deficit = 0;
    bool damaged = false;
};
thread_local HealthRecharge health_recharge{};
std::atomic_bool persistent_dropped_pickups{false};
std::atomic_bool persistent_corpses{false};

std::array<uint32_t, 0x800000 / 128> completed_corpses{};

std::array<uint32_t, 0x800000 / 128> retired_corpses{};
thread_local uint32_t corpse_retirement_object = 0;
std::atomic_bool skip_seen_character_travel{false};
std::atomic<jfc::modernization::SprintMode> sprint_mode{
    jfc::modernization::SprintMode::Off};
std::atomic<jfc::modernization::ShipPartScanner> ship_part_scanner{
    jfc::modernization::ShipPartScanner::Off};
std::atomic_bool sprint_action{false};
std::atomic_bool quick_switch_action{false};
std::atomic<uint16_t> weapon_actions{0};
thread_local uint16_t previous_weapon_actions = 0x3FF;
thread_local uint16_t pressed_weapon_actions = 0;
struct QuickGrenade {
    uint32_t object = 0, player = 0, record = 0;
    uint8_t previous_weapon = 0, phase = 0;
    bool animation_seen = false;
    uint8_t weapon = 8;
};
thread_local QuickGrenade quick_grenade{};

thread_local std::array<int32_t, 3> grenade_saved_input{};
thread_local bool grenade_input_scoped = false;

int pending_save_slot = -1;
uint32_t pending_save_game = 0;
int pending_clear_slot = -1;
thread_local bool actions_initialized = false;
thread_local bool previous_sprint_action = false;
thread_local bool previous_quick_switch_action = false;
thread_local bool sprint_toggled = false;
thread_local jfc::modernization::SprintMode previous_sprint_mode =
    jfc::modernization::SprintMode::Off;
thread_local uint32_t sprint_player = 0;
thread_local uint32_t sprint_object = 0;
thread_local float sprint_multiplier = 1.0f;
enum class SequenceKind { None, Landing, Travel };
thread_local int32_t animseq_previous_state = 0;
thread_local uint64_t active_sequence_key = 0;
thread_local uint16_t active_legacy_sequence_key = 0;
thread_local SequenceRoute active_sequence_route{};
thread_local SequenceKind active_sequence_kind = SequenceKind::None;
thread_local uint64_t pending_sequence_key = 0;
thread_local SequenceRoute pending_sequence_route{};
thread_local SequenceKind pending_sequence_kind = SequenceKind::None;
thread_local bool seen_sequence_skip_applied = false;
thread_local TransitionState committed_transition{};
thread_local bool committed_transition_valid = false;
thread_local std::array<TrackedShipPart, ship_part_objectives.size()>
    tracked_ship_parts{};
thread_local uint64_t scanner_frame = 1;
thread_local uint8_t scanner_level = 0xFFU;
thread_local uint8_t scanner_audio_target = 0xFFU;
thread_local unsigned scanner_audio_countdown = 0;

constexpr size_t version_three_slot_size =
    tribal_level_count * sizeof(uint16_t) +
    scene_key_capacity * sizeof(uint64_t) * 2 + 1;
constexpr size_t version_four_slot_size = version_three_slot_size + 12;
constexpr size_t version_five_slot_size = version_four_slot_size + 3;
constexpr size_t serialized_slot_size = version_five_slot_size +
    canonical_region_count * tribal_level_count * sizeof(uint16_t);
constexpr size_t version_two_slot_size =
    tribal_level_count * sizeof(uint16_t) +
    scene_key_capacity * sizeof(uint16_t) * 2 + 1;
constexpr size_t version_one_slot_size =
    tribal_level_count * sizeof(uint16_t) + 8 * sizeof(uint64_t) + 1;
constexpr size_t serialized_payload_size = save_slot_count * serialized_slot_size;
constexpr size_t version_two_payload_size =
    save_slot_count * version_two_slot_size;
constexpr size_t version_one_payload_size =
    save_slot_count * version_one_slot_size;
constexpr size_t serialized_header_size = metadata_magic.size() + 3 * sizeof(uint32_t);
constexpr size_t serialized_file_size = serialized_header_size + serialized_payload_size;
constexpr size_t version_five_payload_size = save_slot_count * version_five_slot_size;
constexpr size_t version_five_file_size = serialized_header_size + version_five_payload_size;
constexpr size_t version_four_payload_size = save_slot_count * version_four_slot_size;
constexpr size_t version_four_file_size = serialized_header_size + version_four_payload_size;
constexpr size_t version_three_payload_size = save_slot_count * version_three_slot_size;
constexpr size_t version_three_file_size = serialized_header_size + version_three_payload_size;
constexpr size_t version_two_file_size =
    serialized_header_size + version_two_payload_size;
constexpr size_t version_one_file_size =
    serialized_header_size + version_one_payload_size;

bool valid_rdram_address(uint32_t address, uint32_t size) {
    constexpr uint32_t rdram_size = 8U * 1024U * 1024U;
    const uint32_t segment = address & 0xE0000000U;
    const uint32_t physical = address & 0x1FFFFFFFU;
    return (segment == 0x80000000U || segment == 0xA0000000U) &&
        size <= rdram_size && physical <= rdram_size - size;
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
}

bool read_u16(std::span<const uint8_t> bytes, size_t& cursor, uint16_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(uint16_t)) {
        return false;
    }
    value = static_cast<uint16_t>(bytes[cursor]) |
        static_cast<uint16_t>(bytes[cursor + 1]) << 8;
    cursor += sizeof(uint16_t);
    return true;
}

bool read_u32(std::span<const uint8_t> bytes, size_t& cursor, uint32_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(uint32_t)) {
        return false;
    }
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(bytes[cursor++]) << shift;
    }
    return true;
}

bool read_u64(std::span<const uint8_t> bytes, size_t& cursor, uint64_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(uint64_t)) {
        return false;
    }
    value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(bytes[cursor++]) << shift;
    }
    return true;
}

uint32_t crc32(std::span<const uint8_t> bytes) {
    uint32_t crc = std::numeric_limits<uint32_t>::max();
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U &
                (0U - static_cast<uint32_t>(crc & 1U)));
        }
    }
    return ~crc;
}

std::vector<uint8_t> serialize_metadata() {
    std::vector<uint8_t> payload;
    payload.reserve(serialized_payload_size);
    for (const SlotMetadata& slot : slots) {
        for (uint16_t rescued : slot.rescued_tribals) {
            append_u16(payload, rescued);
        }
        for (uint64_t seen : slot.seen_landings) {
            append_u64(payload, seen);
        }
        for (uint64_t seen : slot.seen_travel) {
            append_u64(payload, seen);
        }
        payload.push_back(slot.final_access_granted ? 1U : 0U);
        for (const auto& character : slot.weapon_slots)
            payload.insert(payload.end(), character.begin(), character.end());
        payload.insert(payload.end(), slot.throwables.begin(), slot.throwables.end());
        for (const auto& region : slot.regional_rescues)
            for (uint16_t rescued : region) append_u16(payload, rescued);
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(serialized_file_size);
    bytes.insert(bytes.end(), metadata_magic.begin(), metadata_magic.end());
    append_u32(bytes, metadata_version);
    append_u32(bytes, static_cast<uint32_t>(payload.size()));
    append_u32(bytes, crc32(payload));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

bool deserialize_metadata(std::span<const uint8_t> bytes) {
    if ((bytes.size() != serialized_file_size &&
         bytes.size() != version_five_file_size &&
         bytes.size() != version_four_file_size &&
         bytes.size() != version_three_file_size &&
         bytes.size() != version_two_file_size &&
         bytes.size() != version_one_file_size) ||
        !std::equal(metadata_magic.begin(), metadata_magic.end(), bytes.begin())) {
        return false;
    }
    size_t cursor = metadata_magic.size();
    uint32_t version = 0;
    uint32_t payload_size = 0;
    uint32_t checksum = 0;
    if (!read_u32(bytes, cursor, version) ||
        !read_u32(bytes, cursor, payload_size) ||
        !read_u32(bytes, cursor, checksum)) {
        return false;
    }
    size_t expected_payload_size = 0;
    switch (version) {
    case metadata_version:
        expected_payload_size = serialized_payload_size;
        break;
    case 5U:
        expected_payload_size = version_five_payload_size;
        break;
    case 4U:
        expected_payload_size = version_four_payload_size;
        break;
    case 3U:
        expected_payload_size = version_three_payload_size;
        break;
    case 2U:
        expected_payload_size = version_two_payload_size;
        break;
    case 1U:
        expected_payload_size = version_one_payload_size;
        break;
    default:
        return false;
    }
    if (payload_size != expected_payload_size ||
        cursor > bytes.size() || bytes.size() - cursor != payload_size ||
        crc32(bytes.subspan(cursor)) != checksum) {
        return false;
    }

    std::array<SlotMetadata, save_slot_count> loaded{};
    for (SlotMetadata& slot : loaded) {
        for (uint16_t& rescued : slot.rescued_tribals) {
            if (!read_u16(bytes, cursor, rescued)) {
                return false;
            }
        }
        if (version >= 3U) {
            for (uint64_t& seen : slot.seen_landings) {
                if (!read_u64(bytes, cursor, seen) ||
                    (seen > maximum_scene_identity_key &&
                     (seen & legacy_scene_key_bit) == 0) ||
                    ((seen & legacy_scene_key_bit) != 0 &&
                     ((seen & ~legacy_scene_key_bit) == 0 ||
                      (seen & ~legacy_scene_key_bit) > UINT16_MAX))) {
                    return false;
                }
            }
            for (uint64_t& seen : slot.seen_travel) {
                if (!read_u64(bytes, cursor, seen) ||
                    (seen > maximum_scene_identity_key &&
                     (seen & legacy_scene_key_bit) == 0) ||
                    ((seen & legacy_scene_key_bit) != 0 &&
                     ((seen & ~legacy_scene_key_bit) == 0 ||
                      (seen & ~legacy_scene_key_bit) > UINT16_MAX))) {
                    return false;
                }
            }
        }
        else if (version == 2U) {
            for (uint64_t& seen : slot.seen_landings) {
                uint16_t legacy = 0;
                if (!read_u16(bytes, cursor, legacy)) {
                    return false;
                }
                seen = legacy == 0 ? 0 : legacy_scene_key_bit | legacy;
            }
            for (uint64_t& seen : slot.seen_travel) {
                uint16_t legacy = 0;
                if (!read_u16(bytes, cursor, legacy)) {
                    return false;
                }
                seen = legacy == 0 ? 0 : legacy_scene_key_bit | legacy;
            }
        }
        else {
            uint64_t ignored = 0;
            for (size_t word = 0; word < 8; ++word) {
                if (!read_u64(bytes, cursor, ignored)) {
                    return false;
                }
            }
        }
        if (cursor == bytes.size() || bytes[cursor] > 1U) {
            return false;
        }
        slot.final_access_granted = bytes[cursor++] != 0;
        if (version >= 4U) {
            for (auto& character : slot.weapon_slots) for (auto& weapon : character) {
                if (cursor == bytes.size() || bytes[cursor] > 16) return false;
                weapon = bytes[cursor++];
            }
        }
        if (version >= 5U) for (auto& weapon : slot.throwables) {
            if (cursor == bytes.size() || !jfc::modernization::is_quick_throwable(bytes[cursor])) return false;
            weapon = bytes[cursor++];
        }
        if (version >= 6U) for (auto& region : slot.regional_rescues)
            for (auto& rescued : region)
                if (!read_u16(bytes, cursor, rescued)) return false;
    }
    if (cursor != bytes.size()) {
        return false;
    }
    slots = loaded;
    return true;
}

bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    if (error || (size != serialized_file_size &&
                  size != version_five_file_size &&
                  size != version_four_file_size &&
                  size != version_three_file_size &&
                  size != version_two_file_size &&
                  size != version_one_file_size)) {
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    return input.read(reinterpret_cast<char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size())).good();
}

std::atomic<void (*)(const char*)> metadata_error_reporter{nullptr};

jfc::modernization::MetadataWriter& metadata_writer() {
    static jfc::modernization::MetadataWriter writer(jfc::files::write_atomic,
        [](const char* error) {
            if (auto report = metadata_error_reporter.load(std::memory_order_acquire))
                report(error);
        });
    return writer;
}

bool persist_locked() {
    ++metadata_revision;
    if (metadata_path.empty()) {
        return false;
    }
    for (const auto& binding : bindings) {
        if (!binding.game || binding.slot < 0 || binding.slot >= int(slots.size())) continue;
        auto& saved = slots[size_t(binding.slot)].regional_rescues;
        for (size_t region = 0; region < saved.size(); ++region)
            for (size_t room = 0; room < tribal_level_count; ++room)
                saved[region][room] |= binding.metadata.regional_rescues[region][room];
    }
    for (auto& binding : bindings)
        if (binding.game && binding.slot >= 0 && binding.slot < int(slots.size()))
            binding.metadata = slots[size_t(binding.slot)];
    try {
        metadata_writer().submit(metadata_path, serialize_metadata());
        return true;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Failed to queue QoL save metadata: %s\n", error.what());
        return false;
    }
}

GameBinding* find_binding(uint32_t game) {
    if (game == 0) {
        return nullptr;
    }
    for (GameBinding& binding : bindings) {
        if (binding.game == game) {
            return &binding;
        }
    }
    return nullptr;
}

GameBinding* bind_game(uint8_t* rdram, uint32_t game, int slot) {
    ++metadata_revision;
    GameBinding* binding = find_binding(game);
    if (binding == nullptr) {
        auto available = std::find_if(bindings.begin(), bindings.end(),
            [](const GameBinding& candidate) { return candidate.game == 0; });
        if (available == bindings.end()) {
            const auto active = uint32_t(MEM_W(0, gameplay_pointer_address));

            available = std::find_if(bindings.begin(), bindings.end(), [&](const GameBinding& candidate) {
                return candidate.slot >= 0 && candidate.game != active && candidate.game != pending_save_game;
            });
            if (available == bindings.end())
                throw std::runtime_error("QoL save bindings exhausted by live unsaved owners");
        }
        binding = &*available;
    }
    binding->game = game;
    binding->slot = slot;
    binding->metadata = slot >= 0 && slot < static_cast<int>(save_slot_count)
        ? slots[static_cast<size_t>(slot)] : SlotMetadata{};
    return binding;
}

int game_slot(uint8_t* rdram, uint32_t game) {
    if (!valid_rdram_address(game, 4)) {
        return -1;
    }
    const int slot = MEM_B(3, game);
    return slot >= 0 && slot < static_cast<int>(save_slot_count) ? slot : -1;
}

GameBinding* active_binding(uint8_t* rdram) {
    const uint32_t game = static_cast<uint32_t>(MEM_W(0, gameplay_pointer_address));
    if (!valid_rdram_address(game, 4)) {
        return nullptr;
    }
    if (GameBinding* binding = find_binding(game)) {

        if (binding->slot == -1 || binding->slot == game_slot(rdram, game) || game == pending_save_game) return binding;
    }
    return bind_game(rdram, game, game_slot(rdram, game));
}

uint8_t current_level_id(uint8_t* rdram) {
    const uint32_t level = static_cast<uint32_t>(MEM_W(0, current_level_pointer_address));
    return valid_rdram_address(level, 0x104) ? MEM_BU(0x103, level) : 0xFFU;
}

bool tribal_has_completed_outcome(uint8_t* rdram, uint32_t actor) {
    if (!valid_rdram_address(actor, 0x65U)) {
        return false;
    }
    const uint8_t level = current_level_id(rdram);
    const uint32_t game = static_cast<uint32_t>(
        MEM_W(0, gameplay_pointer_address));
    if (level >= tribal_level_count ||
        !valid_rdram_address(game, 0xDEU + level * 2U)) {
        return false;
    }
    const unsigned placement = MEM_BU(0x64, actor) & 0xFU;
    const uint16_t mask = static_cast<uint16_t>(1U << placement);
    return ((MEM_HU(0, game + 0x5CU + level * 2U) |
             MEM_HU(0, game + 0xDCU + level * 2U)) & mask) != 0;
}

uint16_t legacy_sequence_key(uint8_t level, int32_t group) {
    if (level == 0xFFU || group < 0 || group > 0xFF) {
        return 0;
    }
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(level) << 8 | static_cast<uint16_t>(group)) + 1U);
}

bool valid_transition_state(const TransitionState& state) {
    return state.level >= 0 && state.level <= 0x1FF &&
        state.character >= 0 && state.character <= 3 &&
        state.region >= 0 && state.region <= 0x1F &&
        state.group >= 0 && state.group <= 0xFF &&
        state.mode >= 0 && state.mode <= 0x7F;
}

TransitionState current_transition_state(uint8_t* rdram) {
    return {
        MEM_H(0, current_level_address),
        MEM_H(0, current_character_address),
        MEM_H(0, current_region_address),
        MEM_H(0, current_anim_group_address),
        MEM_H(0, transition_mode_address),
    };
}

TransitionState next_transition_state(uint8_t* rdram) {
    return {
        MEM_H(0, next_level_address),
        MEM_H(0, next_character_address),
        MEM_H(0, next_region_address),
        MEM_H(0, next_anim_group_address),
        MEM_H(0, next_mode_address),
    };
}

bool same_transition_state(
    const TransitionState& left,
    const TransitionState& right
) {
    return left.level == right.level &&
        left.character == right.character &&
        left.region == right.region &&
        left.group == right.group &&
        left.mode == right.mode;
}

SequenceRoute make_sequence_route(uint8_t* rdram, int32_t sequence_group) {
    SequenceRoute route{};
    route.source = committed_transition_valid
        ? committed_transition : current_transition_state(rdram);
    route.destination = next_transition_state(rdram);
    route.sequence_group = sequence_group;
    return route;
}

uint64_t sequence_identity_key(const SequenceRoute& route) {
    if (!valid_transition_state(route.source) ||
        !valid_transition_state(route.destination) ||
        route.sequence_group < 0 || route.sequence_group > 0xFF) {
        return 0;
    }

    uint64_t packed = static_cast<uint64_t>(route.source.level);
    packed |= static_cast<uint64_t>(route.destination.level) << 9;
    packed |= static_cast<uint64_t>(route.sequence_group) << 18;
    packed |= static_cast<uint64_t>(route.source.region) << 26;
    packed |= static_cast<uint64_t>(route.destination.region) << 31;
    packed |= static_cast<uint64_t>(route.source.character) << 36;
    packed |= static_cast<uint64_t>(route.destination.character) << 38;
    packed |= static_cast<uint64_t>(route.source.mode) << 40;
    packed |= static_cast<uint64_t>(route.destination.mode) << 47;
    packed |= static_cast<uint64_t>(route.destination.group) << 54;
    return packed + 1U;
}

template <size_t N>
bool contains_key(
    const std::array<uint64_t, N>& keys,
    uint64_t key,
    uint16_t legacy_key = 0
) {
    if (key != 0 && std::find(keys.begin(), keys.end(), key) != keys.end()) {
        return true;
    }
    const uint64_t migrated_legacy_key = legacy_key == 0
        ? 0 : legacy_scene_key_bit | static_cast<uint64_t>(legacy_key);
    return migrated_legacy_key != 0 &&
        std::find(keys.begin(), keys.end(), migrated_legacy_key) != keys.end();
}

template <size_t N>
bool insert_key(std::array<uint64_t, N>& keys, uint64_t key) {
    if (key == 0 || contains_key(keys, key)) {
        return false;
    }
    const auto empty = std::find(keys.begin(), keys.end(), 0);
    if (empty == keys.end()) {
        std::fprintf(stderr, "QoL scene history is full; scene was not recorded\n");
        return false;
    }
    *empty = key;
    return true;
}

void clear_pending_sequence() {
    pending_sequence_key = 0;
    pending_sequence_route = {};
    pending_sequence_kind = SequenceKind::None;
}

bool record_seen_sequence(
    uint8_t* rdram,
    SequenceKind kind,
    uint64_t key
) {
    if (key == 0 || kind == SequenceKind::None) {
        return false;
    }
    std::lock_guard lock(metadata_mutex);
    GameBinding* binding = active_binding(rdram);
    if (binding == nullptr) {
        return false;
    }
    const bool inserted = kind == SequenceKind::Landing
        ? insert_key(binding->metadata.seen_landings, key)
        : insert_key(binding->metadata.seen_travel, key);
    if (inserted && binding->slot >= 0) {
        slots[static_cast<size_t>(binding->slot)] = binding->metadata;
        persist_locked();
    }
    return inserted;
}

int tribal_region_total(uint8_t* rdram, unsigned region) {
    const uint32_t table = uint32_t(MEM_W(0, tribal_table_pointer_address));
    if (region >= canonical_region_count ||
        !valid_rdram_address(table, canonical_region_count)) return 0;
    return std::max(0, int(MEM_B(region, table)));
}

unsigned tribal_region(uint8_t* rdram) {
    return uint16_t(MEM_HU(0, 0x800A3268U));
}

uint32_t tribal_counters(uint8_t* rdram, uint32_t game) {
    return (uint32_t(MEM_BU(4, game)) << 16) |
        (uint32_t(MEM_BU(5, game)) << 8) | uint32_t(MEM_BU(6, game));
}

void restore_persistent_region(uint8_t* rdram, unsigned region,
                               int room = -1, unsigned placement = 0) {
    if (!persistent_tribal_rescues.load(std::memory_order_relaxed) ||
        region >= canonical_region_count) {
        return;
    }
    const uint32_t game = static_cast<uint32_t>(MEM_W(0, gameplay_pointer_address));
    if (!valid_rdram_address(game, 0x334 + canonical_region_count * 8)) {
        return;
    }
    std::lock_guard lock(metadata_mutex);
    GameBinding* binding = active_binding(rdram);
    if (binding == nullptr) {
        return;
    }

    const int total = tribal_region_total(rdram, region);
    const bool complete = total > 0 && MEM_BU(0x334 + region * 8, game) == total;
    auto& cached = tribal_reconciliation[size_t(binding - bindings.data())];
    const auto option_revision = tribal_option_revision.load(std::memory_order_relaxed);
    if (room >= 0 && room < int(tribal_level_count) &&
        cached.game == game && cached.region == region &&
        cached.revision == metadata_revision && cached.option_revision == option_revision &&
        cached.counts == tribal_counters(rdram, game) && cached.total == total &&
        cached.complete == complete &&
        cached.rescued[size_t(room)] == MEM_HU(0x5C + room * 2, game) &&
        cached.killed[size_t(room)] == MEM_HU(0xDC + room * 2, game) &&
        (!complete || (MEM_HU(0x5C + room * 2, game) & (1U << placement)) != 0)) {
        return;
    }
    int rescued_count = 0, killed_count = 0;
    bool changed = false;
    const bool current = region == tribal_region(rdram);
    for (unsigned local = 0; local < tribal_level_count; ++local) {
        const uint32_t rescue_address = game + 0x5CU + local * 2U;
        const uint32_t killed_address = game + 0xDCU + local * 2U;
        uint16_t rescued = MEM_HU(0, rescue_address) |
            binding->metadata.regional_rescues[region][local];
        if (complete && int(local) == room && placement < 16)
            rescued |= uint16_t(1U << placement);
        if (current && binding->metadata.regional_rescues[region][local] != rescued) {
            binding->metadata.regional_rescues[region][local] = rescued;
            changed = true;
        }
        const uint16_t killed = complete ? 0 : uint16_t(MEM_HU(0, killed_address) & ~rescued);
        MEM_H(0, rescue_address) = rescued;
        MEM_H(0, killed_address) = killed;
        cached.rescued[local] = rescued;
        cached.killed[local] = killed;
        rescued_count += std::popcount(rescued);
        killed_count += std::popcount(killed);
    }
    if (total > 0) {
        MEM_B(4, game) = complete ? total : std::min(rescued_count, total);
        MEM_B(5, game) = complete ? 0 : std::min(killed_count, total - int(MEM_BU(4, game)));
        MEM_B(6, game) = total - MEM_BU(4, game) - MEM_BU(5, game);
    }
    if (changed && binding->slot >= 0) {
        slots[static_cast<size_t>(binding->slot)] = binding->metadata;
        persist_locked();
    }
    cached.game = game;
    cached.region = region;
    cached.revision = metadata_revision;
    cached.option_revision = option_revision;
    cached.counts = tribal_counters(rdram, game);
    cached.total = total;
    cached.complete = complete;
}

void record_rescue(uint8_t* rdram, uint8_t level_id, unsigned placement) {
    if (!persistent_tribal_rescues.load(std::memory_order_relaxed) ||
        level_id >= tribal_level_count || placement >= 16) {
        return;
    }
    const uint32_t game = static_cast<uint32_t>(MEM_W(0, gameplay_pointer_address));
    const unsigned region = tribal_region(rdram);
    if (!valid_rdram_address(game, 4) || region >= canonical_region_count) {
        return;
    }
    std::lock_guard lock(metadata_mutex);
    GameBinding* binding = active_binding(rdram);
    if (binding == nullptr) {
        return;
    }
    const uint16_t mask = static_cast<uint16_t>(1U << placement);
    if ((binding->metadata.regional_rescues[region][level_id] & mask) != 0) {
        return;
    }
    binding->metadata.regional_rescues[region][level_id] |= mask;
    if (binding->slot >= 0) {
        slots[static_cast<size_t>(binding->slot)] = binding->metadata;
        persist_locked();
    }
}

int rescued_count(uint8_t* rdram, const GameBinding* binding) {
    const uint32_t game = static_cast<uint32_t>(MEM_W(0, gameplay_pointer_address));
    if (!valid_rdram_address(game, 0x334 + canonical_region_count * 8)) {
        return 0;
    }
    int count = 0;
    const bool include_persistent =
        persistent_tribal_rescues.load(std::memory_order_relaxed) &&
        binding != nullptr;
    const unsigned current_region = tribal_region(rdram);
    for (unsigned region = 0; region < canonical_region_count; ++region) {
        int individual_count = 0;
        for (unsigned room = 0; room < tribal_level_count; ++room) {
            uint16_t rescued = region == current_region ? MEM_HU(0x5C + room * 2, game) : 0;
            if (include_persistent) rescued |= binding->metadata.regional_rescues[region][room];
            individual_count += std::popcount(rescued);
        }
        count += std::min(tribal_region_total(rdram, region),
            std::max(individual_count, int(MEM_BU(0x334 + region * 8, game))));
    }
    return count;
}

int canonical_tribal_total(uint8_t* rdram) {
    const uint32_t table = static_cast<uint32_t>(
        MEM_W(0, tribal_table_pointer_address));
    if (!valid_rdram_address(table, canonical_region_count)) {
        return 0;
    }
    int total = 0;
    for (size_t region = 0; region < canonical_region_count; ++region) {
        const int count = MEM_B(static_cast<uint32_t>(region), table);
        if (count < 0) {
            return 0;
        }
        total += count;
    }
    return total;
}

int reduced_tribal_threshold(uint8_t* rdram) {
    const auto requirement = final_requirement.load(std::memory_order_relaxed);
    if (requirement == jfc::modernization::FinalTribalRequirement::Original) return 0;
    return jfc::modernization::required_tribal_count(canonical_tribal_total(rdram), requirement);
}

bool game_flag(uint8_t* rdram, uint32_t game, unsigned flag) {
    const uint32_t address = game + 0x30U + flag / 8U;
    return valid_rdram_address(address, 1) &&
        (MEM_BU(0, address) & (1U << (flag & 7U))) != 0;
}

SequenceKind current_sequence_kind(uint8_t* rdram) {

    if (MEM_BU(0, frontend_mode_address) == 5) {
        return SequenceKind::Travel;
    }
    const uint32_t game = static_cast<uint32_t>(
        MEM_W(0, gameplay_pointer_address));
    if (valid_rdram_address(game, 1) &&
        (game_flag(rdram, game, juno_tawfret_landing_flag) ||
         game_flag(rdram, game, vela_palace_landing_flag))) {
        return SequenceKind::Landing;
    }
    return SequenceKind::None;
}

void refresh_active_travel_route(uint8_t* rdram) {
    if (current_sequence_kind(rdram) != SequenceKind::Travel ||
        !committed_transition_valid || committed_transition.mode != 5 ||
        active_sequence_route.sequence_group < 0 ||
        active_sequence_route.sequence_group > 0xFF ||
        (active_sequence_kind != SequenceKind::None &&
         active_sequence_kind != SequenceKind::Travel)) {
        return;
    }

    active_sequence_kind = SequenceKind::Travel;
    active_sequence_route.source = committed_transition;
    active_sequence_route.destination = next_transition_state(rdram);
    active_sequence_key = sequence_identity_key(active_sequence_route);
    clear_pending_sequence();
    if (active_sequence_key != 0) {
        pending_sequence_key = active_sequence_key;
        pending_sequence_route = active_sequence_route;
        pending_sequence_kind = SequenceKind::Travel;
    }
}

bool cinematic_active(uint8_t* rdram) {

    for (unsigned view = 0; view < 4; ++view) {
        if (MEM_W(view * 4, 0x801045B8U) != 0) return true;
    }
    const int count = MEM_W(0, player_count_address);
    if (count < 0 || count > 4) return true;
    if (count == 0) return false;
    const auto list = static_cast<uint32_t>(MEM_W(0, player_list_address));
    if ((list & 3U) || !valid_rdram_address(list, count * 4U)) return true;
    for (int i = 0; i < count; ++i) {
        const auto object = static_cast<uint32_t>(MEM_W(i * 4, list));
        if ((object & 3U) || !valid_rdram_address(object, 0x6CU)) return true;
        const auto player = static_cast<uint32_t>(MEM_W(0x68, object));
        if ((player & 3U) || !valid_rdram_address(player, 0x5C4U) ||
            MEM_W(0x5C0, player) != 0) return true;
    }
    return false;
}

bool observe_health(uint8_t* rdram) {
    if (health_reset_requested.exchange(false, std::memory_order_relaxed))
        health_recharge = {};
    if (!halo_health.load(std::memory_order_relaxed)) {
        health_recharge = {};
        return false;
    }
    const auto aligned = [](uint32_t p, uint32_t size, uint32_t alignment) {
        return !(p & (alignment - 1)) && valid_rdram_address(p, size);
    };
    const uint32_t game = uint32_t(MEM_W(0, gameplay_pointer_address));
    const uint32_t list = uint32_t(MEM_W(0, player_list_address));
    const unsigned character = MEM_HU(0, current_character_address) & 3U;
    if (MEM_BU(0, 0x800A4FC4U) != 0 ||
        MEM_BU(0, frontend_mode_address) != 16 || character >= 3 ||
        MEM_W(0, player_count_address) != 1 || !aligned(list, 4, 4) ||
        !aligned(game, 0x15C + 3 * 0x76, 4)) {
        health_recharge = {};
        return false;
    }
    const uint32_t object = uint32_t(MEM_W(0, list));
    if (!aligned(object, 0x6C, 4)) { health_recharge = {}; return false; }
    const uint32_t hit = uint32_t(MEM_W(0x4C, object));
    const uint32_t player = uint32_t(MEM_W(0x68, object));
    const uint32_t record = game + 0x15C + character * 0x76;
    const int upgrades = MEM_H(2, record);
    if (!aligned(hit, 8, 2) || !aligned(player, 0x5C4, 4) ||
        MEM_B(0, player) != 0 || (MEM_BU(1, player) & 3U) != character ||
        upgrades < 0 || upgrades > 12) {
        health_recharge = {};
        return false;
    }
    const int current = MEM_H(6, hit), capacity = (5 * upgrades + 4) * 256;
    auto& s = health_recharge;
    if (s.object != object || s.hit != hit || s.record != record || s.capacity != capacity)
        s = {object, hit, record, current, capacity};
    if (current < s.last_health) { s.elapsed = 0; s.deficit = 0; s.damaged = true; }
    s.last_health = current;
    if (current < 256 || current >= capacity) { s.elapsed = 0; s.deficit = 0; }
    return true;
}

bool update_actions() {
    const uint16_t held = weapon_actions.load(std::memory_order_relaxed);
    pressed_weapon_actions = held & ~previous_weapon_actions;
    previous_weapon_actions = held;
    const bool sprint = sprint_action.load(std::memory_order_relaxed);
    const bool quick = quick_switch_action.load(std::memory_order_relaxed);

    if (quick) pressed_weapon_actions = 0;
    const auto mode = sprint_mode.load(std::memory_order_relaxed);
    if (!actions_initialized) {
        actions_initialized = true;
        previous_sprint_action = sprint;
        previous_quick_switch_action = quick;
        previous_sprint_mode = mode;
        return false;
    }
    if (mode != previous_sprint_mode || mode == jfc::modernization::SprintMode::Off) {
        sprint_toggled = false;
        sprint_multiplier = 1.0f;
        sprint_player = 0;
        sprint_object = 0;
        previous_sprint_mode = mode;
    }
    if (mode == jfc::modernization::SprintMode::Toggle &&
        sprint && !previous_sprint_action) {
        sprint_toggled = !sprint_toggled;
    }
    const bool quick_pressed = quick && !previous_quick_switch_action;
    previous_sprint_action = sprint;
    previous_quick_switch_action = quick;
    return quick_pressed;
}

bool sprint_requested() {
    switch (sprint_mode.load(std::memory_order_relaxed)) {
    case jfc::modernization::SprintMode::Hold:
        return sprint_action.load(std::memory_order_relaxed);
    case jfc::modernization::SprintMode::Toggle:
        return sprint_toggled;
    case jfc::modernization::SprintMode::Off:
        return false;
    }
    return false;
}

float read_float(uint8_t* rdram, uint32_t address) {
    fpr value{};
    value.u32l = static_cast<uint32_t>(MEM_W(0, address));
    return value.fl;
}

void write_float(uint8_t* rdram, uint32_t address, float value) {
    fpr bits{};
    bits.fl = value;
    MEM_W(0, address) = static_cast<int32_t>(bits.u32l);
}

int ship_part_objective_score(
    uint8_t* rdram,
    uint32_t game,
    int character,
    const ShipPartObjective& objective
) {
    if (character < 0 || character > 2 ||
        (objective.characters & (1U << character)) == 0) {
        return 3;
    }
    switch (objective.requirement) {
    case ShipPartRequirement::None:
        return 0;
    case ShipPartRequirement::Jetpack:
        return game_flag(rdram, game, jetpack_flag) ? 0 : 2;
    case ShipPartRequirement::QuarryGenerator:
        return game_flag(rdram, game, quarry_generator_flag) ? 0 : 2;
    case ShipPartRequirement::AllTribals:
        return game_flag(rdram, game, all_tribals_flag) ? 0 : 2;
    case ShipPartRequirement::RouteOnly:
        return 1;
    }
    return 3;
}

bool ship_part_character_eligible(
    int character,
    const ShipPartObjective& objective
) {
    return character >= 0 && character <= 2 &&
        (objective.characters & (1U << character)) != 0;
}

bool ship_part_is_direct(const ShipPartObjective& objective) {
    return objective.acquisition == ShipPartAcquisition::DirectPickup;
}

const ShipPartObjective* next_ship_part_objective(
    uint8_t* rdram,
    uint32_t game,
    int character,
    int& score
) {
    const ShipPartObjective* selected = nullptr;
    score = 4;
    for (const ShipPartObjective& objective : ship_part_objectives) {
        if (game_flag(rdram, game, objective.flag)) {
            continue;
        }
        const int candidate_score = ship_part_objective_score(
            rdram, game, character, objective);
        if (candidate_score < score) {
            selected = &objective;
            score = candidate_score;
        }
    }
    return selected;
}

const char* required_character_name(uint8_t characters) {
    switch (characters) {
    case vela_mask:
        return "VELA";
    case juno_mask:
        return "JUNO";
    case lupus_mask:
        return "LUPUS";
    case vela_mask | juno_mask:
        return "JUNO OR VELA";
    default:
        return "ANOTHER CHARACTER";
    }
}

uint32_t first_player_object(uint8_t* rdram) {
    const int count = MEM_W(0, player_count_address);
    const uint32_t list = static_cast<uint32_t>(MEM_W(0, player_list_address));
    if (count <= 0 || !valid_rdram_address(list, sizeof(uint32_t))) {
        return 0;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0, list));
    return valid_rdram_address(object, 0x170U) ? object : 0;
}

bool scanner_gameplay_active(uint8_t* rdram) {
    return MEM_BU(0, frontend_mode_address) == 16 &&
        MEM_W(0, 0x800FD7C4U) == 0 &&
        MEM_B(0, 0x800FD7BDU) == 0 &&
        MEM_B(0, 0x800A3294U) == 0 &&
        MEM_BU(0, 0x800A4FC4U) == 0 &&
        MEM_W(0, 0x800A3328U) == 0 &&
        !cinematic_active(rdram);
}

void reset_scanner_tracking(uint8_t level) {
    tracked_ship_parts = {};
    scanner_level = level;
    scanner_audio_target = 0xFFU;
    scanner_audio_countdown = 0;
}

void advance_scanner_frame() {
    ++scanner_frame;
    if (scanner_frame == 0) {
        scanner_frame = 1;
        tracked_ship_parts = {};
    }
}

void append_debug_byte(uint8_t* rdram, uint32_t& end, uint8_t value) {
    MEM_B(0, end) = static_cast<int8_t>(value);
    ++end;
}

std::atomic_bool guidance_text{false};

bool append_debug_hud_line(uint8_t* rdram, const char* line, uint8_t y) {
    if (!guidance_text.load(std::memory_order_relaxed)) return false;
    const size_t length = std::strlen(line);
    constexpr size_t command_bytes = 18;
    const size_t required = command_bytes + length + 1;
    uint32_t end = static_cast<uint32_t>(
        MEM_W(0, debug_buffer_end_address));
    if (end < debug_buffer_start ||
        end > debug_buffer_start + debug_buffer_capacity ||
        required > debug_buffer_start + debug_buffer_capacity - end ||
        !valid_rdram_address(end, static_cast<uint32_t>(required))) {
        return false;
    }

    const std::array<uint8_t, command_bytes> commands{
        0x81, 0x68, 0xFF, 0xA8, 0xFF, 0,
        0x85, 0, 0, 0, 0, 0,
        0x82, 0, 0, y, 0, 0,
    };
    for (uint8_t byte : commands) {
        append_debug_byte(rdram, end, byte);
    }
    for (size_t index = 0; index <= length; ++index) {
        append_debug_byte(rdram, end, static_cast<uint8_t>(line[index]));
    }
    MEM_W(0, debug_buffer_end_address) = static_cast<int32_t>(end);
    return true;
}

const char* scanner_direction(float dx, float dz, int16_t camera_yaw) {
    if (std::hypot(dx, dz) < 0.001f) {
        return "<>";
    }
    constexpr float angle_units_per_radian =
        65536.0f / (2.0f * 3.14159265358979323846f);
    const int16_t target_yaw = static_cast<int16_t>(std::lround(
        std::atan2(dx, dz) * angle_units_per_radian));
    const int16_t facing_yaw = static_cast<int16_t>(
        static_cast<uint16_t>(0x8000U) -
        static_cast<uint16_t>(camera_yaw));
    const int16_t delta = static_cast<int16_t>(
        static_cast<uint16_t>(target_yaw) -
        static_cast<uint16_t>(facing_yaw));
    if (std::abs(static_cast<int>(delta)) <= 0x2000) {
        return "^^";
    }
    return delta < 0 ? ">>" : "<<";
}

const char* scanner_range(float distance_squared) {
    if (distance_squared <= 250.0f * 250.0f) {
        return "NEAR";
    }
    if (distance_squared <= 800.0f * 800.0f) {
        return "MID";
    }
    return "FAR";
}

unsigned scanner_ping_interval(float distance_squared) {
    if (distance_squared <= 250.0f * 250.0f) {
        return 15;
    }
    if (distance_squared <= 800.0f * 800.0f) {
        return 30;
    }
    return 60;
}

bool sprint_compatible(uint8_t* rdram, uint32_t player) {
    return valid_rdram_address(player, 0x56AU) &&
        jfc::enhancements::grounded_traversal_active(rdram, player) &&
        MEM_BU(0x10D, player) == 0 &&
        MEM_BU(0x19F, player) == 0 &&
        !jfc::enhancements::physical_aim_active(MEM_B(0, player)) &&
        MEM_W(0, 0x800FD7C4U) == 0 &&
        MEM_B(0, 0x800FD7BDU) == 0 &&
        MEM_B(0, 0x800A3294U) == 0 &&
        !cinematic_active(rdram);
}

}

namespace jfc::modernization {
ultramodern::AudioActivity audio_activity(uint8_t* rdram) {
    using Activity = ultramodern::AudioActivity;
    constexpr uint32_t transition_countdown = 0x800A3294U;
    constexpr uint32_t pause_mode = 0x800FD7BDU;
    constexpr uint32_t game_mode = 0x800FD7C4U;

    if (MEM_B(0, transition_countdown) != 0) return Activity::Loading;
    if (MEM_B(0, pause_mode) != 0) return Activity::Paused;
    if (MEM_W(0, game_mode) != 0 || MEM_BU(0, frontend_mode_address) != 16)
        return Activity::Menu;
    return cinematic_active(rdram) ? Activity::Cutscene : Activity::Gameplay;
}

void set_settings_open_request(SettingsOpenRequest request) {
    settings_open_request.store(request, std::memory_order_release);
}

void set_save_metadata_error_reporter(void (*report)(const char*)) {
    metadata_error_reporter.store(report, std::memory_order_release);
}

void flush_save_metadata() {
    metadata_writer().flush();
}

bool initialize_save_metadata(const std::filesystem::path& directory) {

    try { flush_save_metadata(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Cannot reload QoL metadata: %s\n", error.what());
        return false;
    }
    std::lock_guard lock(metadata_mutex);
    slots = {};
    bindings = {};
    tribal_reconciliation = {};
    pending_save_slot = pending_clear_slot = -1;
    pending_save_game = 0;
    ++metadata_revision;
    active_sequence_key = 0;
    active_legacy_sequence_key = 0;
    active_sequence_route = {};
    active_sequence_kind = SequenceKind::None;
    clear_pending_sequence();
    seen_sequence_skip_applied = false;
    animseq_previous_state = 0;
    committed_transition = {};
    committed_transition_valid = false;
    metadata_path = directory / "modernization.sav";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::fprintf(stderr, "Failed to create QoL save directory: %s\n",
                     error.message().c_str());
        metadata_path.clear();
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!std::filesystem::exists(metadata_path, error)) {
        return !error;
    }
    if (!read_file(metadata_path, bytes) || !deserialize_metadata(bytes)) {
        std::fprintf(stderr,
            "Ignoring malformed or unsupported QoL save metadata: %s\n",
            metadata_path.string().c_str());
        slots = {};
    }
    return true;
}

void set_invulnerable_tribals(bool enabled) {
    invulnerable_tribals.store(enabled, std::memory_order_relaxed);
}

void set_persistent_tribal_rescues(bool enabled) {
    if (persistent_tribal_rescues.exchange(enabled, std::memory_order_relaxed) != enabled)
        tribal_option_revision.fetch_add(1, std::memory_order_relaxed);
}

void set_final_tribal_requirement(FinalTribalRequirement requirement) {
    switch (requirement) {
    case FinalTribalRequirement::Original:
    case FinalTribalRequirement::Percent70:
    case FinalTribalRequirement::Percent50:
    case FinalTribalRequirement::Percent25:
        break;
    default:
        requirement = FinalTribalRequirement::Original;
        break;
    }
    final_requirement.store(requirement, std::memory_order_relaxed);
}

void set_skip_seen_landings(bool enabled) {
    skip_seen_landings.store(enabled, std::memory_order_relaxed);
}

void set_quick_character_switching(bool enabled) {
    quick_character_switching.store(enabled, std::memory_order_relaxed);
}

void set_double_pistol_recharge(bool enabled) {
    double_pistol_recharge.store(enabled, std::memory_order_relaxed);
}

void set_double_ammo_pickups(bool enabled) {
    double_ammo_pickups.store(enabled, std::memory_order_relaxed);
}

void set_unlimited_lives(bool enabled) {
    unlimited_lives.store(enabled, std::memory_order_relaxed);
}

void set_mission_complete_refill(bool enabled) {
    mission_complete_refill.store(enabled, std::memory_order_relaxed);
}

void set_double_enemy_drops(bool enabled) {
    double_enemy_drops.store(enabled, std::memory_order_relaxed);
}

void set_double_robot_mission_time(bool enabled) {
    double_robot_mission_time.store(enabled, std::memory_order_relaxed);
}

void set_hide_companion(bool enabled) {
    hide_companion.store(enabled, std::memory_order_relaxed);
}

void set_fast_throws(bool enabled) {
    fast_throws.store(enabled, std::memory_order_relaxed);
}

void set_halo_health(bool enabled) {
    if (halo_health.exchange(enabled, std::memory_order_relaxed) != enabled)
        health_reset_requested.store(true, std::memory_order_relaxed);
}

void set_persistent_dropped_pickups(bool enabled) {
    persistent_dropped_pickups.store(enabled, std::memory_order_relaxed);
}

void set_persistent_corpses(bool enabled) {
    persistent_corpses.store(enabled, std::memory_order_relaxed);
}

void set_sprint_mode(SprintMode mode) {
    if (mode != SprintMode::Off && mode != SprintMode::Hold &&
        mode != SprintMode::Toggle) {
        mode = SprintMode::Off;
    }
    sprint_mode.store(mode, std::memory_order_relaxed);
}

void set_skip_seen_character_travel(bool enabled) {
    skip_seen_character_travel.store(enabled, std::memory_order_relaxed);
}

void set_ship_part_scanner(ShipPartScanner mode) {
    if (mode != ShipPartScanner::Off && mode != ShipPartScanner::Audio &&
        mode != ShipPartScanner::Visual &&
        mode != ShipPartScanner::AudioVisual) {
        mode = ShipPartScanner::Off;
    }
    ship_part_scanner.store(mode, std::memory_order_relaxed);
}

void set_guidance_text(bool enabled) {
    guidance_text.store(enabled, std::memory_order_relaxed);
}

void sample_actions(bool sprint, bool quick_switch) {
    sprint_action.store(sprint, std::memory_order_relaxed);
    quick_switch_action.store(quick_switch, std::memory_order_relaxed);
}

void sample_weapon_actions(uint16_t actions) {
    weapon_actions.store(actions & 0x3FFU, std::memory_order_relaxed);
}

WeaponShortcuts weapon_shortcuts(uint8_t* rdram, unsigned character) {
    if (character >= 3) return {};
    std::lock_guard lock(metadata_mutex);
    const auto* binding = active_binding(rdram);
    if (!binding) return {};
    return {binding->metadata.weapon_slots[character], binding->metadata.throwables[character]};
}

int required_tribal_count(int canonical_total,
                          FinalTribalRequirement requirement) {
    if (canonical_total <= 0) {
        return 0;
    }
    const int percent = static_cast<int>(requirement);
    return (canonical_total * percent + 99) / 100;
}

float advance_sprint_multiplier(float current, bool active, float frame_delta) {
    constexpr float original = 1.0f;
    constexpr float sprint = 1.5f;
    if (!std::isfinite(current) || !std::isfinite(frame_delta)) {
        return original;
    }
    current = std::clamp(current, original, sprint);
    const float target = active ? sprint : original;
    const float step = std::clamp(frame_delta, 0.0f, 4.0f) *
        (active ? 0.10f : 0.16f);
    return current < target
        ? std::min(current + step, target)
        : std::max(current - step, target);
}

}

extern "C" int jfc_unlimited_lives_enabled(void) {
    return unlimited_lives.load(std::memory_order_relaxed);
}

extern "C" int32_t jfc_throw_timer_step(int32_t weapon, int32_t before, int32_t after) {

    if (!fast_throws.load(std::memory_order_relaxed) || weapon < 8 || weapon > 14 ||
        before <= 0 || after >= before) return after;
    return int32_t(std::max<int64_t>(0, int64_t(after) * 2 - before));
}

extern "C" float jfc_throw_animation_rate(int32_t character, int32_t animation, float rate) {

    const bool throwing = character == 0 ? (animation == 22 || animation == 49) :
        character == 1 ? (animation == 21 || animation == 46) :
        character == 2 && (animation == 14 || animation == 27);
    return throwing && fast_throws.load(std::memory_order_relaxed) &&
        std::isfinite(rate) && rate > 0.0f ? rate * 2.0f : rate;
}

extern "C" void jfc_scale_enemy_drop_roll(recomp_context* ctx) {

    if (double_enemy_drops.load(std::memory_order_relaxed) && ctx->r2 == 2)
        ctx->r2 = 1;
}

extern "C" int32_t jfc_robot_mission_allowance(int32_t tenths) {
    const int multiplier = double_robot_mission_time.load(std::memory_order_relaxed) ? 2 : 1;
    if (tenths <= 0 || tenths > INT16_MAX / multiplier)
        throw std::runtime_error("Robot mission time allowance exceeds its signed 16-bit counter");
    return tenths * multiplier;
}

extern "C" int jfc_hide_companion(uint8_t* rdram, uint32_t object) {
    if (!hide_companion.load(std::memory_order_relaxed) ||
        !valid_rdram_address(object, 0x6CU) || MEM_H(0x48, object) != 2) return 0;
    const uint32_t companion = uint32_t(MEM_W(0x68, object));
    if (!valid_rdram_address(companion, 0x28U)) return 0;
    const uint32_t owner = uint32_t(MEM_W(0x24, companion));
    if (!valid_rdram_address(owner, 0x6CU) || MEM_H(0x48, owner) != 1) return 0;
    const uint32_t player = uint32_t(MEM_W(0x68, owner));
    if (!valid_rdram_address(player, 0x5C4U)) return 0;

    return uint32_t(MEM_W(0x50C, player)) == object && MEM_BU(0x10D, player) == 0 &&
        MEM_W(0, 0x801045B8U) == 0 && MEM_W(0x5C0, player) == 0;
}

extern "C" void jfc_refill_after_mission(uint8_t* rdram) {

    if (!mission_complete_refill.load(std::memory_order_relaxed) ||
        MEM_BU(0, 0x800A4FC4U) != 0) return;
    const uint32_t game = uint32_t(MEM_W(0, gameplay_pointer_address));
    const unsigned character = MEM_HU(0, current_character_address) & 3U;
    if (character >= 3 || (game & 1U) ||
        !valid_rdram_address(game, 0x15CU + 3U * 0x76U)) return;
    const uint32_t record = game + 0x15CU + character * 0x76U;
    const int upgrades = MEM_H(2, record);
    if (upgrades < 0 || upgrades > 12) return;
    const int count = MEM_W(0, player_count_address);
    const uint32_t list = uint32_t(MEM_W(0, player_list_address));
    if (count <= 0 || count > 4 || (list & 3U) ||
        !valid_rdram_address(list, count * 4U)) return;
    const uint32_t object = first_player_object(rdram);
    if (!object || (object & 3U)) return;
    const uint32_t hit = uint32_t(MEM_W(0x4C, object));
    const uint32_t player = uint32_t(MEM_W(0x68, object));
    if ((hit & 1U) || (player & 3U) || !valid_rdram_address(hit, 8) ||
        !valid_rdram_address(player, 0x543U) ||
        (MEM_BU(1, player) & 3U) != character) return;
    const int health = (5 * upgrades + 4) * 256;
    MEM_H(0, record) = health;
    MEM_H(6, hit) = health;
    const unsigned weapons = MEM_HU(0xA, record);

    for (unsigned weapon = 0; weapon < 16; ++weapon)
        if (weapons & (1U << weapon))
            MEM_H(0x14 + weapon * 2, record) = jfc_weapon_pickup_capacity(
                MEM_HU(0x34 + weapon * 2, record), weapon, MEM_HU(0x14 + weapon * 2, record));
    MEM_B(0x542, player) = 0;
}

extern "C" void jfc_refill_boss_phase_ammo(uint8_t* rdram) {
    if (MEM_BU(0, 0x800A4FC4U) != 0) return;
    const auto game = uint32_t(MEM_W(0, gameplay_pointer_address));
    const unsigned character = MEM_HU(0, current_character_address) & 3U;
    if (character >= 3 || (game & 1U) ||
        !valid_rdram_address(game, 0x15CU + 3U * 0x76U)) return;
    const uint32_t record = game + 0x15CU + character * 0x76U;
    const unsigned weapons = MEM_HU(0xA, record);

    for (unsigned weapon = 0; weapon < 16; ++weapon) {
        if (!(weapons & (1U << weapon))) continue;
        const unsigned capacity = jfc_weapon_capacity(MEM_HU(0x34 + weapon * 2, record), weapon);
        const unsigned held = MEM_HU(0x14 + weapon * 2, record);
        if (held < capacity)
            MEM_H(0x14 + weapon * 2, record) = std::min(capacity, held + (capacity + 3U) / 4U);
    }
}

extern "C" void jfc_reset_health_recharge(void) {
    health_recharge = {};
}

extern "C" void jfc_observe_health(uint8_t* rdram) {
    observe_health(rdram);
}

extern "C" void jfc_advance_health_recharge(uint8_t* rdram, int32_t ticks) {
    if (!observe_health(rdram)) return;
    auto& s = health_recharge;

    if (s.damaged) { s.damaged = false; return; }
    if (ticks <= 0 || s.last_health < 256 || s.last_health >= s.capacity ||
        MEM_B(0, 0x800FD7BDU) != 0 || MEM_W(0, 0x800A3328U) != 0 ||
        MEM_B(0, 0x800A3294U) != 0 || MEM_W(0, 0x800FD7C4U) != 0 ||
        cinematic_active(rdram)) return;
    constexpr int delay = 30 * 60, interval = 5 * 60, finish = 90 * 60;
    const int before = s.elapsed;
    s.elapsed += std::min(ticks, finish - s.elapsed);
    if (before < delay && s.elapsed >= delay) s.deficit = s.capacity - s.last_health;
    const int prior_step = std::max(0, before - delay) / interval;
    const int step = std::max(0, s.elapsed - delay) / interval;

    if (step == prior_step) return;
    const int award = s.deficit * step / 12 - s.deficit * prior_step / 12;
    const int health = std::min(s.capacity, s.last_health + award);
    MEM_H(6, s.hit) = health;
    MEM_H(0, s.record) = health;
    s.last_health = health;
}

extern "C" void jfc_route_pause_settings(recomp_context* ctx) {

    if (ctx->r4 != 6) return;
    const auto request = settings_open_request.load(std::memory_order_acquire);
    if (request == nullptr) {
        std::fputs("JFC pause settings host request is not registered\n", stderr);
        std::abort();
    }
    ctx->r4 = 1;
    request();
}

extern "C" void jfc_scale_pistol_recharge(recomp_context* ctx) {
    const auto delta = static_cast<uint32_t>(ctx->r25);
    if (double_pistol_recharge.load(std::memory_order_relaxed) && delta < 255U) {

        ctx->r25 = std::min(delta * 2U, 255U);
    }
}

namespace {
gpr doubled_ammo_award(gpr award, gpr current, gpr capacity) {
    if (!double_ammo_pickups.load(std::memory_order_relaxed) ||
        static_cast<int32_t>(award) <= 0) return award;
    const auto amount = static_cast<uint32_t>(award);
    const auto held = static_cast<uint32_t>(current);
    const auto cap = static_cast<uint32_t>(capacity);

    if (held > 65535U || cap > 65535U || amount > 32767U) return award;
    return std::min(amount * 2U, cap > held ? cap - held : 0U);
}
}

extern "C" void jfc_scale_small_ammo_pickup(recomp_context* ctx) {
    ctx->r12 = doubled_ammo_award(ctx->r12, ctx->r11, ctx->r4);
}

extern "C" void jfc_scale_large_ammo_pickup(recomp_context* ctx) {
    ctx->r10 = doubled_ammo_award(ctx->r10, ctx->r25, ctx->r4);
}

extern "C" void jfc_scale_dropped_ammo_pickup(recomp_context* ctx) {
    ctx->r24 = doubled_ammo_award(ctx->r24, ctx->r15, ctx->r4);
}

extern "C" void jfc_scale_duplicate_weapon_ammo(uint8_t* rdram, recomp_context* ctx) {
    if (!double_ammo_pickups.load(std::memory_order_relaxed)) return;
    const auto record = static_cast<uint32_t>(ctx->r5);
    if (!valid_rdram_address(record, 0x36U) || (record & 1U)) return;
    const auto pickup = uint32_t(ctx->r6);
    if (!valid_rdram_address(pickup, 2) || (pickup & 1U)) return;
    ctx->r25 = doubled_ammo_award(ctx->r25, ctx->r24,
        jfc_weapon_capacity(MEM_HU(0x34, record), MEM_HU(0, pickup)));
}

extern "C" void jfc_preserve_dropped_collectible(uint8_t* rdram, recomp_context* ctx) {
    if (!persistent_dropped_pickups.load(std::memory_order_relaxed)) return;
    const auto object = static_cast<uint32_t>(ctx->r7);
    const auto state = static_cast<uint32_t>(ctx->r3);
    const auto delta = static_cast<int32_t>(ctx->r5);
    if (!valid_rdram_address(object, 0x6CU) || (object & 3U) ||
        !valid_rdram_address(state, 0x18U) || (state & 1U) ||
        static_cast<uint32_t>(MEM_W(0x68, object)) != state ||
        delta < 0 || delta > 32767) return;
    const auto kind = MEM_BU(0x17, state);
    if (kind < 2 || kind > 4) return;

    ctx->r2 = std::max(static_cast<int32_t>(ctx->r2), 1) + delta * 2;
    MEM_B(0x39, object) = 255;
}

extern "C" void jfc_preserve_dropped_health(recomp_context* ctx) {
    if (!persistent_dropped_pickups.load(std::memory_order_relaxed) ||
        static_cast<int32_t>(ctx->r2) <= 0) return;

    ctx->r2 = std::max(static_cast<int32_t>(ctx->r2), 128);
    ctx->r14 = 0;
}

namespace {
bool corpse_completed(uint32_t object) {
    const uint32_t index = (object & 0x1FFFFFFFU) / 4;
    return (completed_corpses[index / 32] & (1U << (index % 32))) != 0;
}

bool is_fading_corpse(uint8_t* rdram, uint32_t object, uint32_t state, bool removing = false) {
    if (!rdram || (object & 3U) || (state & 3U) ||
        !valid_rdram_address(object, 0xA4U) ||
        !valid_rdram_address(state, 0xC8U) ||
        MEM_H(0x48, object) != 24 ||
        uint32_t(MEM_W(0x68, object)) != state ||
        (!removing && (MEM_BU(7, state) & 0x80U)) ||
        MEM_H(0xA6, state) > 0 || MEM_BU(0x55, state) == 0) return false;
    const uint32_t health = uint32_t(MEM_W(0x4C, object));
    return !(health & 3U) && valid_rdram_address(health, 8U) &&
        MEM_H(6, health) <= 0;
}

void finish_corpse_membership(uint8_t* rdram, recomp_context* ctx,
                              uint32_t object, uint32_t state,
                              recomp_func_t* tidy_squad) {

    constexpr uint32_t detached = 0x4000U;
    if (uint32_t(MEM_W(0, state)) & detached) return;
    if (!tidy_squad) throw std::runtime_error("Missing native squad retirement");
    auto retirement = *ctx;
    retirement.r4 = int32_t(object);
    tidy_squad(rdram, &retirement);
    MEM_W(0, state) = uint32_t(MEM_W(0, state)) | detached;
}
}

extern "C" void jfc_preserve_corpse_fade(uint8_t* rdram, recomp_context* ctx,
                                          recomp_func_t* remove_squaddie) {
    if (!persistent_corpses.load(std::memory_order_relaxed)) return;
    const uint32_t stack = uint32_t(ctx->r29);
    if (!rdram || (stack & 3U) || !valid_rdram_address(stack, 0x16CU)) return;
    const uint32_t object = uint32_t(MEM_W(0x168, stack));
    const uint32_t state = uint32_t(ctx->r8);
    if (!is_fading_corpse(rdram, object, state) || !corpse_completed(object)) return;
    const uint32_t index = (object & 0x1FFFFFFFU) / 4;
    if (!(retired_corpses[index / 32] & (1U << (index % 32)))) {
        if (!remove_squaddie || corpse_retirement_object)
            throw std::runtime_error("Invalid retained corpse cleanup scope");

        auto retirement = *ctx;
        retirement.r4 = int32_t(object);
        corpse_retirement_object = object;
        try { remove_squaddie(rdram, &retirement); }
        catch (...) { corpse_retirement_object = 0; throw; }
        corpse_retirement_object = 0;
    }

    MEM_B(0x55, state) = 1;
    ctx->r2 = 1;
    ctx->r4 = 0;
}

extern "C" void jfc_begin_corpse_retirement(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t object = uint32_t(ctx->r4);
    if (corpse_retirement_object) throw std::runtime_error("Nested corpse retirement");
    if (!rdram || (object & 3U) || !valid_rdram_address(object, 0x6CU)) return;
    if (is_fading_corpse(rdram, object, uint32_t(MEM_W(0x68, object))))
        corpse_retirement_object = object;
}

extern "C" void jfc_end_corpse_retirement() {
    corpse_retirement_object = 0;
}

extern "C" int jfc_corpse_reward_completed(uint32_t object) {
    return object != 0 && object == corpse_retirement_object && corpse_completed(object);
}

extern "C" int jfc_retain_completed_corpse(uint8_t* rdram, recomp_context* ctx,
                                            recomp_func_t* tidy_squad,
                                            recomp_func_t* clear_targets,
                                            recomp_func_t* clear_events) {

    const uint32_t object = uint32_t(ctx->r16);
    if (!persistent_corpses.load(std::memory_order_relaxed) ||
        !rdram || !object || object != corpse_retirement_object) return 0;
    const uint32_t state = uint32_t(MEM_W(0x68, object));
    if (!is_fading_corpse(rdram, object, state, true)) return 0;
    if (!clear_targets || !clear_events)
        throw std::runtime_error("Missing native corpse reference cleanup");

    MEM_W(0xBC, state) = 0;
    for (auto cleanup : {clear_targets, clear_events}) {
        auto retirement = *ctx;
        retirement.r4 = int32_t(object);
        cleanup(rdram, &retirement);
    }
    finish_corpse_membership(rdram, ctx, object, state, tidy_squad);
    const uint32_t index = (object & 0x1FFFFFFFU) / 4;
    completed_corpses[index / 32] |= 1U << (index % 32);
    retired_corpses[index / 32] |= 1U << (index % 32);
    MEM_B(7, state) = MEM_BU(7, state) & ~0x80U;
    MEM_B(0x55, state) = 1;
    return 1;
}

extern "C" void jfc_preserve_detached_squad_resources(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t object = uint32_t(ctx->r16), state = uint32_t(ctx->r3);
    if (rdram && !(object & 3U) && valid_rdram_address(object, 0x6CU) &&
        !(state & 3U) && valid_rdram_address(state, 0xC0U) && corpse_completed(object) &&
        uint32_t(MEM_W(0x68, object)) == state && (uint32_t(MEM_W(0, state)) & 0x4000U)) {

        ctx->r4 = 0;

        const uint32_t sound = uint32_t(MEM_W(0xBC, state));
        if (sound && ((sound & 3U) || !valid_rdram_address(sound, 0x20U) ||
                      uint32_t(MEM_W(0x1C, sound)) != state + 0xBCU))
            MEM_W(0xBC, state) = 0;
    }
}

extern "C" void jfc_forget_corpse(uint32_t object) {
    if ((object & 3U) || !valid_rdram_address(object, 0x6CU)) return;
    const uint32_t index = (object & 0x1FFFFFFFU) / 4;
    completed_corpses[index / 32] &= ~(1U << (index % 32));
    retired_corpses[index / 32] &= ~(1U << (index % 32));
}

extern "C" void jfc_corpse_opacity(uint8_t* rdram, recomp_context* ctx) {
    if (!persistent_corpses.load(std::memory_order_relaxed)) return;
    if (is_fading_corpse(rdram, uint32_t(ctx->r7), uint32_t(ctx->r12))) {

        ctx->r13 = 0;
    }
}

extern "C" void jfc_begin_default_game(uint8_t* rdram, recomp_context* ctx) {
    const auto game = uint32_t(ctx->r4);
    if (!valid_rdram_address(game, 0x538))
        throw std::runtime_error("Invalid new-game metadata owner");
    std::lock_guard lock(metadata_mutex);
    bind_game(rdram, game, -1);
}

extern "C" void jfc_finish_game_load(uint8_t* rdram, recomp_context* ctx) {
    const int slot = static_cast<int>(MEM_W(0x30, ctx->r29));
    const uint32_t game = static_cast<uint32_t>(MEM_W(0x34, ctx->r29));
    if (slot < 0 || slot >= static_cast<int>(save_slot_count) ||
        !valid_rdram_address(game, 4) || MEM_B(3, game) != slot) {
        return;
    }
    std::lock_guard lock(metadata_mutex);
    bind_game(rdram, game, slot);
}

extern "C" void jfc_copy_game_binding(uint8_t* rdram, uint32_t source, uint32_t destination) {
    if (!rdram || (source & 3U) || (destination & 3U) ||
        !valid_rdram_address(source, 0x538U) || !valid_rdram_address(destination, 0x538U) ||
        destination != uint32_t(MEM_W(0, gameplay_pointer_address)))
        throw std::runtime_error("Invalid native save selection copy");
    std::lock_guard lock(metadata_mutex);
    const auto* from = find_binding(source);
    const int destination_slot = game_slot(rdram, destination);
    if (!from || from->slot < 0 || from->slot != game_slot(rdram, source) || destination_slot < 0) {

        bind_game(rdram, destination, -1);
        return;
    }
    const auto metadata = from->metadata;

    bind_game(rdram, destination, destination_slot)->metadata = metadata;
}

extern "C" void jfc_begin_game_save(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock(metadata_mutex);
    pending_save_slot = static_cast<int>(ctx->r4);
    pending_save_game = static_cast<uint32_t>(ctx->r5);
    if (pending_save_slot >= 0 && pending_save_slot < int(save_slot_count) &&
        valid_rdram_address(pending_save_game, 4) && !find_binding(pending_save_game))
        bind_game(rdram, pending_save_game, game_slot(rdram, pending_save_game));
}

extern "C" void jfc_finish_game_save(uint8_t* rdram, recomp_context*) {
    std::lock_guard lock(metadata_mutex);
    if (pending_save_slot < 0 ||
        pending_save_slot >= static_cast<int>(save_slot_count) ||
        !valid_rdram_address(pending_save_game, 4)) {
        pending_save_slot = -1;
        pending_save_game = 0;
        return;
    }
    GameBinding* binding = find_binding(pending_save_game);
    if (binding == nullptr) {
        binding = bind_game(rdram, pending_save_game, pending_save_slot);
    }
    if (binding != nullptr) {
        slots[static_cast<size_t>(pending_save_slot)] = binding->metadata;
        binding->slot = pending_save_slot;

        for (auto& alias : bindings)
            if (&alias != binding && alias.game && alias.slot == pending_save_slot)
                alias.metadata = binding->metadata;
        persist_locked();
    }
    pending_save_slot = -1;
    pending_save_game = 0;
}

extern "C" void jfc_begin_game_clear(uint8_t*, recomp_context* ctx) {
    std::lock_guard lock(metadata_mutex);
    pending_clear_slot = static_cast<int>(ctx->r4);
}

extern "C" void jfc_finish_game_clear(uint8_t*, recomp_context* ctx) {
    std::lock_guard lock(metadata_mutex);
    if (ctx->r16 != 0x7F || pending_clear_slot < 0 ||
        pending_clear_slot >= static_cast<int>(save_slot_count)) {
        return;
    }
    slots[static_cast<size_t>(pending_clear_slot)] = {};
    for (GameBinding& binding : bindings) {
        if (binding.slot == pending_clear_slot) {
            binding = {};
        }
    }
    persist_locked();
    pending_clear_slot = -1;
}

extern "C" void jfc_finish_all_game_clear(uint8_t*, recomp_context* ctx) {
    if (ctx->r20 != 8) {
        return;
    }
    std::lock_guard lock(metadata_mutex);
    slots = {};
    bindings = {};
    persist_locked();
}

extern "C" void jfc_restore_persistent_tribals(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t level = static_cast<uint32_t>(ctx->r2);
    if (valid_rdram_address(level, 0x104)) {
        restore_persistent_region(rdram, MEM_BU(0x102, level),
            MEM_BU(0x103, level), unsigned(MEM_W(0x18, ctx->r29)) & 15U);
    }
}

extern "C" void jfc_restore_tribal_region(uint8_t* rdram) {
    restore_persistent_region(rdram, tribal_region(rdram));
}

extern "C" void jfc_record_tribal_rescue(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_W(0x1C, ctx->r29) != 1) {
        return;
    }
    const uint8_t level = current_level_id(rdram);
    const unsigned placement = static_cast<unsigned>(MEM_W(0x18, ctx->r29)) & 0xFU;
    const uint32_t game = static_cast<uint32_t>(MEM_W(0, gameplay_pointer_address));
    if (level >= tribal_level_count ||
        !valid_rdram_address(game, 0x5E + level * 2U)) {
        return;
    }
    const uint16_t mask = static_cast<uint16_t>(1U << placement);
    if ((MEM_HU(0, game + 0x5C + level * 2U) & mask) != 0) {
        record_rescue(rdram, level, placement);
    }
}

extern "C" int jfc_should_protect_tribal_object(
    uint8_t* rdram,
    uint32_t object
) {
    if (!invulnerable_tribals.load(std::memory_order_relaxed) ||
        !valid_rdram_address(object, 0x6CU) || MEM_H(0x48, object) != 24) {
        return 0;
    }

    const uint32_t object_link = static_cast<uint32_t>(MEM_W(0x68, object));
    if (!valid_rdram_address(object_link, 0x28U)) {
        return 0;
    }
    const uint32_t baddy_object = static_cast<uint32_t>(
        MEM_W(0x24, object_link));
    if (!valid_rdram_address(baddy_object, 0x6CU)) {
        return 0;
    }
    const uint32_t actor = static_cast<uint32_t>(
        MEM_W(0x68, baddy_object));
    const uint32_t health = static_cast<uint32_t>(MEM_W(0x4C, object));
    if (!valid_rdram_address(actor, 0x65U) ||
        !jfc::modernization::is_tribal_actor_type(MEM_HU(0x2C, actor)) ||
        !valid_rdram_address(health, 8U)) {
        return 0;
    }

    if (tribal_has_completed_outcome(rdram, actor)) {
        return 0;
    }
    if (MEM_H(6, health) <= 0) {
        MEM_H(6, health) = 1;
    }
    return 1;
}

extern "C" void jfc_protect_tribal_death(uint8_t* rdram, recomp_context* ctx) {
    if (SIGNED(ctx->r4) > 0) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0x168, ctx->r29));
    if (!jfc_should_protect_tribal_object(rdram, object)) {
        return;
    }
    const uint32_t state = static_cast<uint32_t>(ctx->r8);
    if (!valid_rdram_address(state, 0xA8)) {
        return;
    }
    MEM_H(0xA6, state) = 1;
    ctx->r4 = 1;
}

extern "C" void jfc_override_final_tribal_gate(
    uint8_t* rdram,
    recomp_context* ctx
) {
    if (ctx->r2 != 0) {
        return;
    }
    std::lock_guard lock(metadata_mutex);
    GameBinding* binding = active_binding(rdram);
    if (binding != nullptr && binding->metadata.final_access_granted) {
        ctx->r2 = 1;
        return;
    }
    const auto requirement = final_requirement.load(std::memory_order_relaxed);
    const int total = canonical_tribal_total(rdram);
    const int required = requirement == jfc::modernization::FinalTribalRequirement::Original
        ? total : reduced_tribal_threshold(rdram);
    if (total <= 0 || required <= 0) {
        return;
    }
    const int rescued = rescued_count(rdram, binding);
    char progress[32];
    std::snprintf(
        progress, sizeof(progress), "TRIBALS %d/%d", rescued, required);
    append_debug_hud_line(rdram, progress, 218);
    if (requirement == jfc::modernization::FinalTribalRequirement::Original ||
        rescued < required) {
        return;
    }
    ctx->r2 = 1;
    if (binding != nullptr && !binding->metadata.final_access_granted) {
        binding->metadata.final_access_granted = true;
        if (binding->slot >= 0) {
            slots[static_cast<size_t>(binding->slot)] = binding->metadata;
            persist_locked();
        }
    }
}

extern "C" void jfc_adapt_tribal_dialogue(uint8_t* rdram, uint32_t block,
                                        uint32_t size, uint32_t message, uint32_t asset) {
    if (message != 57 || asset != 10) return;
    const int required = reduced_tribal_threshold(rdram);
    if (required <= 0) return;
    if ((block & 3U) || size < 4 || size > 32768 || !valid_rdram_address(block, size))
        throw std::runtime_error("Invalid King Jeff dialogue allocation");
    const auto& original = jfc::native_data::get().tribal_requirement_clause;
    char replacement[64];
    const int length = std::snprintf(replacement, sizeof(replacement),
        "when %d Tribals are free.", required);
    if (length <= 0 || size_t(length) * 2 > original.size())
        throw std::runtime_error("Tribal requirement exceeds native dialogue capacity");
    const auto glyph = [rdram](char c) { return MEM_BU(unsigned(c) - 32, 0x800A6F04U); };
    std::array<uint32_t, 64> paragraphs{};
    size_t count = 0;
    uint32_t command = 0;
    while (command < size && MEM_BU(command, block) != 0) {
        const unsigned op = MEM_BU(command, block);
        const uint32_t bytes = op >= 1 && op <= 5 ? 4 : op <= 7 ? 8 : 0;
        if (!bytes || bytes > size - command)
            throw std::runtime_error("Invalid King Jeff dialogue command");
        if (op >= 2 && op <= 5) {
            const uint32_t offset = uint32_t(MEM_W(command, block)) & 0xFFFFFFU;
            if (offset >= size || count == paragraphs.size())
                throw std::runtime_error("Invalid King Jeff dialogue text offset");
            paragraphs[count++] = offset;
        }
        command += bytes;
    }
    if (command == size) throw std::runtime_error("Unterminated King Jeff dialogue commands");
    for (size_t paragraph = 0; paragraph < count; ++paragraph) {
        const uint32_t start = paragraphs[paragraph];
        if (start <= command) throw std::runtime_error("King Jeff text overlaps commands");
        uint32_t end = start;
        while (end < size && MEM_BU(end, block) != 0)
            end += (MEM_BU(end, block) & 0x80U) ? 2 : 1;
        if (end >= size) throw std::runtime_error("Unterminated King Jeff dialogue text");
        for (uint32_t at = start; at < end;) {
            bool match = original.size() <= end - at;
            for (size_t i = 0; match && i < original.size(); ++i)
                match = MEM_BU(at + unsigned(i), block) == original[i];
            if (match) {
                for (size_t other = 0; other < count; ++other)
                    if (paragraphs[other] > at && paragraphs[other] <= end)
                        throw std::runtime_error("King Jeff dialogue has overlapping text owners");
                for (int i = 0; i < length; ++i) {
                    MEM_B(at + i * 2, block) = 0x80;
                    MEM_B(at + i * 2 + 1, block) = glyph(replacement[i]);
                }
                const unsigned removed = unsigned(original.size() - size_t(length) * 2);
                for (uint32_t read = at + unsigned(original.size()); read <= end; ++read)
                    MEM_B(read - removed, block) = MEM_BU(read, block);
                for (uint32_t clear = end - removed + 1; clear <= end; ++clear)
                    MEM_B(clear, block) = 0;
                return;
            }
            at += (MEM_BU(at, block) & 0x80U) ? 2 : 1;
        }
    }
    throw std::runtime_error("King Jeff dialogue requirement clause was not found");
}

extern "C" int jfc_prepare_quick_character_switch(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const bool quick_pressed = update_actions();
    if (!quick_pressed ||
        quick_grenade.phase != 0 ||
        !quick_character_switching.load(std::memory_order_relaxed) ||
        ctx->r2 != 0 || MEM_W(0, 0x800FD7C4U) != 0 ||
        MEM_B(0, 0x800FD7BDU) != 0 || MEM_B(0, 0x800A3294U) != 0 ||
        MEM_BU(0, 0x800A4FC4U) != 0 || MEM_W(0, 0x800A3328U) != 0 ||
        cinematic_active(rdram)) {
        return 0;
    }
    const uint32_t game = static_cast<uint32_t>(
        MEM_W(0, gameplay_pointer_address));
    if (!valid_rdram_address(game, 0x15CU + 3U * 0x76U)) {
        return 0;
    }
    const int current = MEM_H(0, 0x800A3260U);
    if (current < 0 || current > 2) {
        return 0;
    }
    for (int offset = 1; offset < 3; ++offset) {
        const int target = (current + offset) % 3;
        if (!game_flag(rdram, game, static_cast<unsigned>(target)) ||
            game_flag(rdram, game, static_cast<unsigned>(target + 0x5A))) {
            continue;
        }
        const uint32_t record = game + 0x15CU +
            static_cast<uint32_t>(target) * 0x76U;
        const uint16_t packed = MEM_HU(0x74, record);
        ctx->r4 = packed >> 5;
        ctx->r5 = target;
        ctx->r6 = packed & 0x1FU;
        ctx->r8 = MEM_HU(0x4, record) & 0x1FU;
        return 1;
    }
    return 0;
}

extern "C" void jfc_commit_quick_character_switch(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t game = static_cast<uint32_t>(
        MEM_W(0, gameplay_pointer_address));
    const int target = static_cast<int>(ctx->r5);
    if (target < 0 || target > 2 ||
        !valid_rdram_address(game, 0x15CU + 3U * 0x76U)) {
        return;
    }
    const uint32_t record = game + 0x15CU +
        static_cast<uint32_t>(target) * 0x76U;
    const uint16_t packed = static_cast<uint16_t>(
        (static_cast<uint32_t>(ctx->r4) << 5) |
        (static_cast<uint32_t>(ctx->r6) & 0x1FU));
    MEM_H(0x74, record) = packed;
    MEM_B(0x75, record) = (MEM_BU(0x75, record) & 0xE0U) |
        (static_cast<uint32_t>(ctx->r6) & 0x1FU);
    MEM_B(0x5, record) = (MEM_BU(0x5, record) & 0xE0U) |
        (static_cast<uint32_t>(ctx->r8) & 0x1FU);
}

namespace {

void select_weapon(uint8_t* rdram, const recomp_context& context,
                   uint32_t object, uint32_t player, unsigned weapon) {
    recomp_context call;
    jfc::runtime::copy_context(call, context);
    call.r29 = ADD32(call.r29, -0x20);
    call.r4 = int32_t(object); call.r5 = int32_t(player); call.r6 = weapon;
    controlSelectWeapon(rdram, &call);
    if (call.r2 != 0) {
        MEM_B(0x585, player) = weapon;
        MEM_B(0x538, player) = 0;
        MEM_B(0x53D, player) = 0;

        MEM_B(0x540, player) = MEM_BU(0x540, player) & ~0x30U;
    }
}

bool shortcut_owner(uint8_t* rdram, uint32_t object, uint32_t player, uint32_t& record) {
    const uint32_t game = uint32_t(MEM_W(0, gameplay_pointer_address));
    if ((object & 3U) || (player & 3U) || (game & 3U) ||
        !valid_rdram_address(object, 0x6C) || !valid_rdram_address(player, 0x5D8) ||
        !valid_rdram_address(game, 0x15C + 3 * 0x76) ||
        uint32_t(MEM_W(0x68, object)) != player || MEM_B(0, player) != 0) return false;
    const unsigned character = MEM_BU(1, player) & 3U;
    if (character >= 3 || character != (MEM_HU(0, current_character_address) & 3U)) return false;
    record = game + 0x15C + character * 0x76;
    return true;
}

bool shortcut_allowed(uint8_t* rdram, uint32_t object, uint32_t player) {
    const uint32_t hit = uint32_t(MEM_W(0x4C, object));
    return MEM_BU(0, frontend_mode_address) == 16 && MEM_BU(0, 0x800A4FC4U) == 0 &&
        MEM_W(0, 0x800FD7C4U) == 0 && MEM_B(0, 0x800FD7BDU) == 0 &&
        MEM_B(0, 0x800A3294U) == 0 && MEM_W(0, 0x800A3328U) == 0 &&
        jfc_allow_player_weapon_cycle(rdram, player) &&
        !(hit & 1U) && valid_rdram_address(hit, 8) && MEM_H(6, hit) >= 256;
}

bool throw_animation(uint8_t* rdram, uint32_t object, uint32_t player) {
    const unsigned character = MEM_BU(1, player) & 3U;
    const unsigned clip = MEM_BU(0x3B, object);

    constexpr std::array<std::array<uint8_t, 2>, 3> clips{{{22,49}, {21,46}, {14,27}}};
    return character < clips.size() && (clip == clips[character][0] || clip == clips[character][1]);
}

void finish_quick_grenade(uint8_t* rdram, const recomp_context& context) {
    const auto owned = quick_grenade;
    quick_grenade = {};
    uint32_t record = 0;
    if (owned.phase && shortcut_owner(rdram, owned.object, owned.player, record) &&
        record == owned.record && MEM_BU(0x70, record) == owned.weapon &&
        (MEM_HU(0xA, record) & (1U << owned.previous_weapon)))
        select_weapon(rdram, context, owned.object, owned.player, owned.previous_weapon);
}
}

extern "C" int jfc_weapon_shortcut_busy(uint32_t player) {
    return quick_grenade.phase != 0 && quick_grenade.player == player;
}

extern "C" void jfc_begin_weapon_shortcuts(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t player = uint32_t(ctx->r16), stack = uint32_t(ctx->r29);
    if (!valid_rdram_address(stack, 0x98)) return;
    const uint32_t object = uint32_t(MEM_W(0x90, stack));
    uint32_t record = 0;
    if (!shortcut_owner(rdram, object, player, record)) return;
    const uint16_t pressed = std::exchange(pressed_weapon_actions, 0);
    if (quick_grenade.phase) {

        if (quick_grenade.object != object || quick_grenade.player != player) {
            const auto old = quick_grenade;
            quick_grenade = {};
            if (old.record == record && MEM_BU(0x70, record) == old.weapon)
                select_weapon(rdram, *ctx, object, player, old.previous_weapon);
        }
        return;
    }
    if (!pressed || !shortcut_allowed(rdram, object, player) ||
        MEM_B(0x53D, player) != 0 || MEM_BU(0x577, player) != 0 ||
        MEM_BU(0x56B, player) != 0 || throw_animation(rdram, object, player)) return;
    const unsigned equipped = MEM_BU(0x70, record);
    if (equipped >= 16) return;
    const unsigned character = MEM_BU(1, player) & 3U;
    if (pressed & 0x200U) {
        if (!jfc::modernization::is_quick_throwable(equipped) ||
            !(MEM_HU(0xA, record) & (1U << equipped))) return;
        std::lock_guard lock(metadata_mutex);
        auto* binding = active_binding(rdram);
        if (!binding) return;
        auto& assigned = binding->metadata.throwables[character];
        if (assigned == equipped) return;
        const auto previous = assigned;
        assigned = uint8_t(equipped);
        if (binding->slot >= 0) {
            auto& saved = slots[size_t(binding->slot)].throwables[character];
            const auto old_saved = saved;
            saved = assigned;
            if (!persist_locked()) { saved = old_saved; assigned = previous; }
        }
        return;
    }
    if (pressed & 0xFFU) {
        const uint16_t eligible = (pressed & 0xF0U) ? (pressed & 0xF0U) : (pressed & 0xFU);
        std::lock_guard lock(metadata_mutex);
        GameBinding* binding = active_binding(rdram);
        if (binding == nullptr) return;
        auto& shortcuts = binding->metadata.weapon_slots[character];
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (eligible & (0x10U << slot)) {
                if (!(MEM_HU(0xA, record) & (1U << equipped)) || shortcuts[slot] == equipped + 1) return;
                const auto previous = shortcuts[slot];
                shortcuts[slot] = uint8_t(equipped + 1);
                if (binding->slot >= 0) {
                    auto& saved = slots[size_t(binding->slot)].weapon_slots[character][slot];
                    const auto previous_saved = saved;
                    saved = shortcuts[slot];
                    if (!persist_locked()) { saved = previous_saved; shortcuts[slot] = previous; }
                }
                return;
            } else if (eligible & (1U << slot)) {
                const unsigned encoded = shortcuts[slot];
                if (encoded && encoded <= 16 && (MEM_HU(0xA, record) & (1U << (encoded - 1))))
                    select_weapon(rdram, *ctx, object, player, encoded - 1);
                return;
            }
        }
        return;
    }
    const auto throwable = jfc::modernization::weapon_shortcuts(rdram, character).throwable;
    if ((pressed & 0x100U) && (MEM_HU(0xA, record) & (1U << throwable)) &&
        MEM_H(0x14 + 2 * throwable, record) > 0 && MEM_B(0x538, player) == 0 &&
        !(MEM_BU(0x540, player) & 0x80U)) {
        select_weapon(rdram, *ctx, object, player, throwable);
        if (MEM_BU(0x70, record) == throwable)
            quick_grenade = {object, player, record, uint8_t(equipped), 1, false, throwable};
    }
}

extern "C" void jfc_begin_quick_grenade_input(uint8_t* rdram, recomp_context* ctx) {
    if (!quick_grenade.phase || uint32_t(ctx->r4) != quick_grenade.object ||
        uint32_t(ctx->r5) != quick_grenade.player) return;
    const uint32_t table = uint32_t(MEM_W(0, 0x800A18B0U));
    if ((table & 3U) || !valid_rdram_address(table, 4)) return;
    const uint32_t fire = uint32_t(MEM_W(0, table));
    if (fire == 0 || (fire & ~0xFFFFU)) return;
    if (grenade_input_scoped) std::terminate();
    grenade_input_scoped = true;
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t address = 0x800F6DA0U + 4 * i;
        grenade_saved_input[i] = MEM_W(0, address);
        MEM_W(0, address) &= ~fire;
    }
    if (quick_grenade.phase == 1) {
        MEM_W(0, 0x800F6DA0U) |= fire;
        MEM_W(0, 0x800F6DA4U) |= fire;
    }
}

extern "C" void jfc_end_quick_grenade_input(uint8_t* rdram, recomp_context* ctx) {
    if (!grenade_input_scoped) return;
    for (unsigned i = 0; i < 3; ++i) MEM_W(0, 0x800F6DA0U + 4 * i) = grenade_saved_input[i];
    grenade_input_scoped = false;
    if (quick_grenade.phase == 1 || quick_grenade.phase == 3) {
        if (int32_t(ctx->r2) == quick_grenade.weapon) quick_grenade.phase = 2;
        else if (quick_grenade.phase == 1 && (MEM_BU(0x540, quick_grenade.player) & 0x80U))
            quick_grenade.phase = 3;
        else quick_grenade.phase = 1;
    }
}

extern "C" void jfc_end_weapon_shortcuts(uint8_t* rdram, recomp_context* ctx) {
    if (!quick_grenade.phase || uint32_t(ctx->r16) != quick_grenade.player) return;
    auto& g = quick_grenade;
    const bool throwing = throw_animation(rdram, g.object, g.player);
    if (throwing) g.animation_seen = true;
    if (g.phase == 1 ||
        (g.phase == 2 && g.animation_seen && !throwing && MEM_B(0x53D, g.player) == 0 &&
         MEM_BU(0x577, g.player) == 0)) finish_quick_grenade(rdram, *ctx);
}

extern "C" void jfc_begin_sprint_control(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t player = static_cast<uint32_t>(ctx->r16);
    const uint32_t stack = static_cast<uint32_t>(ctx->r29);
    if (!valid_rdram_address(stack, 0x98U) ||
        !valid_rdram_address(player, 0x5C4U)) {
        sprint_multiplier = 1.0f;
        sprint_player = 0;
        sprint_object = 0;
        return;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0x90, stack));
    if (!valid_rdram_address(object, 0x6CU) ||
        static_cast<uint32_t>(MEM_W(0x68, object)) != player) {
        sprint_multiplier = 1.0f;
        sprint_player = sprint_object = 0;
        return;
    }
    const bool compatible = sprint_compatible(rdram, player);
    if (!compatible || sprint_mode.load(std::memory_order_relaxed) ==
                           jfc::modernization::SprintMode::Off) {
        sprint_multiplier = 1.0f;
    }
    else {
        sprint_multiplier = jfc::modernization::advance_sprint_multiplier(
            sprint_object == object && sprint_player == player ? sprint_multiplier : 1.0f,
            sprint_requested(), static_cast<float>(MEM_W(0x94, stack)));
    }
    sprint_player = player;
    sprint_object = object;
}

extern "C" float jfc_sprint_speed_operand(
    uint8_t* rdram, uint32_t player, float native_operand
) {

    if (player != sprint_player || sprint_multiplier == 1.0f ||
        !sprint_compatible(rdram, player) || !std::isfinite(native_operand)) {
        return native_operand;
    }
    return native_operand * sprint_multiplier;
}

extern "C" void jfc_begin_animseq_group(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const int32_t sequence_group = static_cast<int32_t>(SIGNED(ctx->r4));
    active_sequence_route = make_sequence_route(rdram, sequence_group);
    active_sequence_key = sequence_identity_key(active_sequence_route);
    active_legacy_sequence_key = legacy_sequence_key(
        current_level_id(rdram), sequence_group);
    active_sequence_kind = current_sequence_kind(rdram);
    clear_pending_sequence();
    if (active_sequence_kind != SequenceKind::None &&
        active_sequence_key != 0) {
        pending_sequence_key = active_sequence_key;
        pending_sequence_route = active_sequence_route;
        pending_sequence_kind = active_sequence_kind;
    }
    seen_sequence_skip_applied = false;
    animseq_previous_state = MEM_W(0, animseq_state_address);

}

extern "C" int jfc_prepare_seen_animseq_skip(
    uint8_t* rdram,
    recomp_context*
) {
    refresh_active_travel_route(rdram);
    animseq_previous_state = MEM_W(0, animseq_state_address);
    if (seen_sequence_skip_applied || animseq_previous_state == 0 ||
        active_sequence_kind == SequenceKind::None ||
        (active_sequence_key == 0 && active_legacy_sequence_key == 0)) {
        return 0;
    }

    std::lock_guard lock(metadata_mutex);
    const GameBinding* binding = active_binding(rdram);
    if (binding == nullptr) {
        return 0;
    }

    const bool enabled = active_sequence_kind == SequenceKind::Landing
        ? skip_seen_landings.load(std::memory_order_relaxed)
        : skip_seen_character_travel.load(std::memory_order_relaxed);
    const auto& history = active_sequence_kind == SequenceKind::Landing
        ? binding->metadata.seen_landings : binding->metadata.seen_travel;
    const bool seen = enabled && contains_key(
        history, active_sequence_key, active_legacy_sequence_key);
    seen_sequence_skip_applied = seen;
    if (seen) {

    }
    return seen ? 1 : 0;
}

extern "C" void jfc_classify_animseq_transition(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const SequenceKind kind = active_sequence_kind != SequenceKind::None
        ? active_sequence_kind : current_sequence_kind(rdram);

    if (kind == SequenceKind::None || SIGNED(ctx->r4) < 0 ||
        SIGNED(ctx->r4) > 0x1FF || SIGNED(ctx->r5) < 0 ||
        SIGNED(ctx->r5) > 3 || SIGNED(ctx->r6) < 0 ||
        SIGNED(ctx->r6) > 0x1F || SIGNED(ctx->r7) < 0 ||
        SIGNED(ctx->r7) > 0x7F || SIGNED(ctx->r20) < 0 ||
        SIGNED(ctx->r20) > 0xFF) {
        return;
    }

    SequenceRoute exact_route = active_sequence_route;
    exact_route.destination = {
        static_cast<int>(SIGNED(ctx->r4)),
        static_cast<int>(SIGNED(ctx->r5)),
        static_cast<int>(SIGNED(ctx->r6)),
        static_cast<int>(SIGNED(ctx->r20)),
        static_cast<int>(SIGNED(ctx->r7)),
    };
    const uint64_t exact_key = sequence_identity_key(exact_route);
    if (exact_key == 0) {
        return;
    }
    active_sequence_route = exact_route;
    active_sequence_key = exact_key;
    active_sequence_kind = kind;
    pending_sequence_key = exact_key;
    pending_sequence_route = exact_route;
    pending_sequence_kind = kind;
}

extern "C" void jfc_finish_animseq_update(
    uint8_t* rdram,
    recomp_context*
) {
    const int32_t state = MEM_W(0, animseq_state_address);
    if (animseq_previous_state != 0 && state == 0 &&
        pending_sequence_key != 0 &&
        pending_sequence_kind == SequenceKind::Landing) {

        record_seen_sequence(
            rdram, pending_sequence_kind, pending_sequence_key);
        clear_pending_sequence();
    }
    animseq_previous_state = state;
}

extern "C" void jfc_log_level_request(uint8_t* rdram, recomp_context* ctx) {

    const TransitionState source = current_transition_state(rdram);

    std::fflush(stderr);
}

extern "C" void jfc_end_game_save(uint8_t*, recomp_context*) {

    std::lock_guard lock(metadata_mutex);
    pending_save_slot = -1;
    pending_save_game = 0;
}

extern "C" void jfc_observe_completed_level_load(
    uint8_t* rdram,
    recomp_context*
) {

    const TransitionState loaded = current_transition_state(rdram);

    std::fflush(stderr);

    if (!valid_transition_state(loaded) ||
        static_cast<int>(MEM_BU(0, frontend_mode_address)) != loaded.mode) {
        if (pending_sequence_kind == SequenceKind::Travel) {

            clear_pending_sequence();
        }
        committed_transition = {};
        committed_transition_valid = false;
        return;
    }

    if (pending_sequence_kind == SequenceKind::Travel) {
        const bool source_matches = committed_transition_valid &&
            same_transition_state(
                pending_sequence_route.source, committed_transition);
        const bool destination_matches = same_transition_state(
            pending_sequence_route.destination, loaded);
        const bool completed = source_matches && destination_matches &&
            pending_sequence_route.source.mode == 5 && loaded.mode != 5;
        if (completed) {

            record_seen_sequence(
                rdram, pending_sequence_kind, pending_sequence_key);
            clear_pending_sequence();
        }
        else if (!source_matches ||
                 !same_transition_state(
                     pending_sequence_route.source, loaded)) {

            clear_pending_sequence();
        }
    }

    committed_transition = loaded;
    committed_transition_valid = true;
}

extern "C" void jfc_autosave_completed_level_load(uint8_t* rdram, recomp_context* ctx,
                                                 recomp_func_t* request_save) {
    if (!rdram) return;
    const auto loaded = current_transition_state(rdram);

    if (!valid_transition_state(loaded) || (loaded.mode != 16 && loaded.mode != 5) ||
        loaded.character > 2 || MEM_BU(0, frontend_mode_address) != loaded.mode ||
        MEM_W(0, 0x800FD7C4U) != 0) return;
    const uint32_t game = uint32_t(MEM_W(0, gameplay_pointer_address));
    if ((game & 3U) || !valid_rdram_address(game, 0x580U)) return;
    int slot = -1;
    {
        std::lock_guard lock(metadata_mutex);
        const auto* binding = find_binding(game);

        if (!binding || binding->slot < 0 || binding->slot != game_slot(rdram, game)) return;
        slot = binding->slot;
    }
    const uint32_t pending = uint32_t(MEM_W(0, 0x800A32A8U));

    if ((pending & 0xBD000000U) ||
        ((pending & 0x40000000U) && ((pending >> 3) & 7U) != uint32_t(slot))) return;
    if (!request_save) throw std::runtime_error("Missing native transition save request");
    auto save = *ctx;
    save.r4 = slot;
    request_save(rdram, &save);
}

extern "C" void jfc_track_ship_part_collectable(
    uint8_t* rdram,
    recomp_context* ctx
) {
    if (ship_part_scanner.load(std::memory_order_relaxed) ==
        jfc::modernization::ShipPartScanner::Off) {
        return;
    }
    const uint32_t record = static_cast<uint32_t>(ctx->r20);
    const uint32_t object = static_cast<uint32_t>(ctx->r9);
    if (!valid_rdram_address(record, 8) ||
        !valid_rdram_address(object, 0x8CU) ||
        MEM_BU(4, record) != 0 || MEM_BU(5, record) != 4 ||
        MEM_BU(7, record) != 0 || MEM_W(0x88, object) == 0) {
        return;
    }
    const unsigned flag = MEM_BU(6, record);
    if (flag < first_ship_part_flag || flag > last_ship_part_flag) {
        return;
    }
    const uint32_t game = static_cast<uint32_t>(
        MEM_W(0, gameplay_pointer_address));
    const uint8_t level = current_level_id(rdram);
    if (!valid_rdram_address(game, 0x36U) || level == 0xFFU ||
        game_flag(rdram, game, flag)) {
        return;
    }
    const float x = read_float(rdram, object + 0xCU);
    const float y = read_float(rdram, object + 0x10U);
    const float z = read_float(rdram, object + 0x14U);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return;
    }
    if (scanner_level != level) {
        reset_scanner_tracking(level);
    }
    tracked_ship_parts[flag - first_ship_part_flag] = {
        level, object, x, y, z, scanner_frame};
}

extern "C" int jfc_update_ship_part_scanner(
    uint8_t* rdram,
    recomp_context*
) {
    const auto mode = ship_part_scanner.load(std::memory_order_relaxed);
    const uint8_t level = current_level_id(rdram);
    if (scanner_level != level) {
        reset_scanner_tracking(level);
    }
    const auto finish_frame = [](int sound) {
        advance_scanner_frame();
        return sound;
    };
    if (mode == jfc::modernization::ShipPartScanner::Off ||
        level == 0xFFU || !scanner_gameplay_active(rdram)) {
        scanner_audio_target = 0xFFU;
        scanner_audio_countdown = 0;
        return finish_frame(0);
    }

    const uint32_t game = static_cast<uint32_t>(
        MEM_W(0, gameplay_pointer_address));
    const uint32_t player_object = first_player_object(rdram);
    if (!valid_rdram_address(game, 0x36U) || player_object == 0) {
        return finish_frame(0);
    }
    const float player_x = read_float(rdram, player_object + 0xCU);
    const float player_y = read_float(rdram, player_object + 0x10U);
    const float player_z = read_float(rdram, player_object + 0x14U);
    const uint32_t player = static_cast<uint32_t>(
        MEM_W(0x68, player_object));
    if (!std::isfinite(player_x) || !std::isfinite(player_y) ||
        !std::isfinite(player_z) ||
        !valid_rdram_address(player, 0x106U)) {
        return finish_frame(0);
    }

    const int character = MEM_H(0, 0x800A3260U);
    const ShipPartObjective* local_objective = nullptr;
    const TrackedShipPart* local_target = nullptr;
    float nearest_distance_squared = std::numeric_limits<float>::max();
    for (size_t index = 0; index < tracked_ship_parts.size(); ++index) {
        const TrackedShipPart& tracked = tracked_ship_parts[index];
        const ShipPartObjective& objective = ship_part_objectives[index];
        if (tracked.level != level || tracked.frame != scanner_frame ||
            game_flag(rdram, game, objective.flag) ||
            !ship_part_character_eligible(character, objective) ||
            ship_part_objective_score(rdram, game, character, objective) == 2) {
            continue;
        }
        const float dx = tracked.x - player_x;
        const float dy = tracked.y - player_y;
        const float dz = tracked.z - player_z;
        const float distance_squared = dx * dx + dy * dy + dz * dz;
        if (std::isfinite(distance_squared) &&
            distance_squared < nearest_distance_squared) {
            nearest_distance_squared = distance_squared;
            local_objective = &objective;
            local_target = &tracked;
        }
    }

    const bool visual = mode == jfc::modernization::ShipPartScanner::Visual ||
        mode == jfc::modernization::ShipPartScanner::AudioVisual;
    const bool audio = mode == jfc::modernization::ShipPartScanner::Audio ||
        mode == jfc::modernization::ShipPartScanner::AudioVisual;

    char line[48]{};
    if (local_target != nullptr) {
        if (visual) {
            const float dx = local_target->x - player_x;
            const float dz = local_target->z - player_z;
            std::snprintf(line, sizeof(line), "SCAN %s %s %s",
                scanner_direction(dx, dz, MEM_H(0x104, player)),
                local_objective->name,
                scanner_range(nearest_distance_squared));
            append_debug_hud_line(rdram, line, 230);
        }
        if (!audio) {
            scanner_audio_target = 0xFFU;
            scanner_audio_countdown = 0;
            return finish_frame(0);
        }
        if (scanner_audio_target != local_objective->flag) {
            scanner_audio_target = local_objective->flag;
            scanner_audio_countdown = 0;
        }
        if (scanner_audio_countdown == 0) {
            scanner_audio_countdown = scanner_ping_interval(
                nearest_distance_squared);
            return finish_frame(scanner_sound_id);
        }
        --scanner_audio_countdown;
        return finish_frame(0);
    }

    scanner_audio_target = 0xFFU;
    scanner_audio_countdown = 0;
    if (!visual) {
        return finish_frame(0);
    }
    int score = 4;
    const ShipPartObjective* objective = next_ship_part_objective(
        rdram, game, character, score);
    if (objective == nullptr) {
        return finish_frame(0);
    }
    if (score == 3) {
        std::snprintf(line, sizeof(line), "SCAN %s | NEED %s",
            objective->name,
            required_character_name(objective->characters));
    }
    else if (score == 2) {
        std::snprintf(line, sizeof(line), "SCAN %s | NEED %s",
            objective->name, objective->route_requirement);
    }
    else if (score == 1) {

        std::snprintf(line, sizeof(line), "SCAN %s | MAP %s",
            objective->name, objective->destination);
        append_debug_hud_line(rdram, line, 218);
        std::snprintf(line, sizeof(line), "NEED %s -> %s",
            objective->route_requirement, objective->action_source);
    }
    else if (!ship_part_is_direct(*objective)) {
        std::snprintf(line, sizeof(line), "SCAN %s | TALK %s",
            objective->name, objective->action_source);
    }
    else {
        std::snprintf(line, sizeof(line), "SCAN %s | %s",
            objective->name, objective->destination);
    }
    append_debug_hud_line(rdram, line, 230);
    return finish_frame(0);
}

#define JFC_MODERNIZATION_STATE_FIELDS(X) \
    X(slots) X(bindings) X(pending_save_slot) X(pending_save_game) X(pending_clear_slot) \
    X(actions_initialized) X(previous_sprint_action) X(previous_quick_switch_action) \
    X(sprint_toggled) X(previous_sprint_mode) X(sprint_player) X(sprint_object) X(sprint_multiplier) \
    X(animseq_previous_state) X(active_sequence_key) X(active_legacy_sequence_key) \
    X(active_sequence_route) X(active_sequence_kind) X(pending_sequence_key) \
    X(pending_sequence_route) X(pending_sequence_kind) X(seen_sequence_skip_applied) \
    X(committed_transition) X(committed_transition_valid) X(tracked_ship_parts) \
    X(scanner_frame) X(scanner_level) X(scanner_audio_target) X(scanner_audio_countdown) \
    X(health_recharge) X(quick_grenade) X(previous_weapon_actions) X(pressed_weapon_actions) \
    X(completed_corpses)

struct jfc::state::PreparedModernization::Impl {
#define DECLARE_FIELD(name) decltype(::name) name{};
    JFC_MODERNIZATION_STATE_FIELDS(DECLARE_FIELD)
#undef DECLARE_FIELD
    jfc::weapons::State weapons{};
    bool committed = false;
};
namespace {
template<class Archive> void transfer_slot_state(Archive& a, SlotMetadata& slot) {
    for (auto& value : slot.rescued_tribals) a.fields(value);
    for (auto& value : slot.seen_landings) a.fields(value);
    for (auto& value : slot.seen_travel) a.fields(value);
    a.fields(slot.final_access_granted);
}
template<class Archive> void transfer_transition_state(Archive& a, TransitionState& s) {
    a.fields(s.level, s.character, s.region, s.group, s.mode);
}
template<class Archive> void transfer_route_state(Archive& a, SequenceRoute& route) {
    transfer_transition_state(a, route.source); transfer_transition_state(a, route.destination);
    a.fields(route.sequence_group);
}
template<class Archive, class State> void transfer_modernization_state(Archive& a, State& s, uint32_t version) {
    for (auto& slot : s.slots) transfer_slot_state(a, slot);
    for (auto& binding : s.bindings) { a.fields(binding.game, binding.slot); transfer_slot_state(a, binding.metadata); }
    a.fields(s.pending_save_slot, s.pending_save_game, s.pending_clear_slot,
        s.actions_initialized, s.previous_sprint_action, s.previous_quick_switch_action,
        s.sprint_toggled, s.previous_sprint_mode, s.sprint_player, s.sprint_object, s.sprint_multiplier,
        s.animseq_previous_state, s.active_sequence_key, s.active_legacy_sequence_key);
    transfer_route_state(a, s.active_sequence_route);
    a.fields(s.active_sequence_kind, s.pending_sequence_key);
    transfer_route_state(a, s.pending_sequence_route);
    a.fields(s.pending_sequence_kind, s.seen_sequence_skip_applied);
    transfer_transition_state(a, s.committed_transition); a.fields(s.committed_transition_valid);
    for (auto& part : s.tracked_ship_parts) a.fields(part.level, part.object, part.x, part.y, part.z, part.frame);
    a.fields(s.scanner_frame, s.scanner_level, s.scanner_audio_target, s.scanner_audio_countdown);
    if (version >= 2) {
        auto& h = s.health_recharge;
        a.fields(h.object, h.hit, h.record, h.last_health, h.capacity, h.elapsed, h.deficit, h.damaged);
    }
    if (version >= 3) {
        for (auto& slot : s.slots) for (auto& character : slot.weapon_slots)
            for (auto& weapon : character) a.fields(weapon);
        for (auto& binding : s.bindings) for (auto& character : binding.metadata.weapon_slots)
            for (auto& weapon : character) a.fields(weapon);
        auto& g = s.quick_grenade;
        a.fields(g.object, g.player, g.record, g.previous_weapon, g.phase, g.animation_seen,
                 s.previous_weapon_actions, s.pressed_weapon_actions);
    }
    if (version >= 4) {
        for (auto& slot : s.slots) for (auto& weapon : slot.throwables) a.fields(weapon);
        for (auto& binding : s.bindings) for (auto& weapon : binding.metadata.throwables) a.fields(weapon);
        a.fields(s.quick_grenade.weapon);
    }
    if (version >= 5) {
        for (auto& slot : s.slots) for (auto& region : slot.regional_rescues)
            for (auto& value : region) a.fields(value);
        for (auto& binding : s.bindings) for (auto& region : binding.metadata.regional_rescues)
            for (auto& value : region) a.fields(value);
    }
    if (version >= 6) for (auto& value : s.completed_corpses) a.fields(value);
    if (version >= 7) {
        for (auto& value : s.weapons.flame_burns) a.fields(value);
        for (auto& value : s.weapons.flame_half_round) a.fields(value);
        for (auto& chain : s.weapons.chains) {
            a.fields(chain.root, chain.owner);
            for (auto& byte : chain.spawn) a.fields(byte);
            for (auto& arc : chain.arcs) a.fields(arc.beam, arc.source, arc.target);
        }
    }
}
constexpr size_t modernization_state_v5_size =
    19500 + (save_slot_count + binding_count) * canonical_region_count * tribal_level_count * 4;

constexpr size_t weapon_state_v7_size = 4 * (65536 + 16 + 4 * (2 + 44 + 3 * 3));
constexpr std::array<size_t, 7> modernization_state_sizes{
    18592, 18624, 19328, 19500,
    modernization_state_v5_size, modernization_state_v5_size + sizeof(completed_corpses),
    modernization_state_v5_size + sizeof(completed_corpses) + weapon_state_v7_size};
void validate_slot_state(const SlotMetadata& slot) {
    for (auto weapon : slot.throwables)
        if (!jfc::modernization::is_quick_throwable(weapon)) throw std::runtime_error("Invalid saved quick throwable");
    for (const auto& character : slot.weapon_slots) for (auto weapon : character)
        if (weapon > 16) throw std::runtime_error("Invalid saved weapon shortcut");
    const auto valid_key = [](uint64_t key) {
        return (key <= maximum_scene_identity_key) ||
            ((key & legacy_scene_key_bit) && (key & ~legacy_scene_key_bit) > 0 && (key & ~legacy_scene_key_bit) <= UINT16_MAX);
    };
    for (auto key : slot.seen_landings) if (!valid_key(key)) throw std::runtime_error("Invalid saved landing identity");
    for (auto key : slot.seen_travel) if (!valid_key(key)) throw std::runtime_error("Invalid saved travel identity");
}
template<class State> void validate_modernization_state(const State& s) {
    jfc::weapons::validate_state(s.weapons);
    const auto address = [](uint32_t value, uint32_t length) {
        if (value != 0 && (!valid_rdram_address(value, length) || (value & 3U) != 0))
            throw std::runtime_error("Invalid save-state modernization owner address");
    };
    const auto slot_id = [](int slot) {
        if (slot < -1 || slot >= int(save_slot_count)) throw std::runtime_error("Invalid save-state game slot");
    };
    const HealthRecharge& h = s.health_recharge;
    const auto& g = s.quick_grenade;
    address(g.object, 0x6C); address(g.player, 0x5D8);
    if (g.phase > 3 || g.previous_weapon >= 16 || !jfc::modernization::is_quick_throwable(g.weapon) ||
        (s.previous_weapon_actions & ~0x3FFU) || (s.pressed_weapon_actions & ~0x3FFU) ||
        (g.phase && (!g.object || !g.player || (g.record & 1U) || !valid_rdram_address(g.record, 0x76))) ||
        (!g.phase && (g.object || g.player || g.record || g.previous_weapon || g.animation_seen)))
        throw std::runtime_error("Invalid quick grenade continuation");
    address(h.object, 0x6C);
    const auto halfword_address = [](uint32_t p, uint32_t size) {
        return p != 0 && !(p & 1U) && valid_rdram_address(p, size);
    };
    if (h.object == 0) {
        if (h.hit || h.record || h.last_health != -1 || h.capacity || h.elapsed || h.deficit || h.damaged)
            throw std::runtime_error("Invalid inactive health recharge state");
    } else if (!halfword_address(h.hit, 8) || !halfword_address(h.record, 0x76) ||
        h.last_health < INT16_MIN || h.last_health > INT16_MAX ||
        h.capacity < 4 * 256 || h.capacity > 64 * 256 || (h.capacity - 4 * 256) % (5 * 256) ||
        h.elapsed < 0 || h.elapsed > 90 * 60 || h.deficit < 0 || h.deficit > h.capacity - 256 ||
        (h.elapsed < 30 * 60 && h.deficit != 0) ||
        (h.elapsed >= 30 * 60 && h.deficit == 0) || (h.damaged && h.elapsed != 0)) {
        throw std::runtime_error("Invalid health recharge state");
    }
    for (const auto& slot : s.slots) validate_slot_state(slot);
    for (size_t i = 0; i < s.bindings.size(); ++i) {
        const auto& binding = s.bindings[i];
        address(binding.game, 0x15c); slot_id(binding.slot); validate_slot_state(binding.metadata);
        if (binding.game != 0) {
            for (size_t j = 0; j < i; ++j) if (s.bindings[j].game == binding.game)
                throw std::runtime_error("Duplicate save-state game binding");
        }
    }
    slot_id(s.pending_save_slot); slot_id(s.pending_clear_slot);
    address(s.pending_save_game, 0x15c); address(s.sprint_player, 0x5c4); address(s.sprint_object, 4);
    if (s.previous_sprint_mode < jfc::modernization::SprintMode::Off || s.previous_sprint_mode > jfc::modernization::SprintMode::Toggle ||
        !std::isfinite(s.sprint_multiplier) || s.sprint_multiplier < 1.0f || s.sprint_multiplier > 1.5f)
        throw std::runtime_error("Invalid save-state sprint state");
    if (s.active_sequence_kind < SequenceKind::None || s.active_sequence_kind > SequenceKind::Travel ||
        s.pending_sequence_kind < SequenceKind::None || s.pending_sequence_kind > SequenceKind::Travel ||
        s.active_sequence_key != sequence_identity_key(s.active_sequence_route) ||
        s.pending_sequence_key != sequence_identity_key(s.pending_sequence_route) ||
        (s.committed_transition_valid && !valid_transition_state(s.committed_transition)))
        throw std::runtime_error("Invalid save-state cinematic route identity");
    for (const auto& part : s.tracked_ship_parts) {
        address(part.object, 4);
        if (!std::isfinite(part.x) || !std::isfinite(part.y) || !std::isfinite(part.z))
            throw std::runtime_error("Invalid save-state scanner position");
    }
    if (s.scanner_frame == 0 || (s.scanner_audio_target != 0xff &&
        (s.scanner_audio_target < first_ship_part_flag || s.scanner_audio_target > last_ship_part_flag)))
        throw std::runtime_error("Invalid save-state scanner identity");
}
}
jfc::state::Bytes jfc::state::capture_modernization() {
    jfc::modernization::flush_save_metadata();
    auto storage = std::make_unique<PreparedModernization::Impl>();
    auto& state = *storage;
    {
        std::lock_guard lock(metadata_mutex);
#define CAPTURE_FIELD(name) state.name = ::name;
        JFC_MODERNIZATION_STATE_FIELDS(CAPTURE_FIELD)
#undef CAPTURE_FIELD
        state.weapons = jfc::weapons::state();
    }
    validate_modernization_state(state);
    if (grenade_input_scoped) throw std::runtime_error("Save requested inside native weapon input scope");
    if (corpse_retirement_object) throw std::runtime_error("Save requested inside native corpse retirement");
    Writer out; out.u32(7); transfer_modernization_state(out, state, 7);
    if (out.bytes.size() != modernization_state_sizes.back())
        throw std::runtime_error("Modernization state layout does not match its schema");
    return std::move(out.bytes);
}
jfc::state::PreparedModernization::PreparedModernization(std::span<const uint8_t> saved)
{
    Reader in(saved);
    const auto version = in.u32();
    if (version < 1 || version > 7) throw std::runtime_error("Unsupported modernization state schema");
    if (saved.size() != modernization_state_sizes[version - 1])
        throw std::runtime_error("Invalid modernization state section length");
    impl = std::make_unique<Impl>();
    transfer_modernization_state(in, *impl, version); in.end(); validate_modernization_state(*impl);
}
jfc::state::PreparedModernization::~PreparedModernization() = default;
void jfc::state::PreparedModernization::commit() noexcept {
    if (impl->committed) std::terminate();
    std::lock_guard lock(metadata_mutex);
#define RESTORE_FIELD(name) ::name = impl->name;
    JFC_MODERNIZATION_STATE_FIELDS(RESTORE_FIELD)
#undef RESTORE_FIELD
    jfc::weapons::restore_state(impl->weapons);
    tribal_reconciliation = {};
    retired_corpses.fill(0);
    ++metadata_revision;

    health_reset_requested.store(false, std::memory_order_relaxed);

    impl->committed = true;
}
#undef JFC_MODERNIZATION_STATE_FIELDS
