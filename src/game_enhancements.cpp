#include "camera_ownership.hpp"
#include "cpu_framebuffer_effects.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

#include "display_options.hpp"
#include "allocation_generations.hpp"
#include "../include/rt64_extended_gbi.h"
#include "gbi/rt64_display_list.h"
#include "gbi/rt64_gbi_f3ddkr.h"
#include "jfc_enhancements.hpp"
#include "jfc_modernization.hpp"
#include "ultramodern/audio_activity.hpp"
#include "ultramodern/threads.hpp"
#include "native_context.hpp"
#include "native_rom_data.hpp"
#include "jfc_recomp.h"
#include "modern_input.hpp"
#include "save_state_game.hpp"
#include "save_state_codec.hpp"

extern "C" void mainGetGameCharacter(uint8_t* rdram, recomp_context* ctx);

namespace {

std::mutex camera_samples_mutex;
std::array<std::vector<ultramodern::renderer::CameraSample>, 2> camera_samples;
std::array<bool, 2> camera_samples_complete{true, true};
std::mutex allocation_generations_mutex;
jfc::AllocationGenerations allocation_generations;
std::array<std::vector<ultramodern::renderer::MatrixGeneration>, 2> matrix_generations;
std::array<bool, 2> matrix_generations_complete{true, true};

std::vector<ultramodern::renderer::GraphicsTaskMetadata::TextureScrollRange> texture_scrolls;
uint64_t texture_scroll_serial = 0;

void forget_texture_scrolls(uint32_t address, uint32_t bytes) {
    const auto first = std::lower_bound(texture_scrolls.begin(), texture_scrolls.end(), address,
        [](const auto &entry, uint32_t begin) { return entry.end <= begin; });
    auto last = first;
    while (last != texture_scrolls.end() && last->begin < address + bytes) ++last;
    texture_scrolls.erase(first, last);
}

void capture_matrix_generation(uint8_t *rdram, uint32_t matrix, uint32_t owner) {
    const int32_t bank = MEM_W(0, 0x800FD7C0U);
    matrix &= 0x00FFFFF8U;
    if (bank < 0 || bank > 1 || matrix > 0x800000U - 64)
        throw std::runtime_error("Invalid matrix generation producer");
    const std::scoped_lock lock(allocation_generations_mutex, camera_samples_mutex);
    auto &entries = matrix_generations[size_t(bank)];
    const auto it = std::lower_bound(entries.begin(), entries.end(), matrix,
        [](const auto &entry, uint32_t address) { return entry.address < address; });
    const ultramodern::renderer::MatrixGeneration sample{matrix, allocation_generations.at(owner & 0x1FFFFFFFU)};
    if (it != entries.end() && it->address == matrix) *it = sample;
    else entries.insert(it, sample);
}

thread_local uint32_t camera_producer = 0;
thread_local ultramodern::renderer::CameraSample camera_float_sample;
uint64_t camera_generation = 1;

struct AtomicAim {
    std::atomic<float> x{0.0f};
    std::atomic<float> y{0.0f};
    std::atomic<float> stick_x{0.0f};
    std::atomic<float> stick_y{0.0f};
    std::atomic<float> mouse_x{0.0f};
    std::atomic<float> mouse_y{0.0f};
    std::atomic<float> pending_mouse_x{0.0f};
    std::atomic<float> pending_mouse_y{0.0f};
};

std::array<AtomicAim, 4> aims;
void discard_mouse_counts(size_t index) {
    aims[index].pending_mouse_x.store(0,std::memory_order_relaxed);
    aims[index].pending_mouse_y.store(0,std::memory_order_relaxed);
    aims[index].mouse_x.store(0,std::memory_order_relaxed);
    aims[index].mouse_y.store(0,std::memory_order_relaxed);
}
std::array<std::atomic<uint16_t>, 4> routed_movement_buttons{};
std::array<std::atomic<uint16_t>, 4> sampled_direct_action_buttons{};
std::array<std::atomic<uint16_t>, 4> routed_direct_action_buttons{};
std::array<std::atomic<uint16_t>, 4> menu_direct_action_buttons{};
std::array<std::atomic<uint16_t>, 4> suppressed_direct_action_buttons{};
std::array<std::atomic_bool, 4> physical_aim_buttons{};
std::atomic_bool modern_controls{true};
std::atomic_bool mouse_acceleration{false};
std::atomic_bool cutscene_skipping{true};
std::atomic_int joystick_sensitivity{100};
std::atomic_int joystick_sensitivity_x{100};
std::atomic_int joystick_sensitivity_y{100};
std::atomic<float> output_aspect_ratio{4.0f / 3.0f};
std::atomic_int draw_distance_percent{100};
std::atomic_int fog_strength_percent{100};
std::atomic_uint64_t modern_controls_revision{1};
thread_local uint32_t pending_matrix_tag = 1;
thread_local bool pending_matrix_multiply = false;
thread_local uint32_t sprite_owner = 0;
thread_local bool sequence_skip_pending = false;
thread_local bool object_skip_pending = false;
thread_local bool target_reticle_visible = true;
thread_local bool target_reticle_interpolation = false;
struct HudNumberFilterScope {
    int32_t prior = 0;
    bool active = false;
};
thread_local HudNumberFilterScope hud_number_filter_scope;
thread_local bool weapon_panel_scope_active = false;
thread_local bool weapon_panel_content_scope_active = false;
thread_local uint32_t weapon_panel_display_list_start = 0;
thread_local uint32_t weapon_panel_content_display_list_start = 0;
thread_local int weapon_panel_alpha = -1;
thread_local int weapon_panel_timer = -1;
thread_local uint32_t weapon_panel_player = 0;
thread_local uint32_t weapon_panel_text = 0;
thread_local bool queued_pickup_text_scope_active = false;
thread_local bool queued_pickup_text_aligned = false;
thread_local bool weapon_selection_scissor_scope_active = false;
thread_local bool physical_transition_scope_active = false;
thread_local uint32_t physical_transition_kind = 0;
thread_local uint32_t physical_transition_native_start = 0;
constexpr uint32_t clear_screen_red_address = 0x800A35B0U;
constexpr uint32_t clear_screen_green_address = 0x800A35B4U;
constexpr uint32_t clear_screen_blue_address = 0x800A35B8U;
constexpr uint32_t cutscene_skip_update = 0x7FFF;
constexpr uint32_t all_weapons_cheat_address = 0x800A5078U;
constexpr uint32_t control_binding_table_address = 0x800A18B0U;
constexpr uint8_t machine_gun_weapon = 1U;

struct NativeGfxCommand {
    uint32_t w0;
    uint32_t w1;
};

struct NativeInputSnapshot {
    bool active = false;
    uint32_t keys = 0;
    uint32_t pressed = 0;
    uint32_t released = 0;
    int32_t x = 0;
    int32_t raw_x = 0;
    int32_t y = 0;
    int32_t raw_y = 0;
};

struct ModernPlayerScope {
    uint32_t owner = 0;
    uint32_t object = 0;
    bool active = false;
    bool awaiting_input = false;
    bool native_input_lockout = false;
    uint8_t state_before = 0;
    int16_t facing_before = 0;
    int16_t yaw_before = 0;
    int16_t pitch_before = 0;
    std::array<uint32_t, 3> position_before{};
    bool isolated_l2_applied = false;
    bool borrowed_l2_tail_active = false;
    bool action_momentum_pending = false;
    uint32_t action_momentum_before_forward_speed = 0;
    uint32_t action_momentum_before_lateral_speed = 0;
    uint32_t action_momentum_forward_speed = 0;
    uint32_t action_momentum_lateral_speed = 0;
    uint32_t action_momentum_timestep = 0;
    uint16_t action_momentum_buttons = 0;
    int16_t action_momentum_heading = 0;
    bool direct_pitch_pending = false;
    int16_t direct_pitch = 0;
    bool direct_yaw_pending = false;
    int16_t direct_yaw = 0;
    bool center_reticle_pending = false;
    uint8_t action_latch_before = 0;
    uint8_t action_state_before = 0;
    NativeInputSnapshot input{};
    jfc::modern_input::GameplayAxes axes{};
};

thread_local std::array<ModernPlayerScope, 4> modern_player_scopes;
struct ModernActionTravel {
    uint32_t owner = 0;
    uint32_t object = 0;
    int16_t movement_heading = 0;
    int16_t facing = 0;
    int16_t yaw = 0;
    bool active = false;
};
thread_local std::array<ModernActionTravel, 4> modern_action_travel;
thread_local int active_modern_input_controller = -1;
thread_local uint64_t observed_modern_controls_revision = 0;
thread_local bool skip_borrowed_state_zero = false;
struct ManualAimScope {
    uint32_t player = 0;
    uint32_t horizontal_target_pointer = 0;
    uint32_t vertical_target_pointer = 0;
    int16_t yaw_before = 0;
    int16_t pitch_before = 0;
    uint32_t horizontal_filter_before = 0;
    uint32_t vertical_filter_before = 0;
    float horizontal_stick = 0.0f;
    float vertical_stick = 0.0f;
    float horizontal_mouse = 0.0f;
    float vertical_mouse = 0.0f;
    float elapsed_vi = 1.0f;
    bool physical_aim = false;
    bool active = false;
};
thread_local ManualAimScope manual_aim_scope;

bool valid_rdram_address(uint32_t address, uint32_t size);

struct ModernActionDirection {
    float forward = 0.0f;
    float lateral = 0.0f;
    bool valid = false;
};

ModernActionDirection modern_action_direction(uint16_t buttons) {
    float forward = 0.0f;
    float lateral = 0.0f;
    if ((buttons & jfc::modern_input::c_up) != 0U) {
        forward -= 1.0f;
    }
    if ((buttons & jfc::modern_input::c_down) != 0U) {
        forward += 1.0f;
    }
    if ((buttons & jfc::modern_input::c_left) != 0U) {
        lateral -= 1.0f;
    }
    if ((buttons & jfc::modern_input::c_right) != 0U) {
        lateral += 1.0f;
    }
    const float magnitude = std::hypot(forward, lateral);
    if (!std::isfinite(magnitude) || magnitude <= 0.0f) {
        return {};
    }
    return {forward / magnitude, lateral / magnitude, true};
}

bool canonicalize_modern_action_velocity(
    uint32_t forward_bits,
    uint32_t lateral_bits,
    uint16_t buttons,
    uint32_t& canonical_forward_bits
) {
    const ModernActionDirection direction = modern_action_direction(buttons);
    const float forward = std::bit_cast<float>(forward_bits);
    const float lateral = std::bit_cast<float>(lateral_bits);
    if (!direction.valid || !std::isfinite(forward) ||
        !std::isfinite(lateral)) {
        return false;
    }

    const float aligned_speed =
        forward * direction.forward + lateral * direction.lateral;
    if (!std::isfinite(aligned_speed)) {
        return false;
    }

    canonical_forward_bits = std::bit_cast<uint32_t>(-aligned_speed);
    return true;
}

constexpr uint32_t display_list_cursor_address = 0x800FF398U;
constexpr uint32_t sprite_texture_filter_address = 0x800A4E64U;
constexpr uint32_t player_manager_address = 0x800A51C8U;
constexpr uint32_t current_viewport_index_address = 0x800FA634U;
constexpr uint32_t ortho_viewport_physical_base = 0x000A3008U;
constexpr uint32_t ortho_viewport_stride = 0x10U;
constexpr uint32_t ortho_viewport_count = 4U;
constexpr uint32_t control_keys_address = 0x800F6DA0U;
constexpr uint32_t control_pressed_address = 0x800F6DA4U;
constexpr uint32_t control_released_address = 0x800F6DA8U;
constexpr uint32_t control_x_address = 0x800F6DACU;
constexpr uint32_t control_y_address = 0x800F6DB0U;

constexpr uint32_t control_raw_x_address = 0x800F6DB4U;
constexpr uint32_t control_raw_y_address = 0x800F6DB8U;

constexpr uint32_t camera_fov_address = 0x800FB078U;
constexpr uint32_t control_mode_keys_address = 0x800A18B0U;
constexpr uint32_t control_mode_expert = 0x800A18D8U;
constexpr uint32_t expert_jump_binding_address = control_mode_expert + 0x0CU;
constexpr uint32_t expert_crouch_binding_address = control_mode_expert + 0x10U;
constexpr uint32_t gameplay_mode_address = 0x800FD7C4U;
constexpr uint32_t animation_camera_address = 0x801045B8U;
constexpr uint32_t r_button = 0x10U;

constexpr uint32_t modern_jump_button = 0x00010000U;
constexpr uint32_t modern_crouch_button = 0x00020000U;

bool valid_rdram_address(uint32_t address, uint32_t size) {
    constexpr uint32_t rdram_size = 8U * 1024U * 1024U;
    const uint32_t segment = address & 0xE0000000U;
    const uint32_t physical = address & 0x1FFFFFFFU;
    return (segment == 0x80000000U || segment == 0xA0000000U) &&
        size <= rdram_size && physical <= rdram_size - size;
}

bool modern_attached_camera_eligible(uint8_t* rdram, uint32_t player) {
    return jfc::camera::attached_eligible(rdram, player);
}

bool boss_control_owns_aim(uint8_t* rdram, uint32_t player) {

    return valid_rdram_address(player, 0x100U) && MEM_W(0xFC, player) != 0;
}

float read_rdram_float(uint8_t* rdram, uint32_t address) {
    fpr value{};
    value.u32l = static_cast<uint32_t>(MEM_W(0, address));
    return value.fl;
}

template <size_t N>
void append_gfx_commands(
    uint8_t* rdram,
    uint32_t cursor_pointer,
    const std::array<NativeGfxCommand, N>& commands
) {
    constexpr uint32_t byte_count = static_cast<uint32_t>(N * sizeof(NativeGfxCommand));
    if (!valid_rdram_address(cursor_pointer, sizeof(uint32_t))) {
        return;
    }
    const uint32_t cursor = static_cast<uint32_t>(MEM_W(0, cursor_pointer));
    if (!valid_rdram_address(cursor, byte_count)) {
        return;
    }

    for (size_t i = 0; i < commands.size(); ++i) {
        const uint32_t address = cursor + static_cast<uint32_t>(i * sizeof(NativeGfxCommand));
        MEM_W(0, address) = static_cast<int32_t>(commands[i].w0);
        MEM_W(4, address) = static_cast<int32_t>(commands[i].w1);
    }
    MEM_W(0, cursor_pointer) = static_cast<int32_t>(cursor + byte_count);
}

template <size_t N>
void append_gfx_commands(
    uint8_t* rdram,
    const std::array<NativeGfxCommand, N>& commands
) {
    append_gfx_commands(rdram, display_list_cursor_address, commands);
}

void write_control_axes(
    uint8_t* rdram,
    const jfc::modern_input::GameplayAxes& axes
) {
    MEM_W(0, control_x_address) = axes.control_x;
    MEM_W(0, control_raw_x_address) = axes.control_raw_x;
    MEM_W(0, control_y_address) = axes.control_y;
    MEM_W(0, control_raw_y_address) = axes.control_raw_y;
}

void write_movement_geometry_axes(
    uint8_t* rdram,
    const jfc::modern_input::GameplayAxes& axes
) {
    MEM_W(0, control_x_address) = axes.movement_x;
    MEM_W(0, control_raw_x_address) = axes.movement_raw_x;
    MEM_W(0, control_y_address) = axes.movement_y;
    MEM_W(0, control_raw_y_address) = axes.movement_raw_y;
}

bool modern_action_state(uint8_t state) {

    return state >= 0x01U && state <= 0x03U;
}

int16_t modern_action_composition_heading(
    uint8_t* rdram,
    uint32_t player,
    const ModernActionTravel& travel,
    uint8_t state
) {

    if ((state != 1U && state != 2U) ||
        !valid_rdram_address(player + 0x04U, sizeof(uint32_t))) {
        return travel.movement_heading;
    }
    const float native_forward = read_rdram_float(rdram, player + 0x04U);
    if (!std::isfinite(native_forward) || native_forward <= 0.0f) {
        return travel.movement_heading;
    }
    return static_cast<int16_t>(
        static_cast<uint16_t>(travel.movement_heading) + 0x8000U);
}

void preserve_modern_action_facing(
    uint8_t* rdram,
    const ModernPlayerScope& scope,
    uint8_t action_state
) {
    if (!modern_action_state(action_state) ||
        !valid_rdram_address(scope.object, sizeof(int16_t)) ||
        !valid_rdram_address(scope.owner + 0x11CU, sizeof(int16_t))) {
        return;
    }

    const bool directional_action =
        scope.axes.native_movement_buttons != 0U ||
        scope.axes.isolated_l2_movement_buttons != 0U;
    const bool grounded_direct_look = scope.direct_yaw_pending &&
        (action_state <= 2U || (action_state == 3U && MEM_BU(0x14A, scope.owner) != 0));
    if (!directional_action && !grounded_direct_look) {
        return;
    }

    MEM_H(0, scope.object) = grounded_direct_look
        ? scope.direct_yaw
        : scope.facing_before;
    MEM_H(0x11C, scope.owner) = scope.direct_yaw_pending
        ? scope.direct_yaw
        : scope.yaw_before;
}

int32_t native_aim_axis(float axis) {
    return jfc::modern_input::manual_aim_axis(axis);
}

struct DirectLookAngles {
    int16_t yaw = 0;
    int16_t pitch = 0;
    int32_t yaw_delta = 0;
    int32_t pitch_delta = 0;
};

DirectLookAngles modern_direct_look_angles(
    float horizontal_stick,
    float vertical_stick,
    float horizontal_mouse,
    float vertical_mouse,
    int16_t yaw_before,
    int16_t pitch_before,
    float elapsed_vi,
    double scope_scale = 1.0
) {
    const double overall = static_cast<double>(
        joystick_sensitivity.load(std::memory_order_relaxed)) / 100.0 * scope_scale;
    const double horizontal = overall * static_cast<double>(
        joystick_sensitivity_x.load(std::memory_order_relaxed)) / 100.0;
    const double vertical = overall * static_cast<double>(
        joystick_sensitivity_y.load(std::memory_order_relaxed)) / 100.0;
    const int32_t yaw_delta = jfc::modern_input::direct_look_angle_delta(
        horizontal_stick,
        horizontal_mouse,
        elapsed_vi,
        horizontal,
        false,scope_scale);
    const int32_t pitch_delta = jfc::modern_input::direct_look_angle_delta(
        vertical_stick,
        vertical_mouse,
        elapsed_vi,
        vertical,
        true,scope_scale);
    return {
        static_cast<int16_t>(static_cast<uint16_t>(yaw_before) +
            static_cast<uint16_t>(yaw_delta)),
        static_cast<int16_t>(std::clamp(
            static_cast<int32_t>(pitch_before) + pitch_delta,
            -jfc::modern_input::direct_pitch_limit,
            jfc::modern_input::direct_pitch_limit)),
        yaw_delta,
        pitch_delta,
    };
}

void stage_modern_direct_look(
    uint8_t* rdram,
    ModernPlayerScope& scope,
    size_t controller,
    float elapsed_vi
) {
    if (scope.direct_yaw_pending ||
        !valid_rdram_address(scope.owner + 0x1E2U, sizeof(int16_t))) {
        return;
    }
    const DirectLookAngles angles = modern_direct_look_angles(
        aims[controller].stick_x.load(std::memory_order_relaxed),
        aims[controller].stick_y.load(std::memory_order_relaxed),
        aims[controller].mouse_x.load(std::memory_order_relaxed),
        aims[controller].mouse_y.load(std::memory_order_relaxed),
        scope.yaw_before,
        scope.pitch_before,
        elapsed_vi);
    scope.direct_yaw = angles.yaw;
    scope.direct_pitch = angles.pitch;
    scope.direct_yaw_pending = true;
    scope.direct_pitch_pending = true;
    scope.center_reticle_pending = true;

    MEM_H(0x11C, scope.owner) = angles.yaw;
    MEM_H(0x1E2, scope.owner) = angles.pitch;
}

NativeInputSnapshot capture_native_input(uint8_t* rdram) {
    return {
        true,
        static_cast<uint32_t>(MEM_W(0, control_keys_address)),
        static_cast<uint32_t>(MEM_W(0, control_pressed_address)),
        static_cast<uint32_t>(MEM_W(0, control_released_address)),
        MEM_W(0, control_x_address),
        MEM_W(0, control_raw_x_address),
        MEM_W(0, control_y_address),
        MEM_W(0, control_raw_y_address),
    };
}

void restore_native_input(
    uint8_t* rdram,
    NativeInputSnapshot& snapshot
) {
    if (!snapshot.active) {
        return;
    }
    MEM_W(0, control_keys_address) =
        static_cast<int32_t>(snapshot.keys);
    MEM_W(0, control_pressed_address) =
        static_cast<int32_t>(snapshot.pressed);
    MEM_W(0, control_released_address) =
        static_cast<int32_t>(snapshot.released);
    MEM_W(0, control_x_address) = snapshot.x;
    MEM_W(0, control_raw_x_address) = snapshot.raw_x;
    MEM_W(0, control_y_address) = snapshot.y;
    MEM_W(0, control_raw_y_address) = snapshot.raw_y;
    snapshot = {};
}

void restore_modern_player_input(uint8_t* rdram) {
    if (active_modern_input_controller < 0 ||
        static_cast<size_t>(active_modern_input_controller) >=
            modern_player_scopes.size()) {
        active_modern_input_controller = -1;
        return;
    }
    restore_native_input(
        rdram,
        modern_player_scopes[static_cast<size_t>(
            active_modern_input_controller)].input);
    active_modern_input_controller = -1;
}

void set_expert_action_bindings(uint8_t* rdram, bool modern) {
    if (!valid_rdram_address(expert_jump_binding_address, sizeof(uint32_t)) ||
        !valid_rdram_address(expert_crouch_binding_address, sizeof(uint32_t))) {
        return;
    }
    MEM_W(0, expert_jump_binding_address) = static_cast<int32_t>(
        modern ? modern_jump_button : jfc::modern_input::c_up);
    MEM_W(0, expert_crouch_binding_address) = static_cast<int32_t>(
        modern ? modern_crouch_button : jfc::modern_input::c_down);
}

ModernPlayerScope* active_scope_for_player(uint8_t* rdram, uint32_t player) {
    if (!valid_rdram_address(player, 0x5C4U)) {
        return nullptr;
    }
    const int controller = MEM_B(0, player);
    if (controller < 0 ||
        static_cast<size_t>(controller) >= modern_player_scopes.size()) {
        return nullptr;
    }
    ModernPlayerScope& scope = modern_player_scopes[controller];
    if (!scope.active || scope.owner != player ||
        !modern_attached_camera_eligible(rdram, player)) {
        return nullptr;
    }
    return &scope;
}

void discard_modern_player_input() {
    if (active_modern_input_controller >= 0 &&
        static_cast<size_t>(active_modern_input_controller) <
            modern_player_scopes.size()) {
        modern_player_scopes[static_cast<size_t>(
            active_modern_input_controller)].input = {};
    }
    active_modern_input_controller = -1;
}

void install_modern_player_input(uint8_t* rdram, size_t index) {
    ModernPlayerScope& scope = modern_player_scopes[index];
    if (!scope.active || !valid_rdram_address(scope.owner, 0x5C4U) ||
        !valid_rdram_address(scope.object, 0x6CU)) {
        return;
    }

    scope.input = capture_native_input(rdram);
    active_modern_input_controller = static_cast<int>(index);

    const float mouse_x = aims[index].pending_mouse_x.exchange(0, std::memory_order_relaxed);
    const float mouse_y = aims[index].pending_mouse_y.exchange(0, std::memory_order_relaxed);
    aims[index].mouse_x.store(mouse_x, std::memory_order_relaxed);
    aims[index].mouse_y.store(mouse_y, std::memory_order_relaxed);
    aims[index].x.store(std::clamp(aims[index].stick_x.load(std::memory_order_relaxed)*80 + mouse_x,-80.0f,80.0f),std::memory_order_relaxed);
    aims[index].y.store(std::clamp(aims[index].stick_y.load(std::memory_order_relaxed)*80 + mouse_y,-80.0f,80.0f),std::memory_order_relaxed);

    uint32_t native_held = scope.input.keys;
    uint32_t native_pressed = scope.input.pressed;
    uint32_t native_released = scope.input.released;

    const int32_t right_x = native_aim_axis(
        aims[index].x.load(std::memory_order_relaxed));
    const int32_t right_y = native_aim_axis(
        aims[index].y.load(std::memory_order_relaxed));
    const bool physical_fine_aim = (scope.input.keys & r_button) != 0;
    scope.axes = jfc::modern_input::gameplay_axes(
        scope.input.x,
        scope.input.raw_x,
        scope.input.y,
        scope.input.raw_y,
        right_x,
        right_y,
        physical_fine_aim);

    constexpr uint32_t movement_button_mask =
        jfc::modern_input::c_left | jfc::modern_input::c_right |
        jfc::modern_input::c_up | jfc::modern_input::c_down;

    const uint16_t movement = physical_fine_aim
        ? scope.axes.native_movement_buttons
        : (modern_action_state(scope.state_before)
            ? 0U
            : (scope.state_before == 0U
                ? scope.axes.native_movement_buttons
                : scope.axes.isolated_l2_movement_buttons));
    const uint16_t previous_movement =
        routed_movement_buttons[index].exchange(
            movement, std::memory_order_relaxed);
    native_held = (native_held & ~movement_button_mask) | movement;
    native_pressed = (native_pressed & ~movement_button_mask) |
        (movement & ~previous_movement);
    native_released = (native_released & ~movement_button_mask) |
        (previous_movement & ~movement);

    const uint16_t sampled_direct_actions =
        sampled_direct_action_buttons[index].load(std::memory_order_relaxed);

    const uint16_t suppressed_direct_actions =
        suppressed_direct_action_buttons[index].load(
            std::memory_order_relaxed) & sampled_direct_actions;
    suppressed_direct_action_buttons[index].store(
        suppressed_direct_actions, std::memory_order_relaxed);
    const uint16_t direct_actions =
        sampled_direct_actions & ~suppressed_direct_actions;
    const uint16_t previous_direct_actions =
        routed_direct_action_buttons[index].exchange(
            direct_actions, std::memory_order_relaxed);

    const auto route_direct_action = [&](uint16_t direct_bit,
                                         uint32_t synthetic_bit,
                                         uint32_t original_bit) {
        native_held &= ~synthetic_bit;
        native_pressed &= ~synthetic_bit;
        native_released &= ~synthetic_bit;
        if ((direct_actions & direct_bit) != 0) {
            native_held &= ~original_bit;
            native_pressed &= ~original_bit;
            native_released &= ~original_bit;
        }
        else if ((previous_direct_actions & direct_bit) != 0) {
            native_released &= ~original_bit;
            if ((native_held & original_bit) != 0) {
                native_pressed |= original_bit;
            }
        }
        if ((direct_actions & direct_bit) != 0) {
            native_held |= synthetic_bit;
            native_released &= ~synthetic_bit;
        }
        if ((direct_actions & ~previous_direct_actions & direct_bit) != 0) {
            native_pressed |= synthetic_bit;
        }
        if ((previous_direct_actions & ~direct_actions & direct_bit) != 0) {
            native_released |= synthetic_bit;
        }
    };
    route_direct_action(
        jfc::modern_input::c_up, modern_jump_button, jfc::modern_input::a);
    route_direct_action(
        jfc::modern_input::c_down, modern_crouch_button, jfc::modern_input::b);

    native_released &= ~native_held;
    MEM_W(0, control_keys_address) = static_cast<int32_t>(native_held);
    MEM_W(0, control_pressed_address) = static_cast<int32_t>(native_pressed);
    MEM_W(0, control_released_address) = static_cast<int32_t>(native_released);

    write_movement_geometry_axes(rdram, scope.axes);
}

constexpr uint32_t packed_draw_scale(
    uint32_t scale,
    uint32_t horizontal_origin,
    uint32_t vertical_origin
) {
    return (scale & 0x1FFU) |
        ((horizontal_origin & 0xFFFU) << 9) |
        ((vertical_origin & 0x3U) << 21);
}

void begin_single_player_hud(
    uint8_t* rdram,
    uint32_t vertical_origin,
    uint32_t draw_scale = G_EX_SCALE_ONE,
    uint32_t rectangle_aspect = G_EX_ASPECT_ADJUST,
    bool centered = false
) {
    const uint32_t viewport_index = static_cast<uint32_t>(
        MEM_W(0, current_viewport_index_address));
    if (viewport_index >= ortho_viewport_count) {
        return;
    }
    const uint32_t viewport_address =
        ortho_viewport_physical_base + viewport_index * ortho_viewport_stride;

    const uint32_t physical_left_inset = centered ? 0U : 1U * 4U;
    constexpr uint32_t physical_scissor_origin_pair =
        G_EX_ORIGIN_LEFT | (G_EX_ORIGIN_RIGHT << 12);
    const std::array commands{
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{0x64000000U | G_EX_PUSHSCISSOR_V1, 0U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETSCISSOR_V1,
            G_EX_ORIGIN_RIGHT << 14},

        NativeGfxCommand{0U, 240U * 4U},

        NativeGfxCommand{
            0x64000000U | G_EX_SETSCISSORALIGN_V1,
            physical_scissor_origin_pair},
        NativeGfxCommand{0U, 0U},
        NativeGfxCommand{0U, 0x7FFF7FFFU},
        NativeGfxCommand{
            0x64000000U | G_EX_SETRECTASPECT_V1,
            rectangle_aspect},
        NativeGfxCommand{
            0x64000000U | G_EX_SETDRAWSCALE_V1,
            packed_draw_scale(
                draw_scale,
                centered ? G_EX_ORIGIN_CENTER : G_EX_ORIGIN_LEFT,
                vertical_origin)},
        NativeGfxCommand{0x64000000U | G_EX_SETRECTALIGN_V1,
            centered ? G_EX_ORIGIN_NONE | (G_EX_ORIGIN_NONE << 12) : 0U},
        NativeGfxCommand{
            physical_left_inset << 16,
            physical_left_inset << 16},
        NativeGfxCommand{
            0x64000000U | G_EX_SETVIEWPORTALIGN_V1,
            uint32_t(centered ? G_EX_ORIGIN_NONE : G_EX_ORIGIN_LEFT)},
        NativeGfxCommand{physical_left_inset << 16, 0U},

        NativeGfxCommand{0x03800010U, viewport_address},
    };
    append_gfx_commands(rdram, commands);
}

bool begin_weapon_panel_projected_scope(uint8_t* rdram) {
    constexpr uint32_t physical_left_inset = 1U * 4U;
    if (!valid_rdram_address(
            current_viewport_index_address, sizeof(uint32_t))) {
        return false;
    }
    const uint32_t viewport_index = static_cast<uint32_t>(
        MEM_W(0, current_viewport_index_address));
    if (viewport_index >= ortho_viewport_count) {
        return false;
    }
    const uint32_t viewport_address =
        ortho_viewport_physical_base + viewport_index * ortho_viewport_stride;
    const std::array commands{
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{0x64000000U | G_EX_PUSHSCISSOR_V1, 0U},
        NativeGfxCommand{0x64000000U | G_EX_PUSHVIEWPORT_V1, 0U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETSCISSOR_V1,
            G_EX_ORIGIN_RIGHT << 14},
        NativeGfxCommand{0U, 240U * 4U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETRECTASPECT_V1,
            G_EX_ASPECT_ADJUST},
        NativeGfxCommand{
            0x64000000U | G_EX_SETDRAWSCALE_V1,
            packed_draw_scale(
                G_EX_SCALE_ONE,
                G_EX_ORIGIN_LEFT,
                G_EX_SCALE_ORIGIN_TOP)},
        NativeGfxCommand{0x64000000U | G_EX_SETRECTALIGN_V1, 0U},
        NativeGfxCommand{
            physical_left_inset << 16,
            physical_left_inset << 16},
        NativeGfxCommand{
            0x64000000U | G_EX_SETVIEWPORTALIGN_V1,
            G_EX_ORIGIN_LEFT},
        NativeGfxCommand{physical_left_inset << 16, 0U},

        NativeGfxCommand{0x03800010U, viewport_address},
    };
    append_gfx_commands(rdram, commands);
    return true;
}

bool begin_weapon_selection_scissor_scope(uint8_t* rdram) {
    if (weapon_selection_scissor_scope_active ||
        !valid_rdram_address(display_list_cursor_address, sizeof(uint32_t))) {
        return false;
    }
    const uint32_t cursor = static_cast<uint32_t>(
        MEM_W(0, display_list_cursor_address));
    constexpr uint32_t left_origin_pair =
        G_EX_ORIGIN_LEFT | (G_EX_ORIGIN_LEFT << 12);
    constexpr std::array commands{
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{0x64000000U | G_EX_PUSHSCISSOR_V1, 0U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETSCISSORALIGN_V1,
            left_origin_pair},
        NativeGfxCommand{0U, 0U},
        NativeGfxCommand{0U, 0x7FFF7FFFU},
    };
    append_gfx_commands(rdram, commands);
    weapon_selection_scissor_scope_active =
        static_cast<uint32_t>(MEM_W(0, display_list_cursor_address)) ==
        cursor + commands.size() * sizeof(NativeGfxCommand);
    return weapon_selection_scissor_scope_active;
}

void end_weapon_selection_scissor_scope(uint8_t* rdram) {
    if (!weapon_selection_scissor_scope_active) {
        return;
    }
    constexpr uint32_t no_origin_pair =
        G_EX_ORIGIN_NONE | (G_EX_ORIGIN_NONE << 12);
    constexpr std::array commands{
        NativeGfxCommand{
            0x64000000U | G_EX_SETSCISSORALIGN_V1,
            no_origin_pair},
        NativeGfxCommand{0U, 0U},
        NativeGfxCommand{0U, 0x7FFF7FFFU},
        NativeGfxCommand{0x64000000U | G_EX_POPSCISSOR_V1, 0U},
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28},
    };
    append_gfx_commands(rdram, commands);
    weapon_selection_scissor_scope_active = false;
}

void end_weapon_panel_projected_scope(uint8_t* rdram) {
    constexpr std::array commands{
        NativeGfxCommand{0x64000000U | G_EX_POPVIEWPORT_V1, 0U},
        NativeGfxCommand{0x64000000U | G_EX_POPSCISSOR_V1, 0U},
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28},
    };
    append_gfx_commands(rdram, commands);
}

}

namespace jfc::enhancements {

void record_effect_matrix(uint8_t* rdram, uint32_t matrix, uint32_t object) {
    if (!valid_rdram_address(matrix,64) || (matrix&7) || !valid_rdram_address(object,0x6C))
        throw std::runtime_error("Invalid effect matrix owner");
    capture_matrix_generation(rdram,matrix,object);
}

const char* try_graphics_task_is_frame(uint8_t* rdram, uint32_t task, bool& is_frame) noexcept {

    if (rdram == nullptr || task < 0x10U || (task & 7U) != 0U ||
        !valid_rdram_address(task - 0x10U, 0x70U)) {
        return "invalid OSScTask container";
    }
    const uint32_t flags = MEM_W(-8, task);
    if ((flags & 3U) != 3U) {
        return "graphics task does not require both native SP and DP";
    }
    is_frame = (flags & 0x20U) != 0U;
    return nullptr;
}

bool graphics_task_is_frame(uint8_t* rdram, uint32_t task) {
    bool is_frame = false;
    if (const char* error = try_graphics_task_is_frame(rdram, task, is_frame)) {
        std::fprintf(stderr, "JFG graphics task: %s (task=%08X)\n", error, task);
        std::exit(EXIT_FAILURE);
    }
    return is_frame;
}

const char* try_graphics_task_framebuffer(uint8_t* rdram, uint32_t task, uint32_t& address) noexcept {
    bool is_frame = false;
    if (const char* error = try_graphics_task_is_frame(rdram, task, is_frame)) return error;
    address = UINT32_MAX;
    if (!is_frame) return nullptr;

    const uint32_t screen = uint32_t(MEM_W(-4, task));
    const uint32_t segment = screen & 0xE0000000U;
    if ((screen & 7U) != 0 || (screen & 0x1FFFFFFFU) >= 0x800000U ||
        (segment != 0x80000000U && segment != 0xA0000000U))
        return "invalid native graphics screen owner";
    address = screen & 0x1FFFFFFFU;
    return nullptr;
}

uint32_t graphics_task_framebuffer(uint8_t* rdram, uint32_t task) {
    uint32_t address = UINT32_MAX;
    if (const char* error = try_graphics_task_framebuffer(rdram, task, address)) {
        std::fprintf(stderr, "JFG graphics image: %s (task=%08X)\n", error, task);
        std::exit(EXIT_FAILURE);
    }
    return address;
}

void set_modern_controls(bool enabled) {
    clear_mouse_aim();
    modern_controls.store(enabled, std::memory_order_relaxed);
    modern_controls_revision.fetch_add(1, std::memory_order_relaxed);
    for (auto& buttons : routed_movement_buttons) {
        buttons.store(0, std::memory_order_relaxed);
    }
    for (auto& buttons : sampled_direct_action_buttons) {
        buttons.store(0, std::memory_order_relaxed);
    }
    for (auto& buttons : routed_direct_action_buttons) {
        buttons.store(0, std::memory_order_relaxed);
    }
    for (auto& buttons : menu_direct_action_buttons) {
        buttons.store(0, std::memory_order_relaxed);
    }
    for (auto& buttons : suppressed_direct_action_buttons) {
        buttons.store(0, std::memory_order_relaxed);
    }
    for (auto& held : physical_aim_buttons) {
        held.store(false, std::memory_order_relaxed);
    }
    modern_action_travel = {};
}

bool modern_controls_enabled() {
    return modern_controls.load(std::memory_order_relaxed);
}

void set_mouse_acceleration(bool enabled) {
    mouse_acceleration.store(enabled, std::memory_order_relaxed);
}

bool physical_aim_active(int controller) {
    return controller >= 0 &&
        static_cast<size_t>(controller) < physical_aim_buttons.size() &&
        physical_aim_buttons[static_cast<size_t>(controller)].load(
            std::memory_order_relaxed);
}

bool grounded_traversal_active(uint8_t* rdram, uint32_t player) {
    if (!valid_rdram_address(player, 0x5C4U)) {
        return false;
    }
    const ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (scope != nullptr && scope->native_input_lockout) {
        return false;
    }
    const uint8_t state = MEM_BU(0x568, player);
    if (state == 0U) {
        return MEM_BU(0x13E, player) == 0U;
    }

    return state == 0x0BU && scope != nullptr &&
        scope->borrowed_l2_tail_active &&
        scope->state_before == 0U && scope->action_latch_before == 0U;
}

void sample_direct_actions(int controller, bool jump, bool crouch) {
    if (controller < 0 ||
        static_cast<size_t>(controller) >= sampled_direct_action_buttons.size()) {
        return;
    }
    uint16_t buttons = 0;
    if (jump) {
        buttons |= jfc::modern_input::c_up;
    }
    if (crouch) {
        buttons |= jfc::modern_input::c_down;
    }
    sampled_direct_action_buttons[static_cast<size_t>(controller)].store(
        buttons, std::memory_order_relaxed);
}

void set_joystick_sensitivity(int overall, int horizontal, int vertical) {
    joystick_sensitivity.store(std::clamp(overall, 25, 100),
                               std::memory_order_relaxed);
    joystick_sensitivity_x.store(std::clamp(horizontal, 50, 150),
                                 std::memory_order_relaxed);
    joystick_sensitivity_y.store(std::clamp(vertical, 50, 150),
                                 std::memory_order_relaxed);
}

void set_cutscene_skipping(bool enabled) {
    cutscene_skipping.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        sequence_skip_pending = false;
        object_skip_pending = false;
    }
}

void set_draw_distance_percent(int percent) {
    draw_distance_percent.store(
        std::clamp(percent, 100, 400), std::memory_order_relaxed);
}

void set_fog_strength_percent(int percent) {
    fog_strength_percent.store(
        std::clamp(percent, 0, 100), std::memory_order_relaxed);
}

void sample_aim(
    int controller,
    float stick_x,
    float stick_y,
    float mouse_x,
    float mouse_y
) {
    if (controller < 0 || static_cast<size_t>(controller) >= aims.size()) {
        return;
    }
    const modern_input::Aim aim = modern_input::compose(
        stick_x,
        stick_y,
        mouse_x,
        mouse_y,
        mouse_acceleration.load(std::memory_order_relaxed));
    const float gain = modern_input::mouse_gain(
        mouse_x, mouse_y,
        mouse_acceleration.load(std::memory_order_relaxed));
    aims[controller].x.store(aim.x, std::memory_order_relaxed);
    aims[controller].y.store(aim.y, std::memory_order_relaxed);
    aims[controller].stick_x.store(stick_x, std::memory_order_relaxed);
    aims[controller].stick_y.store(stick_y, std::memory_order_relaxed);
    aims[controller].pending_mouse_x.fetch_add(mouse_x * gain, std::memory_order_relaxed);
    aims[controller].pending_mouse_y.fetch_add(mouse_y * gain, std::memory_order_relaxed);
}

void clear_mouse_aim() {
    for (size_t i=0;i<aims.size();++i) discard_mouse_counts(i);
}

}

extern "C" void jfc_ui_alpha_scope(uint8_t* rdram, uint32_t enter) noexcept(false) {
    jfc_ui_alpha_scope_at(rdram, display_list_cursor_address, enter);
}

extern "C" void jfc_ui_alpha_scope_at(
    uint8_t* rdram, uint32_t cursor_pointer, uint32_t enter
) noexcept(false) {
    constexpr uint32_t bytes = 3 * sizeof(NativeGfxCommand);
    if (!rdram || (cursor_pointer & 3U) ||
        !valid_rdram_address(cursor_pointer, sizeof(uint32_t))) {
        throw std::runtime_error("Invalid native UI display-list cursor address");
    }
    const uint32_t cursor = uint32_t(MEM_W(0, cursor_pointer));
    if (enter > 1 || (cursor & 7U) || !valid_rdram_address(cursor, bytes)) {
        throw std::runtime_error("Invalid native UI display-list cursor or scope");
    }
    append_gfx_commands(rdram, cursor_pointer, std::array{
        NativeGfxCommand{(RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{0x64000000U | G_EX_UIALPHASCOPE_V1, enter},
        NativeGfxCommand{(RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28}});
}

extern "C" void jfc_prepare_matrix_tag(uint32_t id) {
    pending_matrix_tag = (id >> 2) & RT64::GBI_F3DDKR::MatrixTagIdMask;
    if (pending_matrix_tag == 0) {
        pending_matrix_tag = 1;
    }
    pending_matrix_multiply = false;
}

extern "C" void jfc_record_allocation(uint8_t *rdram, uint32_t slot, uint32_t size, uint32_t taken) noexcept(false) {
    if (taken != 1) return;
    if (!valid_rdram_address(slot, 20) || (slot & 3U))
        throw std::runtime_error("Invalid native allocation slot");
    const uint32_t address = uint32_t(MEM_W(0, slot));
    if (!valid_rdram_address(address, size) || size > uint32_t(MEM_W(4, slot)))
        throw std::runtime_error("Invalid native allocation size");
    if (size == 0) return;
    std::lock_guard lock(allocation_generations_mutex);
    allocation_generations.replace(address & 0x1FFFFFFFU, size, true);
    forget_texture_scrolls(address & 0x1FFFFFFFU, size);
}

extern "C" void jfc_record_release(uint8_t *rdram, uint32_t slot) noexcept(false) {
    if (!valid_rdram_address(slot, 20) || (slot & 3U))
        throw std::runtime_error("Invalid native release slot");
    const uint32_t address = uint32_t(MEM_W(0, slot));
    const uint32_t size = uint32_t(MEM_W(4, slot));
    if (!valid_rdram_address(address, size))
        throw std::runtime_error("Invalid native release extent");
    if (size == 0) return;
    std::lock_guard lock(allocation_generations_mutex);
    allocation_generations.replace(address & 0x1FFFFFFFU, size, false);
    forget_texture_scrolls(address & 0x1FFFFFFFU, size);
}

extern "C" uint64_t jfc_effect_generation(uint32_t address) noexcept(false) {
    if (!valid_rdram_address(address, 4) || (address & 3U))
        throw std::runtime_error("Invalid native effect owner");
    std::lock_guard lock(allocation_generations_mutex);
    return allocation_generations.at(address & 0x1FFFFFFFU);
}

extern "C" void jfc_record_effect_birth(uint32_t address, uint32_t bytes) noexcept(false) {
    if (!valid_rdram_address(address, bytes) || (address & 3U) || !bytes)
        throw std::runtime_error("Invalid native effect slot birth");
    std::lock_guard lock(allocation_generations_mutex);
    allocation_generations.replace(address & 0x1FFFFFFFU, bytes, true);
    forget_texture_scrolls(address & 0x1FFFFFFFU, bytes);
}

extern "C" void jfc_record_texture_scroll(uint32_t address, uint32_t count,
    uint32_t wrap_s, uint32_t wrap_t, int32_t delta_s, int32_t delta_t) {
    if (!count) return;
    if (count > 0x800000U / 16 || !valid_rdram_address(address, count * 16) || (address & 3U))
        throw std::runtime_error("Native texture scroll range is outside RDRAM");
    const uint32_t begin = address & 0x1FFFFFFFU, end = begin + count * 16;
    std::lock_guard lock(allocation_generations_mutex);

    const bool modulo = std::has_single_bit(wrap_s) && std::has_single_bit(wrap_t) &&
        wrap_s <= 65536 && wrap_t <= 65536;
    const uint64_t generation = allocation_generations.at(begin);
    if (!modulo || !generation || allocation_generations.at(end - 1) != generation) {
        forget_texture_scrolls(begin, end - begin);
        return;
    }
    auto found = std::lower_bound(texture_scrolls.begin(), texture_scrolls.end(), begin,
        [](const auto &entry, uint32_t key) { return entry.begin < key; });
    if (found != texture_scrolls.end() && found->begin == begin && found->end == end &&
        found->wrap[0] == wrap_s && found->wrap[1] == wrap_t) {
        const int32_t delta[2]{delta_s, delta_t};
        for (unsigned axis = 0; axis < 2; ++axis) {
            if ((delta[axis] > 0 && found->phase[axis] > INT64_MAX - delta[axis]) ||
                (delta[axis] < 0 && found->phase[axis] < INT64_MIN - delta[axis]))
                throw std::runtime_error("Native texture scroll phase overflow");
        }
        for (unsigned axis = 0; axis < 2; ++axis) found->phase[axis] += delta[axis];
        return;
    }
    if (texture_scroll_serial == UINT64_MAX)
        throw std::runtime_error("Native texture scroll generation overflow");
    const ultramodern::renderer::GraphicsTaskMetadata::TextureScrollRange sample{
        begin, end, ++texture_scroll_serial, {wrap_s, wrap_t}, {delta_s, delta_t}};
    forget_texture_scrolls(begin, end - begin);
    found = std::lower_bound(texture_scrolls.begin(), texture_scrolls.end(), begin,
        [](const auto &entry, uint32_t key) { return entry.begin < key; });
    texture_scrolls.insert(found, sample);
}

extern "C" void jfc_record_object_texture_scroll(uint8_t *rdram, recomp_context *ctx) {
    const int32_t first = int32_t(ctx->r18), end = int32_t(ctx->r14);
    if (first >= end) return;
    const uint32_t model = uint32_t(ctx->r2);
    if (first < 0 || !valid_rdram_address(model, 0x24))
        throw std::runtime_error("Invalid native object scroll batch");
    const uint64_t address = uint64_t(uint32_t(MEM_W(0x20, model))) + uint64_t(first) * 16;
    if (address > UINT32_MAX) throw std::runtime_error("Native object scroll address overflow");
    jfc_record_texture_scroll(uint32_t(address), uint32_t(end - first), uint32_t(ctx->r4), uint32_t(ctx->r5),
        int32_t(ctx->r3), int32_t(ctx->r6));
}

namespace {

std::atomic_bool instant_pause_menu_tick_requested{false};

}

extern "C" void jfc_request_instant_pause_menu_tick(
    uint8_t*,
    recomp_context*
) {

    instant_pause_menu_tick_requested.store(true, std::memory_order_release);

}

extern "C" void jfc_apply_instant_pause_menu_tick(
    uint8_t*,
    recomp_context* ctx
) {
    if (!instant_pause_menu_tick_requested.exchange(
            false, std::memory_order_acq_rel) ||
        SIGNED(ctx->r4) <= 0) {
        return;
    }

    constexpr int32_t instant_transition_delta = 120;
    ctx->r4 = std::max<int32_t>(
        static_cast<int32_t>(SIGNED(ctx->r4)), instant_transition_delta);
}

extern "C" void jfc_use_model_matrix(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t model_matrix = 0x800FAAB0U;
    const uint32_t output = static_cast<uint32_t>(MEM_W(0, ctx->r16));
    if (!valid_rdram_address(model_matrix, 16U * sizeof(float)) ||
        !valid_rdram_address(output, 16U * sizeof(int32_t))) {
        return;
    }

    std::array<int32_t, 16> fixed{};
    for (uint32_t i = 0; i < fixed.size(); ++i) {
        const float value = read_rdram_float(rdram, model_matrix + i * sizeof(float));
        const float scaled = value * 65536.0f;
        if (!std::isfinite(scaled) ||
            scaled < -2147483648.0f ||
            scaled >= 2147483648.0f) {
            return;
        }
        fixed[i] = static_cast<int32_t>(std::nearbyintf(scaled));
    }

    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            const uint32_t value = static_cast<uint32_t>(fixed[row * 4 + column]);
            const uint32_t element = (row * 4 + column) * sizeof(uint16_t);
            MEM_H(element, output) = static_cast<int16_t>(value >> 16);
            MEM_H(0x20U + element, output) = static_cast<int16_t>(value);
        }
    }
    pending_matrix_multiply = true;
}

extern "C" void jfc_tag_procedural_sky(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t command = static_cast<uint32_t>(ctx->r3);
    if (!valid_rdram_address(command, sizeof(RT64::DisplayList)))
        throw std::runtime_error("Procedural sky vertex command is outside RDRAM");
    const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, command));
    const uint32_t w1 = static_cast<uint32_t>(MEM_W(4, command));
    if (w0 != (0x04480062U | ((w1 & 6U) << 16)))
        throw std::runtime_error("Procedural sky vertex command shape changed");
    MEM_W(0, command) = w0 | RT64::GBI_F3DDKR::ProceduralSkyVertexMarker;
}

extern "C" void jfc_tag_last_matrix(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t cursor = static_cast<uint32_t>(MEM_W(0, ctx->r4));
    if (cursor < sizeof(RT64::DisplayList)) {
        return;
    }

    const uint32_t command = cursor - sizeof(RT64::DisplayList);
    if (!valid_rdram_address(command, sizeof(RT64::DisplayList))) {
        return;
    }

    const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, command));
    const uint32_t w1 = static_cast<uint32_t>(MEM_W(4, command));
    if ((w0 >> 24) != 0x01U || ((w0 >> 16) & 0xFU) != 1U) {
        return;
    }

    const uint32_t tagged_w0 = RT64::GBI_F3DDKR::withMatrixTagW0(
        w0, pending_matrix_tag);
    MEM_W(0, command) = pending_matrix_multiply
        ? RT64::GBI_F3DDKR::withMatrixMultiplyW0(tagged_w0)
        : tagged_w0;
    uint32_t tagged_w1 = RT64::GBI_F3DDKR::withMatrixTagW1(w1, pending_matrix_tag) &
        ~RT64::GBI_F3DDKR::SkyMatrixTagW1;

    const uint32_t sky = static_cast<uint32_t>(MEM_W(0, 0x800F2F88U));
    if (sky != 0 && valid_rdram_address(sky, 0x18U) &&
        pending_matrix_tag == ((sky >> 2) & RT64::GBI_F3DDKR::MatrixTagIdMask)) {
        tagged_w1 |= RT64::GBI_F3DDKR::SkyMatrixTagW1;
    }
    MEM_W(4, command) = tagged_w1;
    capture_matrix_generation(rdram, w1, pending_matrix_tag << 2);
    pending_matrix_multiply = false;
}

const char* jfc::enhancements::try_tag_model_bone_dma(
    uint8_t* rdram, uint32_t command, uint32_t instance, uint32_t model
) noexcept {
    using namespace RT64::GBI_F3DDKR;
    if (!valid_rdram_address(command, 8U) || (command & 7U) ||
        !valid_rdram_address(instance, 0x98U) || (instance & 7U) ||
        !valid_rdram_address(model, 0x50U)) {
        return "invalid command, instance or model address";
    }

    if (MEM_B(0x4E, model) == 0) {
        return nullptr;
    }
    const int32_t count = MEM_B(0x4F, model);
    const uint32_t bank0 = static_cast<uint32_t>(MEM_W(0x10, instance));
    const uint32_t bank1 = static_cast<uint32_t>(MEM_W(0x14, instance));
    if (count == 0 && bank0 == 0 && bank1 == 0) {
        return nullptr;
    }
    if (count <= 0 || count > 127 || bank0 != instance + 0x98U ||
        !valid_rdram_address(bank0, static_cast<uint32_t>(count) * 128U) ||
        bank1 != bank0 + static_cast<uint32_t>(count) * 64U) {
        return "bone bank layout disagrees with native allocation";
    }
    const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, command));
    const uint32_t w1 = static_cast<uint32_t>(MEM_W(4, command));
    const uint32_t base = w0 & 0x00FFFFFFU;
    const uint32_t physical0 = bank0 & 0x1FFFFFFFU;
    const uint32_t physical1 = bank1 & 0x1FFFFFFFU;
    if ((w0 >> 24) != F3DDKR_G_DMA_OFFSETS || (base & 7U) ||
        (base != physical0 && base != physical1) || (w1 & 0xFF000000U)) {
        return "command does not select its declared model bank";
    }
    MEM_W(0, command) = static_cast<int32_t>(boneDMAW0(w0, count, base == physical1));
    uint32_t tagged_w1 = boneDMAW1(w1, count);
    const uint32_t sky = static_cast<uint32_t>(MEM_W(0, 0x800F2F88U));
    if (sky != 0 && valid_rdram_address(sky, 0x70U)) {
        const int32_t selected = MEM_B(0x3A, sky);
        const uint32_t instances = static_cast<uint32_t>(MEM_W(0x6C, sky));
        if (selected >= 0 && instances <= UINT32_MAX - uint32_t(selected) * 4U &&
            valid_rdram_address(instances + uint32_t(selected) * 4U, 4U) &&
            static_cast<uint32_t>(MEM_W(uint32_t(selected) * 4U, instances)) == instance) {
            tagged_w1 |= 0x80000000U;
        }
    }
    MEM_W(4, command) = static_cast<int32_t>(tagged_w1);
    return nullptr;
}

extern "C" void jfc_tag_model_bone_dma(
    uint8_t* rdram, uint32_t command, uint32_t instance, uint32_t model
) {
    if (const char* error = jfc::enhancements::try_tag_model_bone_dma(
            rdram, command, instance, model)) {
        std::fprintf(stderr,
            "JFG model DMA: %s (command=%08X instance=%08X model=%08X)\n",
            error, command, instance, model);
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }
    if (MEM_B(0x4E, model) != 0) {
        const int32_t count = MEM_B(0x4F, model);
        const uint32_t base = uint32_t(MEM_W(0, command)) & 0x00FFFFF8U;
        for (int32_t bone = 0; bone < count; ++bone)
            capture_matrix_generation(rdram, base + uint32_t(bone) * 64, instance);
    }
}

extern "C" void jfc_begin_camera_pass(uint32_t producer) noexcept(false) {
    if (producer < 0x80000000U || producer >= 0x90000000U || (producer & 3U))
        throw std::runtime_error("Invalid native camera producer site");
    camera_producer = producer;
}

extern "C" void jfc_capture_camera_sample(uint8_t* rdram, uint32_t role) noexcept(false) {
    const int32_t camera = MEM_W(0, 0x800FA634U);
    if (!camera_producer || camera < 0 || camera > 3 || role > 1)
        throw std::runtime_error("Missing native camera producer ownership");
    ultramodern::renderer::CameraSample sample;

    sample.cameraId = (camera_producer & 0x1FFFFFFCU) + uint32_t(camera) + 1U;
    camera_producer = 0;

    const uint32_t view = role == 0 ? 0x800FAF78U : 0x800FAFF8U;
    sample.billboardScale = read_rdram_float(rdram, 0x800A3168U);
    if (!std::isfinite(sample.billboardScale) || sample.billboardScale < 0)
        throw std::runtime_error("Invalid native billboard lens coefficient");
    for (uint32_t i = 0; i < 16; ++i) {
        sample.view[i] = read_rdram_float(rdram, view + i * 4);
        sample.projection[i] = read_rdram_float(rdram, 0x800FAA70U + i * 4);
        if (!std::isfinite(sample.view[i]) || !std::isfinite(sample.projection[i]))
            throw std::runtime_error("Non-finite native camera sample");
    }
    std::lock_guard lock(camera_samples_mutex);
    sample.generation = camera_generation;
    camera_float_sample = sample;
}

extern "C" void jfc_copy_camera_sample(uint8_t* rdram, uint32_t destination) noexcept(false) {
    const uint32_t address = destination & 0x1FFFFFFFU;
    const int32_t bank = MEM_W(0, 0x800FD7C0U);
    if (bank < 0 || bank > 1 || (address & 7U) || address > 0x800000U - 64)
        throw std::runtime_error("Invalid native camera matrix copy");
    std::lock_guard lock(camera_samples_mutex);
    if (!camera_float_sample.cameraId || camera_float_sample.generation != camera_generation)
        throw std::runtime_error("Camera matrix copy has no current float producer");
    auto sample = camera_float_sample;
    sample.matrixAddress = address;
    auto &samples = camera_samples[size_t(bank)];
    auto found = std::lower_bound(samples.begin(), samples.end(), address,
        [](const auto &entry, uint32_t key) { return entry.matrixAddress < key; });
    if (found != samples.end() && found->matrixAddress == address) *found = sample;
    else samples.insert(found, sample);
}

extern "C" void jfc_tag_saved_camera_matrix(uint8_t* rdram, uint32_t command) noexcept(false) {
    if (!valid_rdram_address(command, sizeof(RT64::DisplayList)))
        throw std::runtime_error("Invalid native camera command address");
    const uint32_t w0 = uint32_t(MEM_W(0, command));
    const uint32_t w1 = uint32_t(MEM_W(4, command));
    const uint32_t address = w1 & 0x00FFFFF8U;
    const int32_t bank = MEM_W(0, 0x800FD7C0U);
    if ((w0 >> 24) != 0x01U || ((w0 >> 16) & 0xFU) || ((w0 >> 22) & 3U) || bank < 0 || bank > 1)
        throw std::runtime_error("Invalid native camera restore command");
    std::lock_guard lock(camera_samples_mutex);
    const auto &samples = camera_samples[size_t(bank)];
    const auto found = std::lower_bound(samples.begin(), samples.end(), address,
        [](const auto &entry, uint32_t key) { return entry.matrixAddress < key; });
    if (found == samples.end() || found->matrixAddress != address)
        throw std::runtime_error("Camera restore has no saved matrix owner");
    MEM_W(4, command) = static_cast<int32_t>(
        RT64::GBI_F3DDKR::withCameraMatrixTagW1(w1));
}

extern "C" void jfc_tag_camera_matrix(uint8_t* rdram, uint32_t command) noexcept(false) {
    if (!valid_rdram_address(command, sizeof(RT64::DisplayList)))
        throw std::runtime_error("Invalid native camera command address");
    jfc_copy_camera_sample(rdram, uint32_t(MEM_W(4, command)) & 0x00FFFFF8U);
    jfc_tag_saved_camera_matrix(rdram, command);
}

extern "C" void jfc_camera_lifecycle_changed() noexcept(false) {
    std::lock_guard lock(camera_samples_mutex);
    if (camera_generation == UINT64_MAX) throw std::overflow_error("Camera lifecycle exhausted");
    ++camera_generation;
}

ultramodern::renderer::CameraSamples jfc::enhancements::graphics_task_cameras(
    uint8_t* rdram, uint32_t task) {
    if (!valid_rdram_address(task, 0x40)) throw std::runtime_error("Invalid camera task container");
    const uint32_t root = uint32_t(MEM_W(0x30, task)) & 0x03FFFFFFU;
    const int32_t capacity = MEM_W(0, 0x800FD7DCU);
    if (capacity < 0 || capacity > 0x100000) throw std::runtime_error("Invalid camera task arena capacity");
    std::lock_guard lock(camera_samples_mutex);
    for (uint32_t bank = 0; bank < 2; ++bank) {
        const uint32_t base = uint32_t(MEM_W(bank * 4, 0x800FB400U)) & 0x03FFFFFFU;
        if (root >= base && uint64_t(root - base) < uint64_t(capacity) * 8)
            return {camera_samples[bank], camera_samples_complete[bank]};
    }
    return {};
}

ultramodern::renderer::GraphicsTaskMetadata jfc::enhancements::graphics_task_metadata(uint8_t *rdram, uint32_t task) {
    ultramodern::renderer::GraphicsTaskMetadata metadata;
    metadata.cameras = graphics_task_cameras(rdram, task);
    {
        std::lock_guard lock(allocation_generations_mutex);
        metadata.textureScrolls = texture_scrolls;
    }
    const uint32_t root = uint32_t(MEM_W(0x30, task)) & 0x03FFFFFFU;
    const uint32_t capacity = uint32_t(MEM_W(0, 0x800FD7DCU));
    std::lock_guard lock(camera_samples_mutex);
    for (uint32_t bank = 0; bank < 2; ++bank) {
        const uint32_t base = uint32_t(MEM_W(bank * 4, 0x800FB400U)) & 0x03FFFFFFU;
        if (root >= base && uint64_t(root - base) < uint64_t(capacity) * 8) {
            metadata.matrices = matrix_generations[bank];
            metadata.matrixGenerationsComplete = matrix_generations_complete[bank];
            break;
        }
    }
    return metadata;
}

const char* jfc::enhancements::try_publish_native_frame_time(uint8_t* rdram) noexcept {
    if (rdram == nullptr) {
        return "missing RDRAM";
    }

    const int32_t ticks = MEM_W(0, 0x800A3374U);
    const uint32_t cadence = MEM_BU(0, 0x800FECACU);
    const int32_t bank = MEM_W(0, 0x800FD7C0U);
    const int32_t capacity = MEM_W(0, 0x800FD7DCU);
    if ((ticks != 12 && (ticks < 1 || ticks > 6)) || MEM_W(0, 0x80000300U) != 1) {
        return "invalid native NTSC frame interval";
    }
    if (cadence < 1 || cadence > 3) {
        return "invalid native NTSC presentation cadence";
    }
    if (bank < 0 || bank > 1 || capacity < 4 || capacity > 0x100000) {
        return "invalid native display-list allocation";
    }
    const uint32_t base = static_cast<uint32_t>(MEM_W(bank * 4, 0x800FB400U));
    const uint32_t cursor = static_cast<uint32_t>(MEM_W(0, 0x800FB408U));
    const uint32_t bytes = static_cast<uint32_t>(capacity) * 8U;
    if ((base & 7U) || (cursor & 7U) || !valid_rdram_address(base, bytes) ||
        cursor < base || cursor - base > bytes - 32U) {
        return "native display-list cursor exceeds allocation";
    }

    const std::array commands{
        NativeGfxCommand{RT64_HOOK_MAGIC_NUMBER, 0x50000064U},
        NativeGfxCommand{0x64000000U | G_EX_SETREFRESHRATE_V1,
            60U / cadence},

        NativeGfxCommand{0x64000000U | G_EX_SETNEARCLIPPING_V1, G_EX_ENABLED},
        NativeGfxCommand{RT64_HOOK_MAGIC_NUMBER, 0x60000000U},
    };
    append_gfx_commands(rdram, 0x800FB408U, commands);
    return nullptr;
}

extern "C" void jfc_publish_native_frame_time(uint8_t* rdram) {

    ultramodern::poll_pending_events(rdram);
    if (const char* error = jfc::enhancements::try_publish_native_frame_time(rdram)) {
        std::fprintf(stderr, "JFC native frame time: %s ticks=%d cadence=%u ntsc=%d bank=%d capacity=%d\n",
            error, rdram ? MEM_W(0, 0x800A3374U) : -1,
            rdram ? unsigned(MEM_BU(0, 0x800FECACU)) : 0U,
            rdram ? MEM_W(0, 0x80000300U) : -1,
            rdram ? MEM_W(0, 0x800FD7C0U) : -1,
            rdram ? MEM_W(0, 0x800FD7DCU) : -1);
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }
    ultramodern::set_audio_activity(jfc::modernization::audio_activity(rdram));
    jfc::framebuffer_effects::begin_frame(rdram);

    std::lock_guard lock(camera_samples_mutex);
    camera_samples[size_t(MEM_W(0, 0x800FD7C0U))].clear();
    camera_samples_complete[size_t(MEM_W(0, 0x800FD7C0U))] = true;
    matrix_generations[size_t(MEM_W(0, 0x800FD7C0U))].clear();
    matrix_generations_complete[size_t(MEM_W(0, 0x800FD7C0U))] = true;
}

extern "C" void jfc_request_cutscene_skip(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t start_button = 0x1000U;
    constexpr uint32_t skip_buttons = start_button | jfc::modern_input::cutscene_skip;
    if (!cutscene_skipping.load(std::memory_order_relaxed) ||
        (static_cast<uint32_t>(ctx->r17) & skip_buttons) == 0) {
        return;
    }
    const int engine_mode = MEM_W(0, gameplay_mode_address);
    const bool gameplay_active = engine_mode == 0;
    bool sequence_camera = false;
    if (gameplay_active) {
        for (unsigned view = 0; view < 4; ++view)
            sequence_camera |= MEM_W(view * 4, animation_camera_address) != 0;
    }
    const bool sequence_active = sequence_camera &&
        MEM_W(0, 0x800A74BCU) != 0 &&
        MEM_W(0, 0x800A74D4U) != 0;
    const bool object_active = gameplay_active && ctx->r2 != 0;
    if (!sequence_active && !object_active) {
        return;
    }
    sequence_skip_pending = sequence_active && !object_active;
    object_skip_pending = object_active;

    ctx->r17 = static_cast<uint32_t>(ctx->r17) & ~skip_buttons;
}

extern "C" void jfc_request_travel_skip(uint8_t* rdram, recomp_context* ctx) {

    constexpr uint32_t skip_buttons = 0x1000U | jfc::modern_input::cutscene_skip;
    const uint32_t pressed = static_cast<uint32_t>(ctx->r2);
    if (!cutscene_skipping.load(std::memory_order_relaxed) ||
        (pressed & skip_buttons) == 0) return;
    ctx->r2 = pressed | 0x8000U;

}

extern "C" void jfc_apply_cutscene_skip(uint8_t* rdram, recomp_context* ctx) {
    const bool skip_seen = jfc_prepare_seen_animseq_skip(rdram, ctx) != 0;
    if (sequence_skip_pending || skip_seen) {

        ctx->r4 = cutscene_skip_update;
        sequence_skip_pending = false;
    }
}

extern "C" void jfc_apply_object_cutscene_skip(recomp_context* ctx) {
    if (object_skip_pending) {
        ctx->r5 = cutscene_skip_update;
        object_skip_pending = false;
    }
}

extern "C" void jfc_begin_single_player_hud(uint8_t* rdram, recomp_context* ctx) {

    begin_single_player_hud(rdram, G_EX_SCALE_ORIGIN_TOP);
}

extern "C" void jfc_begin_companion_hud(uint8_t* rdram, recomp_context*) {

    begin_single_player_hud(rdram, G_EX_SCALE_ORIGIN_TOP,
        G_EX_SCALE_ONE, G_EX_ASPECT_ADJUST, true);
}

extern "C" void jfc_validate_companion_controller(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t state = uint32_t(ctx->r16);
    if (!valid_rdram_address(state, 0x98U)) return;

    const uint32_t port = MEM_BU(1, 0x800FB100U);
    if (port < 4 && MEM_BU(port * 6U + 4U, 0x800FB0C0U) == 0) return;
    MEM_B(0x94, state) = MEM_BU(0x94, state) & ~0x80U;
    MEM_B(0, 0x800A4FC8U) = 0;
}

extern "C" void jfc_begin_single_player_health_hud(
    uint8_t* rdram,
    recomp_context*
) {
    begin_single_player_hud(rdram, G_EX_SCALE_ORIGIN_BOTTOM);
}

void jfc::enhancements::set_output_aspect_ratio(float aspect_ratio) {
    const float valid_aspect = std::isfinite(aspect_ratio) && aspect_ratio > 0.0f
        ? aspect_ratio
        : 4.0f / 3.0f;
    if (output_aspect_ratio.exchange(valid_aspect, std::memory_order_relaxed) != valid_aspect)
        jfc::framebuffer_effects::invalidate_cursor();
}

extern "C" float jfc_culling_horizontal_tangent(uint8_t* rdram) {
    if (rdram == nullptr) throw std::runtime_error("Missing camera projection memory");

    const float x = read_rdram_float(rdram, 0x800FAA70U);
    const float w = -read_rdram_float(rdram, 0x800FAA9CU);
    const float aspect = output_aspect_ratio.load(std::memory_order_relaxed);
    const float tangent = (w / x) * std::max(1.0f, aspect / (4.0f / 3.0f));
    if (!std::isfinite(x) || x <= 0 || !std::isfinite(w) || w <= 0 ||
        !std::isfinite(tangent) || tangent <= 0) {
        throw std::runtime_error("Invalid camera visibility projection");
    }
    return tangent;
}

extern "C" void jfc_begin_single_player_weapon_panel(
    uint8_t* rdram,
    recomp_context* ctx
) {
    if (weapon_panel_scope_active ||
        !valid_rdram_address(display_list_cursor_address, sizeof(uint32_t))) {
        return;
    }
    const uint32_t cursor = static_cast<uint32_t>(
        MEM_W(0, display_list_cursor_address));
    constexpr uint32_t command_bytes = 12U * sizeof(NativeGfxCommand);
    if (!valid_rdram_address(cursor, command_bytes)) {
        return;
    }

    if (!begin_weapon_panel_projected_scope(rdram)) {
        return;
    }
    weapon_panel_scope_active = true;
    weapon_panel_content_scope_active = false;
    weapon_panel_display_list_start = cursor;
    weapon_panel_content_display_list_start = 0;
    weapon_panel_alpha = ctx != nullptr
        ? static_cast<int16_t>(ctx->r3)
        : -1;
    weapon_panel_player = ctx != nullptr
        ? static_cast<uint32_t>(ctx->r17)
        : 0U;
    weapon_panel_timer = valid_rdram_address(weapon_panel_player, 0x728U)
        ? MEM_H(0x592, weapon_panel_player)
        : -1;
    const uint32_t player_manager =
        valid_rdram_address(player_manager_address, sizeof(uint32_t))
            ? static_cast<uint32_t>(MEM_W(0, player_manager_address))
            : 0U;
    weapon_panel_text = valid_rdram_address(player_manager, 0x728U)
        ? static_cast<uint32_t>(MEM_W(0x724, player_manager))
        : 0U;

}

extern "C" void jfc_observe_weapon_selection_overlay(
    uint8_t* rdram,
    recomp_context*,
    uint32_t character,
    uint32_t animation_x_bits
) {
    if (!valid_rdram_address(character, 0x71U)) {
        return;
    }
    const float animation_x = std::bit_cast<float>(animation_x_bits);
    if (std::isfinite(animation_x) && animation_x < 55.0f) {

        begin_weapon_selection_scissor_scope(rdram);
    }
}

extern "C" void jfc_finish_weapon_selection_overlay(
    uint8_t* rdram,
    recomp_context*
) {
    end_weapon_selection_scissor_scope(rdram);
}

extern "C" void jfc_begin_single_player_weapon_panel_content(
    uint8_t* rdram,
    recomp_context*
) {
    if (!weapon_panel_scope_active || weapon_panel_content_scope_active) {
        return;
    }
    end_weapon_panel_projected_scope(rdram);
    weapon_panel_scope_active = false;
    if (valid_rdram_address(display_list_cursor_address, sizeof(uint32_t))) {
        weapon_panel_content_display_list_start = static_cast<uint32_t>(
            MEM_W(0, display_list_cursor_address));
    }
    begin_single_player_hud(rdram, G_EX_SCALE_ORIGIN_TOP);
    weapon_panel_content_scope_active = true;

}

extern "C" void jfc_end_single_player_weapon_panel(
    uint8_t* rdram,
    recomp_context* ctx
) {
    if (!weapon_panel_scope_active && !weapon_panel_content_scope_active) {
        return;
    }
    if (weapon_panel_scope_active) {
        end_weapon_panel_projected_scope(rdram);
        weapon_panel_scope_active = false;
    }
    if (weapon_panel_content_scope_active) {
        jfc_end_single_player_hud(rdram, ctx);
        weapon_panel_content_scope_active = false;
    }

    weapon_panel_display_list_start = 0;
    weapon_panel_content_display_list_start = 0;
    weapon_panel_alpha = -1;
    weapon_panel_timer = -1;
    weapon_panel_player = 0;
    weapon_panel_text = 0;
}

extern "C" void jfc_set_sprite_owner(uint32_t object) {
    sprite_owner = object;
}

extern "C" void jfc_tag_owned_sprite_matrix(
    uint8_t* rdram, recomp_context* ctx
) {
    using namespace RT64::GBI_F3DDKR;

    if (rdram == nullptr || ctx == nullptr || sprite_owner == 0 ||
        sprite_owner != static_cast<uint32_t>(ctx->r17) ||
        (sprite_owner & 3U) || !valid_rdram_address(sprite_owner, 0x70U)) {
        return;
    }
    const uint32_t command = static_cast<uint32_t>(ctx->r3);
    if ((command & 7U) || !valid_rdram_address(command, 8U)) {
        return;
    }
    const uint32_t w0 = static_cast<uint32_t>(MEM_W(0, command));
    const uint32_t w1 = static_cast<uint32_t>(MEM_W(4, command));
    if (w0 != 0x01020040U || (w1 & 7U) ||
        w1 > 8U * 1024U * 1024U - 64U) {
        return;
    }
    const uint32_t id = (sprite_owner >> 2) & MatrixTagIdMask;
    MEM_W(0, command) = withMatrixTagW0(w0, id);
    MEM_W(4, command) = withMatrixTagW1(w1, id);
    capture_matrix_generation(rdram, w1, sprite_owner);
}

extern "C" void jfc_begin_queued_pickup_scissor(
    uint8_t* rdram,
    recomp_context*
) {
    if (queued_pickup_text_scope_active ||
        !valid_rdram_address(display_list_cursor_address, sizeof(uint32_t))) {
        return;
    }

    const uint32_t cursor = static_cast<uint32_t>(
        MEM_W(0, display_list_cursor_address));

    constexpr std::array commands{
        NativeGfxCommand{0x64000000U | G_EX_PUSHSCISSOR_V1, 0U},
    };
    append_gfx_commands(rdram, commands);
    queued_pickup_text_scope_active =
        static_cast<uint32_t>(MEM_W(0, display_list_cursor_address)) ==
        cursor + commands.size() * sizeof(NativeGfxCommand);
    queued_pickup_text_aligned = false;
}

extern "C" void jfc_align_queued_pickup_text(
    uint8_t* rdram,
    recomp_context*
) {
    if (!queued_pickup_text_scope_active || queued_pickup_text_aligned ||
        !valid_rdram_address(display_list_cursor_address, sizeof(uint32_t))) {
        return;
    }

    const uint32_t cursor = static_cast<uint32_t>(
        MEM_W(0, display_list_cursor_address));

    constexpr uint32_t physical_left_inset = 1U * 4U;
    constexpr uint32_t left_origin_pair =
        G_EX_ORIGIN_LEFT | (G_EX_ORIGIN_LEFT << 12);
    const std::array commands{
        NativeGfxCommand{
            0x64000000U | G_EX_SETRECTASPECT_V1,
            G_EX_ASPECT_ADJUST},
        NativeGfxCommand{
            0x64000000U | G_EX_SETRECTALIGN_V1,
            left_origin_pair},
        NativeGfxCommand{
            physical_left_inset << 16,
            physical_left_inset << 16},
    };
    append_gfx_commands(rdram, commands);
    queued_pickup_text_aligned =
        static_cast<uint32_t>(MEM_W(0, display_list_cursor_address)) ==
        cursor + commands.size() * sizeof(NativeGfxCommand);
}

extern "C" void jfc_end_queued_pickup_text(
    uint8_t* rdram,
    recomp_context*
) {
    if (!queued_pickup_text_scope_active) {
        return;
    }
    if (queued_pickup_text_aligned) {
        constexpr uint32_t physical_left_inset = 1U * 4U;
        constexpr uint32_t left_origin_pair =
            G_EX_ORIGIN_LEFT | (G_EX_ORIGIN_LEFT << 12);
        const std::array commands{
            NativeGfxCommand{
                0x64000000U | G_EX_SETRECTASPECT_V1,
                G_EX_ASPECT_ADJUST},
            NativeGfxCommand{
                0x64000000U | G_EX_SETRECTALIGN_V1,
                left_origin_pair},
            NativeGfxCommand{
                physical_left_inset << 16,
                physical_left_inset << 16},
            NativeGfxCommand{0x64000000U | G_EX_POPSCISSOR_V1, 0U},
        };
        append_gfx_commands(rdram, commands);
    }
    else {
        constexpr std::array commands{
            NativeGfxCommand{0x64000000U | G_EX_POPSCISSOR_V1, 0U},
        };
        append_gfx_commands(rdram, commands);
    }
    queued_pickup_text_scope_active = false;
    queued_pickup_text_aligned = false;
}

extern "C" void jfc_begin_hud_number_filter(uint8_t* rdram, recomp_context*) {
    if (hud_number_filter_scope.active ||
        !valid_rdram_address(sprite_texture_filter_address, sizeof(uint32_t))) {
        return;
    }
    hud_number_filter_scope.prior = MEM_W(0, sprite_texture_filter_address);
    hud_number_filter_scope.active = true;
    MEM_W(0, sprite_texture_filter_address) = 0;
}

extern "C" void jfc_end_hud_number_filter(uint8_t* rdram, recomp_context*) {
    if (!hud_number_filter_scope.active ||
        !valid_rdram_address(sprite_texture_filter_address, sizeof(uint32_t))) {
        return;
    }
    MEM_W(0, sprite_texture_filter_address) = hud_number_filter_scope.prior;
    hud_number_filter_scope = {};
}

extern "C" void jfc_begin_guidance_draw(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint32_t>(MEM_W(0, 0x800A6384U)) == 0x80100960U) return;
    constexpr std::array commands{
        NativeGfxCommand{(RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{0x64000000U | G_EX_PUSHSCISSOR_V1, 0},
        NativeGfxCommand{0x64000000U | G_EX_SETRECTASPECT_V1, G_EX_ASPECT_ADJUST},
        NativeGfxCommand{0x64000000U | G_EX_SETDRAWSCALE_V1,
            packed_draw_scale(G_EX_SCALE_ONE, G_EX_ORIGIN_LEFT, G_EX_SCALE_ORIGIN_BOTTOM)},
        NativeGfxCommand{0x64000000U | G_EX_SETRECTALIGN_V1,
            G_EX_ORIGIN_LEFT | (G_EX_ORIGIN_LEFT << 12)},
        NativeGfxCommand{0, 0},
        NativeGfxCommand{0x64000000U | G_EX_SETSCISSORALIGN_V1,
            G_EX_ORIGIN_LEFT | (G_EX_ORIGIN_RIGHT << 12)},
        NativeGfxCommand{0, 0},
        NativeGfxCommand{0, 0x7FFF7FFFU},
        NativeGfxCommand{0x64000000U | G_EX_SETSCISSOR_V1, G_EX_ORIGIN_RIGHT << 14},
        NativeGfxCommand{0, 240 * 4},
    };
    append_gfx_commands(rdram, static_cast<uint32_t>(ctx->r19), commands);
}

extern "C" void jfc_end_guidance_draw(uint8_t* rdram, recomp_context* ctx) {

    if (static_cast<uint32_t>(MEM_W(0, 0x800A6384U)) == 0x80100960U) return;
    constexpr std::array commands{
        NativeGfxCommand{0x64000000U | G_EX_POPSCISSOR_V1, 0},
        NativeGfxCommand{(RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28},
    };
    append_gfx_commands(rdram, static_cast<uint32_t>(ctx->r19), commands);
}

extern "C" void jfc_end_single_player_hud(uint8_t* rdram, recomp_context*) {
    constexpr uint32_t no_origin_pair =
        G_EX_ORIGIN_NONE | (G_EX_ORIGIN_NONE << 12);
    constexpr std::array commands{
        NativeGfxCommand{
            0x64000000U | G_EX_SETDRAWSCALE_V1,
            packed_draw_scale(
                G_EX_SCALE_ONE,
                G_EX_ORIGIN_NONE,
                G_EX_SCALE_ORIGIN_CENTER)},
        NativeGfxCommand{0x64000000U | G_EX_SETRECTASPECT_V1, G_EX_ASPECT_AUTO},
        NativeGfxCommand{
            0x64000000U | G_EX_SETVIEWPORTALIGN_V1,
            G_EX_ORIGIN_NONE},
        NativeGfxCommand{0U, 0U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETRECTALIGN_V1,
            no_origin_pair},
        NativeGfxCommand{0U, 0U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETSCISSORALIGN_V1,
            no_origin_pair},
        NativeGfxCommand{0U, 0U},
        NativeGfxCommand{0U, 0x7FFF7FFFU},
        NativeGfxCommand{0x64000000U | G_EX_POPSCISSOR_V1, 0U},
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28},
    };
    append_gfx_commands(rdram, commands);
}

void begin_physical_transition_scope(
    uint8_t* rdram,
    uint32_t cursor_pointer,
    uint32_t kind
) {

    if ((kind == 2U || kind == 3U) &&
        (!valid_rdram_address(clear_screen_red_address, sizeof(uint8_t)) ||
         !valid_rdram_address(clear_screen_green_address, sizeof(uint8_t)) ||
         !valid_rdram_address(clear_screen_blue_address, sizeof(uint8_t)) ||
         MEM_BU(0, clear_screen_red_address) != 0U ||
         MEM_BU(0, clear_screen_green_address) != 0U ||
         MEM_BU(0, clear_screen_blue_address) != 0U)) {
        return;
    }
    if (physical_transition_scope_active ||
        !valid_rdram_address(cursor_pointer, sizeof(uint32_t))) {
        return;
    }
    const uint32_t cursor = static_cast<uint32_t>(MEM_W(0, cursor_pointer));

    const uint32_t video_mode = MEM_BU(0, 0x800FECA8U);
    const int32_t native_height = video_mode < 12U
        ? MEM_W(4, 0x800A392CU + video_mode * 40U) : 0;
    if (native_height <= 0 || native_height > 0x7FFF / 4) {
        std::fprintf(stderr,
            "JFC_TRANSITION_VIDEO_FAILURE mode=%u height=%d kind=%u\n",
            video_mode, native_height, kind);
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }

    const uint32_t aspect = kind == 5U ? G_EX_ASPECT_ADJUST : G_EX_ASPECT_COVER;
    const std::array commands{
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{0x64000000U | G_EX_PUSHSCISSOR_V1, 0U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETSCISSOR_V1,
            G_EX_ORIGIN_RIGHT << 14},
        NativeGfxCommand{0U, static_cast<uint32_t>(native_height) * 4U},
        NativeGfxCommand{
            0x64000000U | G_EX_SETRECTASPECT_V1,
            aspect},
    };
    append_gfx_commands(rdram, cursor_pointer, commands);
    physical_transition_scope_active =
        static_cast<uint32_t>(MEM_W(0, cursor_pointer)) ==
        cursor + commands.size() * sizeof(NativeGfxCommand);
    if (physical_transition_scope_active) {
        physical_transition_kind = kind;
        physical_transition_native_start = static_cast<uint32_t>(
            MEM_W(0, cursor_pointer));

    }
}

void end_physical_transition_scope(uint8_t* rdram, uint32_t cursor_pointer) {
    if (!physical_transition_scope_active) {
        return;
    }
    const uint32_t native_end = valid_rdram_address(
        cursor_pointer, sizeof(uint32_t))
        ? static_cast<uint32_t>(MEM_W(0, cursor_pointer))
        : 0U;

    constexpr std::array commands{
        NativeGfxCommand{
            0x64000000U | G_EX_SETRECTASPECT_V1,
            G_EX_ASPECT_AUTO},
        NativeGfxCommand{0x64000000U | G_EX_POPSCISSOR_V1, 0U},
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28},
    };
    append_gfx_commands(rdram, cursor_pointer, commands);
    physical_transition_scope_active = false;
    physical_transition_kind = 0;
    physical_transition_native_start = 0;
}

extern "C" void jfc_begin_transition_mask(
    uint8_t* rdram,
    uint32_t cursor_pointer
) {
    begin_physical_transition_scope(rdram, cursor_pointer, 0U);
}

extern "C" void jfc_end_transition_mask(
    uint8_t* rdram,
    uint32_t cursor_pointer
) {
    end_physical_transition_scope(rdram, cursor_pointer);
}

extern "C" void jfc_begin_transition_mask_kind(
    uint8_t* rdram,
    uint32_t cursor_pointer,
    uint32_t kind
) {
    begin_physical_transition_scope(rdram, cursor_pointer, kind);
}

extern "C" void jfc_begin_fullscreen_fade(
    uint8_t* rdram,
    recomp_context* ctx
) {
    constexpr uint32_t noisy_fade_amount_address = 0x801046EEU;
    if (ctx == nullptr ||
        !valid_rdram_address(noisy_fade_amount_address, sizeof(uint16_t)) ||
        MEM_H(0, noisy_fade_amount_address) == 0) {
        return;
    }
    begin_physical_transition_scope(
        rdram, static_cast<uint32_t>(ctx->r4), 7U);
}

extern "C" void jfc_end_fullscreen_fade(
    uint8_t* rdram,
    recomp_context* ctx
) {
    if (ctx != nullptr) {
        end_physical_transition_scope(rdram, static_cast<uint32_t>(ctx->r6));
    }
}

extern "C" void jfc_begin_starfield_batch(uint8_t* rdram, recomp_context* ctx) {
    constexpr std::array commands{
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{0x64000000U | G_EX_SETRECTBATCH_V1, G_EX_ENABLED},
    };
    uint32_t cursor = static_cast<uint32_t>(ctx->r5);
    if (!valid_rdram_address(cursor, sizeof(commands))) {
        return;
    }
    for (const NativeGfxCommand& command : commands) {
        MEM_W(0, cursor) = static_cast<int32_t>(command.w0);
        MEM_W(4, cursor) = static_cast<int32_t>(command.w1);
        cursor += sizeof(command);
    }
    ctx->r5 = cursor;
}

extern "C" void jfc_end_starfield_batch(uint8_t* rdram, recomp_context* ctx) {
    constexpr std::array commands{
        NativeGfxCommand{0x64000000U | G_EX_SETRECTBATCH_V1, G_EX_DISABLED},
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28},
    };
    uint32_t cursor = static_cast<uint32_t>(ctx->r5);
    if (!valid_rdram_address(cursor, sizeof(commands))) {
        return;
    }
    for (const NativeGfxCommand& command : commands) {
        MEM_W(0, cursor) = static_cast<int32_t>(command.w0);
        MEM_W(4, cursor) = static_cast<int32_t>(command.w1);
        cursor += sizeof(command);
    }
    ctx->r5 = cursor;
}

extern "C" uint32_t jfc_allow_player_weapon_cycle(uint8_t* rdram, uint32_t player) {
    if (rdram == nullptr || (player & 3U) != 0 || !valid_rdram_address(player, 0x5C4U)) {
        std::fprintf(stderr, "JFC weapon-cycle input: invalid player owner 0x%08X\n", player);
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }

    return MEM_W(0, animation_camera_address) == 0 &&
        MEM_W(0x5C0, player) == 0 && MEM_BU(0x18E, player) == 0 &&
        MEM_BU(0x1F9, player) == 0 && MEM_BU(0x1FA, player) == 0;
}

extern "C" void jfc_route_interaction_buttons(recomp_context* ctx) {

    if ((ctx->r2 & jfc::modern_input::interact) != 0) {
        ctx->r2 |= jfc::modern_input::a;
    }
}

static int32_t robot_pitch_input(uint8_t* rdram, uint32_t sidekick, int32_t input) {
    if (!modern_controls.load(std::memory_order_relaxed)) return input;
    if (!valid_rdram_address(sidekick, 0x28)) return input;
    const uint32_t object = uint32_t(MEM_W(0x24, sidekick));
    if (!valid_rdram_address(object, 0x6C)) return input;
    const uint32_t player = uint32_t(MEM_W(0x68, object));
    if (!valid_rdram_address(player, 0x10E) || MEM_BU(0x10D, player) != 1) return input;
    return int32_t(0U - uint32_t(input));
}

extern "C" void jfc_route_robot_pitch(uint8_t* rdram, recomp_context* ctx) {
    ctx->r3 = robot_pitch_input(rdram, uint32_t(ctx->r16), int32_t(ctx->r3));
}

extern "C" void jfc_route_robot_target_pitch(uint8_t* rdram, recomp_context* ctx) {

    const uint32_t sidekick = uint32_t(MEM_W(0x19C, ctx->r29));
    ctx->r16 = robot_pitch_input(rdram, sidekick, int32_t(ctx->r16));
}

extern "C" void jfc_route_modern_input(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<uint64_t>(ctx->r16) >= aims.size()) {
        return;
    }

    const size_t controller = static_cast<size_t>(ctx->r16);
    const NativeInputSnapshot physical_input = capture_native_input(rdram);
    discard_modern_player_input();
    const uint32_t held = static_cast<uint32_t>(MEM_W(0, control_keys_address));
    physical_aim_buttons[controller].store(
        (held & r_button) != 0, std::memory_order_relaxed);

    ModernPlayerScope& pending_scope = modern_player_scopes[controller];
    if (modern_controls.load(std::memory_order_relaxed) &&
        pending_scope.active && pending_scope.awaiting_input &&
        pending_scope.native_input_lockout &&
        valid_rdram_address(pending_scope.owner, 0x5C4U) &&
        MEM_B(0, pending_scope.owner) == static_cast<int>(controller)) {
        discard_mouse_counts(controller);

        const uint16_t sampled = sampled_direct_action_buttons[controller].load(
            std::memory_order_relaxed);
        suppressed_direct_action_buttons[controller].fetch_or(
            sampled, std::memory_order_relaxed);
        menu_direct_action_buttons[controller].store(
            0, std::memory_order_relaxed);
        routed_movement_buttons[controller].store(0, std::memory_order_relaxed);
        routed_direct_action_buttons[controller].store(
            0, std::memory_order_relaxed);
        modern_action_travel[controller] = {};

        pending_scope = {};
        set_expert_action_bindings(rdram, false);
        return;
    }

    {
        uint32_t native_held = held;
        uint32_t native_pressed = static_cast<uint32_t>(
            MEM_W(0, control_pressed_address));
        uint32_t native_released = static_cast<uint32_t>(
            MEM_W(0, control_released_address));
        const uint16_t sampled_raw =
            sampled_direct_action_buttons[controller].load(
                std::memory_order_relaxed);

        const uint16_t sampled = sampled_raw &
            ~suppressed_direct_action_buttons[controller].load(
                std::memory_order_relaxed);
        const uint16_t previous = menu_direct_action_buttons[controller].exchange(
            sampled, std::memory_order_relaxed);
        const auto route_menu_action = [&](uint16_t direct_bit,
                                           uint32_t native_bit) {
            if ((sampled & direct_bit) != 0) {
                native_held |= native_bit;
                native_released &= ~native_bit;
            }
            if ((sampled & ~previous & direct_bit) != 0) {
                native_pressed |= native_bit;
            }
            if ((previous & ~sampled & direct_bit) != 0 &&
                (native_held & native_bit) == 0) {
                native_released |= native_bit;
            }
        };
        route_menu_action(jfc::modern_input::c_up, jfc::modern_input::a);
        route_menu_action(jfc::modern_input::c_down, jfc::modern_input::b);
        native_released &= ~native_held;
        MEM_W(0, control_keys_address) = static_cast<int32_t>(native_held);
        MEM_W(0, control_pressed_address) = static_cast<int32_t>(native_pressed);
        MEM_W(0, control_released_address) = static_cast<int32_t>(native_released);

    }

    if (!modern_controls.load(std::memory_order_relaxed)) {
        modern_player_scopes[controller] = {};
        routed_movement_buttons[controller].store(0, std::memory_order_relaxed);
        routed_direct_action_buttons[controller].store(
            0, std::memory_order_relaxed);
        set_expert_action_bindings(rdram, false);
        return;
    }

    if (MEM_W(0, gameplay_mode_address) != 0) {
        modern_player_scopes[controller] = {};
        routed_movement_buttons[controller].store(0, std::memory_order_relaxed);
        routed_direct_action_buttons[controller].store(
            0, std::memory_order_relaxed);
        set_expert_action_bindings(rdram, false);
        const uint16_t sampled = sampled_direct_action_buttons[controller].load(
            std::memory_order_relaxed);
        suppressed_direct_action_buttons[controller].fetch_or(
            sampled, std::memory_order_relaxed);
        return;
    }

    ModernPlayerScope& scope = modern_player_scopes[controller];
    if (scope.active && scope.awaiting_input &&
        valid_rdram_address(scope.owner, 0x5C4U) &&
        MEM_B(0, scope.owner) == static_cast<int>(controller) &&
        modern_attached_camera_eligible(rdram, scope.owner)) {
        scope.input = physical_input;
        scope.awaiting_input = false;
        install_modern_player_input(rdram, controller);
        return;
    }

    modern_player_scopes[controller] = {};
    set_expert_action_bindings(rdram, false);
}

extern "C" void jfc_select_modern_control_table(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t player = static_cast<uint32_t>(ctx->r16);
    ModernPlayerScope* scope = nullptr;
    if (modern_controls.load(std::memory_order_relaxed) &&
        MEM_W(0, gameplay_mode_address) == 0) {
        scope = active_scope_for_player(rdram, player);
    }
    if (scope != nullptr) {

        MEM_W(0, control_mode_keys_address) =
            static_cast<int32_t>(control_mode_expert);
        set_expert_action_bindings(rdram, true);
    }
    else {
        set_expert_action_bindings(rdram, false);
    }
}

extern "C" void jfc_begin_modern_player_control(
    uint8_t* rdram,
    recomp_context* ctx
) {

    restore_modern_player_input(rdram);

    skip_borrowed_state_zero = false;

    const uint64_t revision = modern_controls_revision.load(
        std::memory_order_relaxed);
    if (revision != observed_modern_controls_revision) {
        modern_player_scopes = {};
        modern_action_travel = {};
        active_modern_input_controller = -1;
        observed_modern_controls_revision = revision;
    }

    const uint32_t player = static_cast<uint32_t>(ctx->r16);
    if (!valid_rdram_address(player, 0x5C4U)) {
        return;
    }
    const int controller = MEM_B(0, player);
    if (controller < 0 || static_cast<size_t>(controller) >= aims.size()) {
        return;
    }
    const size_t index = static_cast<size_t>(controller);
    const uint8_t state_before = static_cast<uint8_t>(MEM_B(0x568, player));
    ModernActionTravel& action_travel = modern_action_travel[index];
    if (action_travel.owner != player || !modern_action_state(state_before)) {
        action_travel = {};
    }

    ModernPlayerScope& scope = modern_player_scopes[index];
    scope = {};
    if (!modern_controls.load(std::memory_order_relaxed) ||
        MEM_W(0, gameplay_mode_address) != 0 ||
        !modern_attached_camera_eligible(rdram, player)) {
        discard_mouse_counts(index);
        routed_movement_buttons[index].store(0, std::memory_order_relaxed);
        routed_direct_action_buttons[index].store(0, std::memory_order_relaxed);
        set_expert_action_bindings(rdram, false);
        return;
    }
    const uint32_t stack = static_cast<uint32_t>(ctx->r29);
    if (!valid_rdram_address(stack + 0x90U, 8U)) {
        return;
    }
    const uint32_t object = static_cast<uint32_t>(MEM_W(0x90, stack));
    if (!valid_rdram_address(object, 0x6CU) ||
        static_cast<uint32_t>(MEM_W(0x68, object)) != player) {
        return;
    }
    scope.owner = player;
    scope.object = object;
    scope.active = true;

    scope.native_input_lockout =
        MEM_BU(0x56B, player) != 0U || MEM_BU(0x577, player) != 0U;
    scope.state_before = state_before;
    scope.facing_before = static_cast<int16_t>(MEM_H(0, object));
    scope.yaw_before = static_cast<int16_t>(MEM_H(0x11C, player));
    scope.pitch_before = static_cast<int16_t>(MEM_H(0x1E2, player));
    scope.position_before = {
        static_cast<uint32_t>(MEM_W(0x0C, object)),
        static_cast<uint32_t>(MEM_W(0x10, object)),
        static_cast<uint32_t>(MEM_W(0x14, object)),
    };

    scope.awaiting_input = true;
}

extern "C" void jfc_apply_modern_l2_movement_and_vertical_look(
    uint8_t* rdram,
    recomp_context* ctx,
    recomp_func_t* fine_aim_handler,
    uint32_t object,
    uint32_t player,
    int32_t movement_delta,
    int32_t heading,
    uint32_t timestep
) {
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || !scope->input.active ||
        !scope->axes.apply_isolated_l2_movement ||
        scope->state_before != 0U || scope->object != object ||
        fine_aim_handler == nullptr ||
        !valid_rdram_address(player, 0x1ECU) ||
        MEM_BU(0x568, player) != 0U) {
        return;
    }

    const uint32_t stack = static_cast<uint32_t>(ctx->r29);
    if (!valid_rdram_address(stack + 0x10U, sizeof(uint32_t))) {
        return;
    }

    const NativeInputSnapshot input = capture_native_input(rdram);
    constexpr uint32_t movement_button_mask =
        jfc::modern_input::c_left | jfc::modern_input::c_right |
        jfc::modern_input::c_up | jfc::modern_input::c_down;
    constexpr uint32_t state_zero_action_mask =
        ~(movement_button_mask | r_button);
    const bool authentic_action_edge =
        ((input.pressed | input.released) & state_zero_action_mask) != 0U;
    const uint32_t stack_argument = static_cast<uint32_t>(MEM_W(0x10, stack));
    const uint8_t state = MEM_BU(0x568, player);
    const uint8_t action_latch = MEM_BU(0x13E, player);
    const uint8_t action_state = MEM_BU(0x576, player);
    const uint16_t requested_movement =
        scope->axes.isolated_l2_movement_buttons;
    const bool directional_action_edge =
        (input.pressed & (modern_jump_button | modern_crouch_button)) != 0U &&
        requested_movement != 0U;
    const uint32_t movement_forward_before = static_cast<uint32_t>(
        MEM_W(0x4, player));
    const uint32_t movement_lateral_before = static_cast<uint32_t>(
        MEM_W(0x10, player));

    uint32_t canonical_forward_before = 0U;
    const bool canonical_action = directional_action_edge &&
        canonicalize_modern_action_velocity(
            movement_forward_before,
            movement_lateral_before,
            requested_movement,
            canonical_forward_before);
    const uint16_t borrowed_movement = canonical_action
        ? jfc::modern_input::c_up
        : requested_movement;
    if (canonical_action) {
        MEM_W(0x4, player) = static_cast<int32_t>(canonical_forward_before);
        MEM_W(0x10, player) = 0;
    }

    const uint32_t borrowed_actions = input.keys &
        ~(movement_button_mask | r_button |
          modern_jump_button | modern_crouch_button);
    MEM_W(0, control_keys_address) = static_cast<int32_t>(
        r_button | borrowed_movement | borrowed_actions);
    MEM_W(0, control_pressed_address) = 0;
    MEM_W(0, control_released_address) = 0;
    MEM_W(0, control_x_address) = scope->axes.isolated_l2_horizontal_look;
    MEM_W(0, control_raw_x_address) =
        scope->axes.isolated_l2_horizontal_look;
    MEM_W(0, control_y_address) = scope->axes.isolated_l2_vertical_look;
    MEM_W(0, control_raw_y_address) =
        scope->axes.isolated_l2_vertical_look;
    MEM_B(0x568, player) = 0x0BU;
    MEM_W(0x10, stack) = static_cast<int32_t>(timestep);

    recomp_context child;
    jfc::runtime::copy_context(child, *ctx);
    child.r4 = object;
    child.r5 = player;
    child.r6 = movement_delta;
    child.r7 = heading;

    scope->borrowed_l2_tail_active = true;
    scope->action_latch_before = action_latch;
    scope->action_state_before = action_state;
    fine_aim_handler(rdram, &child);

    scope->action_momentum_pending = canonical_action;
    if (canonical_action) {

        scope->action_momentum_before_forward_speed =
            movement_forward_before;
        scope->action_momentum_before_lateral_speed =
            movement_lateral_before;
        scope->action_momentum_forward_speed = static_cast<uint32_t>(
            MEM_W(0x4, player));
        scope->action_momentum_lateral_speed = static_cast<uint32_t>(
            MEM_W(0x10, player));
        scope->action_momentum_timestep = timestep;
        scope->action_momentum_buttons =
            requested_movement;
        scope->action_momentum_heading = static_cast<int16_t>(heading);
    }

    scope->borrowed_l2_tail_active = !authentic_action_edge;
    scope->action_latch_before = action_latch;
    scope->action_state_before = action_state;
    MEM_W(0x10, stack) = static_cast<int32_t>(stack_argument);
    if (authentic_action_edge) {
        MEM_B(0x568, player) = state;
        MEM_B(0x13E, player) = action_latch;
        MEM_B(0x576, player) = action_state;
        MEM_W(0, control_keys_address) = static_cast<int32_t>(input.keys);
        MEM_W(0, control_pressed_address) = static_cast<int32_t>(input.pressed);
        MEM_W(0, control_released_address) =
            static_cast<int32_t>(input.released);

        MEM_W(0, control_x_address) = 0;
        MEM_W(0, control_raw_x_address) = 0;
        MEM_W(0, control_y_address) = 0;
        MEM_W(0, control_raw_y_address) = 0;
    }
    scope->isolated_l2_applied = true;

    skip_borrowed_state_zero = !authentic_action_edge;
}

extern "C" int32_t jfc_modern_aim_entry_pitch(
    uint8_t* rdram, uint32_t player, int32_t native_pitch
) {
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || !scope->input.active ||
        (scope->input.keys & r_button) == 0U ||
        !valid_rdram_address(player, 0x569U)) {
        return native_pitch;
    }
    const uint8_t state = MEM_BU(0x568, player);
    if (!((scope->state_before == 0U && state == 11U) ||
          (scope->state_before == 1U && state == 5U))) {
        return native_pitch;
    }

    const int16_t yaw = scope->direct_yaw_pending
        ? scope->direct_yaw : scope->yaw_before;
    const int16_t pitch = scope->direct_pitch_pending
        ? scope->direct_pitch : scope->pitch_before;
    MEM_H(0x11C, player) = yaw;
    MEM_H(0x1E2, player) = pitch;
    scope->direct_yaw = yaw;
    scope->direct_pitch = pitch;
    scope->direct_yaw_pending = true;
    scope->direct_pitch_pending = true;
    scope->center_reticle_pending = true;

    return pitch;
}

extern "C" void jfc_prepare_modern_native_fine_aim(
    uint8_t* rdram,
    uint32_t player
) {
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || !scope->input.active ||
        MEM_BU(0x568, player) != 0x0BU ||
        (scope->input.keys & r_button) == 0U) {
        return;
    }

    write_control_axes(rdram, scope->axes);
}

extern "C" void jfc_restore_modern_action_momentum(
    uint8_t* rdram,
    uint32_t player
) {
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || !scope->action_momentum_pending ||
        !valid_rdram_address(player, 0x569U)) {
        return;
    }

    scope->action_momentum_pending = false;
    const uint8_t state_after = MEM_BU(0x568, player);

    if (modern_action_state(state_after)) {
        MEM_W(0x4, player) = static_cast<int32_t>(
            scope->action_momentum_forward_speed);
        MEM_W(0x10, player) = 0;
        const int controller = MEM_B(0, player);
        if (controller >= 0 &&
            static_cast<size_t>(controller) < modern_action_travel.size()) {
            modern_action_travel[static_cast<size_t>(controller)] = {
                player,
                scope->object,
                scope->action_momentum_heading,
                scope->facing_before,
                scope->yaw_before,
                true,
            };
        }

        preserve_modern_action_facing(rdram, *scope, state_after);
    }
    else {

        MEM_W(0x4, player) = static_cast<int32_t>(
            scope->action_momentum_before_forward_speed);
        MEM_W(0x10, player) = static_cast<int32_t>(
            scope->action_momentum_before_lateral_speed);
    }
}

extern "C" void jfc_prepare_modern_grounded_action_travel(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t player,
    uint32_t timestep_bits
) {
    if (ctx == nullptr || !modern_controls.load(std::memory_order_relaxed) ||
        !valid_rdram_address(player, 0x569U)) {
        return;
    }
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    const uint8_t state = MEM_BU(0x568, player);
    if (scope == nullptr || (state != 1U && state != 2U)) {
        return;
    }
    const int controller = MEM_B(0, player);
    if (controller < 0 ||
        static_cast<size_t>(controller) >= modern_action_travel.size()) {
        return;
    }
    ModernActionTravel& travel =
        modern_action_travel[static_cast<size_t>(controller)];
    float elapsed_vi = std::bit_cast<float>(timestep_bits);
    if (!std::isfinite(elapsed_vi) || elapsed_vi <= 0.0f) {
        elapsed_vi = 1.0f;
    }
    stage_modern_direct_look(
        rdram, *scope, static_cast<size_t>(controller), elapsed_vi);

    const uint16_t movement = scope->axes.isolated_l2_movement_buttons;
    const bool had_travel = travel.active && travel.owner == player;
    const int16_t prior_heading = travel.movement_heading;
    if (movement != 0U) {

        travel = {
            player,
            scope->object,
            static_cast<int16_t>(ctx->r7),
            scope->facing_before,
            scope->direct_yaw_pending ? scope->direct_yaw : scope->yaw_before,
            true,
        };
    }

}

extern "C" void jfc_prepare_modern_airborne_travel(
    uint8_t* rdram, recomp_context* ctx
) {
    if (ctx == nullptr) return;
    const uint32_t player = static_cast<uint32_t>(ctx->r5);
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (!modern_controls.load(std::memory_order_relaxed) || scope == nullptr ||
        !scope->input.active || scope->object != static_cast<uint32_t>(ctx->r4) ||
        (!scope->axes.apply_isolated_l2_movement && MEM_BU(0x14A, player) == 0) ||
        MEM_BU(0x568, player) != 3U) {
        return;
    }
    ModernActionTravel& travel = modern_action_travel[MEM_BU(0, player)];
    const bool owned = travel.active && travel.owner == player &&
        travel.object == scope->object;

    if (owned && scope->state_before == 3U) return;

    const float forward = read_rdram_float(rdram, player + 4U);
    const float lateral = read_rdram_float(rdram, player + 0x10U);
    const float magnitude = std::hypot(forward, lateral);
    if (!std::isfinite(magnitude)) return;
    int16_t heading = owned
        ? modern_action_composition_heading(rdram, player, travel, scope->state_before)
        : scope->yaw_before;
    if (magnitude > 0.0f) {

        constexpr double angle_units_per_radian = 65536.0 / 6.28318530717958647692;
        const int32_t offset = static_cast<int32_t>(std::lround(
            std::atan2(-static_cast<double>(lateral), -static_cast<double>(forward)) *
            angle_units_per_radian));
        heading = static_cast<int16_t>(static_cast<uint16_t>(heading) + offset);
        MEM_W(4, player) = std::bit_cast<int32_t>(-magnitude);
        MEM_W(0x10, player) = 0;
    }
    else if (scope->axes.isolated_l2_movement_buttons != 0U) {
        heading = static_cast<int16_t>(ctx->r7);
    }
    travel = {player, scope->object, heading, scope->facing_before,
              scope->yaw_before, true};

}

extern "C" void jfc_begin_modern_flight_steering(
    uint8_t* rdram, recomp_context* ctx, uint32_t player, uint32_t timestep_bits
) {
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (ctx == nullptr || !modern_controls.load(std::memory_order_relaxed) ||
        scope == nullptr || !scope->input.active || MEM_BU(0x568, player) != 3U ||
        MEM_BU(0x14A, player) == 0) return;

    recomp_context adoption = *ctx;
    adoption.r4 = scope->object;
    adoption.r5 = player;
    jfc_prepare_modern_airborne_travel(rdram, &adoption);
    ModernActionTravel& travel = modern_action_travel[MEM_BU(0, player)];
    if (!travel.active || travel.owner != player || travel.object != scope->object) return;

    const float elapsed = std::bit_cast<float>(timestep_bits);
    if (std::isfinite(elapsed) && elapsed >= 0.0f)
        stage_modern_direct_look(rdram, *scope, MEM_BU(0, player), elapsed);

    ctx->r4 = static_cast<int32_t>(travel.movement_heading);
    if (scope->axes.movement_x == 0 && scope->axes.movement_y == 0)
        ctx->r5 = ctx->r4;
}

extern "C" void jfc_end_modern_flight_steering(
    uint8_t* rdram, recomp_context* ctx, uint32_t player
) {
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (ctx == nullptr || !modern_controls.load(std::memory_order_relaxed) ||
        scope == nullptr || !scope->input.active || MEM_BU(0x568, player) != 3U ||
        MEM_BU(0x14A, player) == 0) return;
    ModernActionTravel& travel = modern_action_travel[MEM_BU(0, player)];
    if (!travel.active || travel.owner != player || travel.object != scope->object) return;
    travel.movement_heading = static_cast<int16_t>(ctx->r2);
    travel.yaw = scope->direct_yaw_pending ? scope->direct_yaw : scope->yaw_before;
    travel.facing = scope->direct_yaw_pending ? scope->direct_yaw : scope->facing_before;

    ctx->r2 = static_cast<int32_t>(travel.yaw);
}

extern "C" void jfc_select_modern_action_travel_heading(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t player
) {
    if (ctx == nullptr || !valid_rdram_address(player, 0x569U)) {
        return;
    }
    const int controller = MEM_B(0, player);
    if (controller < 0 ||
        static_cast<size_t>(controller) >= modern_action_travel.size()) {
        return;
    }

    ModernActionTravel& travel =
        modern_action_travel[static_cast<size_t>(controller)];
    const uint8_t state = MEM_BU(0x568, player);
    if (!modern_controls.load(std::memory_order_relaxed) || !travel.active ||
        travel.owner != player || !modern_action_state(state)) {
        travel = {};
        return;
    }
    const int16_t composition_heading = modern_action_composition_heading(
        rdram, player, travel, state);
    ctx->r6 = static_cast<int32_t>(composition_heading);
    ctx->r4 = static_cast<int32_t>(composition_heading);

}

extern "C" void jfc_select_modern_action_projection_basis(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t player
) {
    if (ctx == nullptr || !valid_rdram_address(player, 0x569U)) {
        return;
    }
    const int controller = MEM_B(0, player);
    if (controller < 0 ||
        static_cast<size_t>(controller) >= modern_action_travel.size()) {
        return;
    }

    const ModernActionTravel& travel =
        modern_action_travel[static_cast<size_t>(controller)];
    const uint8_t state = MEM_BU(0x568, player);
    const uint32_t stack = static_cast<uint32_t>(ctx->r29);
    if (!modern_controls.load(std::memory_order_relaxed) || !travel.active ||
        travel.owner != player || !modern_action_state(state) ||
        !valid_rdram_address(stack + 0x2CU, sizeof(int16_t))) {
        return;
    }

    const int16_t native_projection_heading = MEM_H(0x2C, stack);
    const int16_t composition_heading = modern_action_composition_heading(
        rdram, player, travel, state);
    const int16_t travel_projection_heading = static_cast<int16_t>(
        -static_cast<int32_t>(composition_heading));
    MEM_H(0x2C, stack) = travel_projection_heading;

}

extern "C" void jfc_validate_modern_action_travel(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t player
) {
    if (ctx == nullptr || !valid_rdram_address(player, 0x569U)) {
        return;
    }
    const int controller = MEM_B(0, player);
    if (controller < 0 ||
        static_cast<size_t>(controller) >= modern_action_travel.size()) {
        return;
    }
    ModernActionTravel& travel =
        modern_action_travel[static_cast<size_t>(controller)];
    const uint8_t state = MEM_BU(0x568, player);
    if (!modern_controls.load(std::memory_order_relaxed) || !travel.active ||
        travel.owner != player || !modern_action_state(state)) {
        travel = {};
        return;
    }

}

extern "C" void jfc_finalize_modern_character_dispatch(
    uint8_t* rdram,
    uint32_t player
) {
    ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || scope->owner != player ||
        !valid_rdram_address(player + 0x568U, sizeof(uint8_t))) {
        return;
    }
    const uint8_t state = MEM_BU(0x568, player);

    const bool crouched_attached_camera = state <= 2U || state == 5U ||
        (state == 3U && MEM_BU(0x14A, player) != 0);
    if (crouched_attached_camera && scope->direct_yaw_pending &&
        valid_rdram_address(scope->object, sizeof(int16_t))) {
        MEM_H(0, scope->object) = scope->direct_yaw;
    }

    if (scope->direct_yaw_pending &&
        valid_rdram_address(player + 0x11CU, sizeof(int16_t))) {
        MEM_H(0x11C, player) = scope->direct_yaw;
    }
    if (scope->direct_pitch_pending &&
        valid_rdram_address(player + 0x1E2U, sizeof(int16_t))) {
        MEM_H(0x1E2, player) = scope->direct_pitch;
    }
    if (scope->center_reticle_pending &&
        valid_rdram_address(player + 0x1ECU, sizeof(uint32_t))) {

        MEM_H(0x1CE, player) = 0;
        MEM_H(0x1DC, player) = 0;
        MEM_H(0x1E0, player) = 0;
        MEM_H(0x1DE, player) = MEM_H(0x1E2, player);
        MEM_W(0x1E4, player) = 0;
        MEM_W(0x1E8, player) = 0;
        scope->center_reticle_pending = false;
    }

    const int controller = MEM_B(0, player);
    if (controller < 0 ||
        static_cast<size_t>(controller) >= modern_action_travel.size()) {
        return;
    }
    ModernActionTravel& travel =
        modern_action_travel[static_cast<size_t>(controller)];
    if (modern_controls.load(std::memory_order_relaxed) && travel.active &&
        travel.owner == player && travel.object == scope->object &&
        !modern_action_state(state) && (state == 0U || state == 5U || state == 11U)) {

        const int16_t actor_heading = scope->direct_yaw_pending
            ? scope->direct_yaw : travel.yaw;
        const uint8_t previous_action = modern_action_state(scope->state_before)
            ? scope->state_before : 3U;
        const int16_t from_heading = modern_action_composition_heading(
            rdram, player, travel, previous_action);
        const int16_t angle = static_cast<int16_t>(
            static_cast<uint16_t>(from_heading) - static_cast<uint16_t>(actor_heading));
        const float forward = read_rdram_float(rdram, player + 4U);
        const float lateral = read_rdram_float(rdram, player + 0x10U);
        if (angle != 0 && std::isfinite(forward) && std::isfinite(lateral)) {
            constexpr double radians_per_angle_unit = 6.28318530717958647692 / 65536.0;
            const double radians = angle * radians_per_angle_unit;
            const double sine = angle == -32768 ? 0.0 : std::sin(radians);
            const double cosine = (angle == 16384 || angle == -16384)
                ? 0.0 : std::cos(radians);
            const float actor_forward = static_cast<float>(forward * cosine - lateral * sine);
            const float actor_lateral = static_cast<float>(forward * sine + lateral * cosine);
            MEM_W(4, player) = std::bit_cast<int32_t>(actor_forward);
            MEM_W(0x10, player) = std::bit_cast<int32_t>(actor_lateral);
        }
        MEM_H(0, scope->object) = actor_heading;
        MEM_H(0x11C, player) = actor_heading;

    }
    if (!modern_controls.load(std::memory_order_relaxed) || !travel.active ||
        travel.owner != player || !modern_action_state(state)) {
        if (travel.active && travel.owner == player) {
            travel = {};
        }
        return;
    }

    if (valid_rdram_address(travel.object, sizeof(int16_t))) {
        MEM_H(0, travel.object) =
            (scope->direct_yaw_pending &&
             (state <= 2U || (state == 3U && MEM_BU(0x14A, player) != 0)))
                ? scope->direct_yaw
                : travel.facing;
    }

    if (!scope->direct_yaw_pending) {
        MEM_H(0x11C, player) = travel.yaw;
    }
}

extern "C" uint32_t jfc_should_skip_borrowed_state_zero(
    uint8_t* rdram, uint32_t player
) {
    const ModernPlayerScope* const scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || !scope->input.active ||
        !scope->isolated_l2_applied || scope->state_before != 0U ||
        active_modern_input_controller != MEM_B(0, player)) {
        return 0;
    }

    const bool skip = skip_borrowed_state_zero;
    skip_borrowed_state_zero = false;
    return skip ? 1U : 0U;
}

extern "C" int32_t jfc_modern_locomotion_pose_heading(
    uint8_t* rdram, uint32_t player, int32_t native_heading
) {
    if (!modern_controls.load(std::memory_order_relaxed) ||
        active_scope_for_player(rdram, player) == nullptr) {
        return native_heading;
    }

    MEM_W(0x580, player) = 0;
    return 0;
}

extern "C" void jfc_end_modern_player_control(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t player = static_cast<uint32_t>(ctx->r16);
    if (valid_rdram_address(player, 1U)) {
        const int controller = MEM_B(0, player);
        if (controller >= 0 &&
            static_cast<size_t>(controller) < modern_player_scopes.size()) {
            ModernPlayerScope& scope = modern_player_scopes[
                static_cast<size_t>(controller)];
            if (scope.owner == player) {

                if (scope.borrowed_l2_tail_active) {
                    if (MEM_BU(0x568, player) == 0x0BU) {
                        MEM_B(0x568, player) = scope.state_before;
                    }
                    MEM_B(0x13E, player) = scope.action_latch_before;
                    MEM_B(0x576, player) = scope.action_state_before;
                    scope.borrowed_l2_tail_active = false;
                }
                preserve_modern_action_facing(
                    rdram, scope, scope.state_before);
                const bool isolated_l2_applied = scope.isolated_l2_applied;
                const uint16_t actions = sampled_direct_action_buttons[
                    static_cast<size_t>(controller)].load(
                        std::memory_order_relaxed);
                const bool physical_aim = physical_aim_buttons[
                    static_cast<size_t>(controller)].load(
                        std::memory_order_relaxed);
                const float aim_x = aims[static_cast<size_t>(controller)].x.load(
                    std::memory_order_relaxed);
                const float aim_y = aims[static_cast<size_t>(controller)].y.load(
                    std::memory_order_relaxed);
                const uint8_t state_after = static_cast<uint8_t>(
                    MEM_B(0x568, player));
                const int16_t pitch_after = static_cast<int16_t>(
                    MEM_H(0x1E2, player));
                const std::array<uint32_t, 3> position_after{
                    static_cast<uint32_t>(MEM_W(0x0C, scope.object)),
                    static_cast<uint32_t>(MEM_W(0x10, scope.object)),
                    static_cast<uint32_t>(MEM_W(0x14, scope.object)),
                };

                scope.awaiting_input = false;
            }
        }
    }
    restore_modern_player_input(rdram);
    skip_borrowed_state_zero = false;

    set_expert_action_bindings(rdram, false);
}

extern "C" void jfc_adjust_modern_manual_aim_rate(
    uint8_t* rdram,
    recomp_context* ctx
) {
    manual_aim_scope = {};
    if (active_modern_input_controller < 0 ||
        static_cast<size_t>(active_modern_input_controller) >=
            modern_player_scopes.size()) {
        return;
    }
    const ModernPlayerScope& scope = modern_player_scopes[
        static_cast<size_t>(active_modern_input_controller)];
    const uint32_t player = static_cast<uint32_t>(ctx->r5);
    if (!scope.active || !scope.input.active || scope.owner != player ||
        !valid_rdram_address(player, 0x1ECU)) {
        return;
    }

    const float camera_fov = valid_rdram_address(
        camera_fov_address, sizeof(float))
        ? read_rdram_float(rdram, camera_fov_address)
        : 0.0f;
    const float active_output_aspect =
        output_aspect_ratio.load(std::memory_order_relaxed);

    const jfc::modern_input::ManualAimBounds visible_bounds =
        boss_control_owns_aim(rdram, player)
        ? jfc::modern_input::ManualAimBounds{
            static_cast<int32_t>(ctx->r6), static_cast<int32_t>(ctx->r7)}
        : jfc::modern_input::visible_manual_aim_bounds(
            static_cast<int32_t>(ctx->r6),
            static_cast<int32_t>(ctx->r7),
            active_output_aspect,
            camera_fov);
    ctx->r6 = visible_bounds.horizontal;
    ctx->r7 = visible_bounds.vertical;

    const uint32_t stack = static_cast<uint32_t>(ctx->r29);
    manual_aim_scope.player = player;
    if (valid_rdram_address(stack, 0x20U)) {
        manual_aim_scope.horizontal_target_pointer = static_cast<uint32_t>(
            MEM_W(0x18, stack));
        manual_aim_scope.vertical_target_pointer = static_cast<uint32_t>(
            MEM_W(0x1C, stack));
    }
    manual_aim_scope.yaw_before = static_cast<int16_t>(MEM_H(0x11C, player));
    manual_aim_scope.pitch_before = static_cast<int16_t>(MEM_H(0x1E2, player));
    manual_aim_scope.horizontal_filter_before = static_cast<uint32_t>(
        MEM_W(0x1E4, player));
    manual_aim_scope.vertical_filter_before = static_cast<uint32_t>(
        MEM_W(0x1E8, player));
    const size_t controller_index = static_cast<size_t>(
        active_modern_input_controller);
    manual_aim_scope.physical_aim = jfc_scope_input_active(rdram, player, 1) != 0;
    manual_aim_scope.horizontal_stick = aims[controller_index].stick_x.load(
        std::memory_order_relaxed);
    manual_aim_scope.vertical_stick = aims[controller_index].stick_y.load(
        std::memory_order_relaxed);
    manual_aim_scope.horizontal_mouse = aims[controller_index].mouse_x.load(
        std::memory_order_relaxed);
    manual_aim_scope.vertical_mouse = aims[controller_index].mouse_y.load(
        std::memory_order_relaxed);
    if (valid_rdram_address(stack + 0x20U, sizeof(float))) {
        const float elapsed_vi = read_rdram_float(rdram, stack + 0x20U);
        if (std::isfinite(elapsed_vi) && elapsed_vi > 0.0f) {
            manual_aim_scope.elapsed_vi = elapsed_vi;
        }
    }
    manual_aim_scope.active = true;

    if (boss_control_owns_aim(rdram, player)) {

        const int32_t look_x = native_aim_axis(aims[controller_index].x.load(
            std::memory_order_relaxed));
        const int32_t look_y = native_aim_axis(aims[controller_index].y.load(
            std::memory_order_relaxed));
        MEM_W(0, control_x_address) = MEM_W(0, control_raw_x_address) = look_x;
        MEM_W(0, control_y_address) = MEM_W(0, control_raw_y_address) = look_y;
    }

}

extern "C" int jfc_scope_input_active(
    uint8_t* rdram, uint32_t player, int native_eligible
) {

    const auto* scope = active_scope_for_player(rdram, player);
    if (scope != nullptr && scope->input.active &&
        active_modern_input_controller == MEM_B(0, player)) {
        return native_eligible && (scope->input.keys & r_button) != 0;
    }
    return native_eligible != 0;
}

extern "C" void jfc_select_modern_scope(uint8_t*, recomp_context* ctx) {

    if (manual_aim_scope.active &&
        manual_aim_scope.player == static_cast<uint32_t>(ctx->r16) &&
        !manual_aim_scope.physical_aim) {
        ctx->r9 = 0;
    }
}

extern "C" void jfc_finalize_modern_manual_aim(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const ManualAimScope transaction = manual_aim_scope;
    manual_aim_scope = {};
    const uint32_t player = static_cast<uint32_t>(ctx->r16);
    if (!transaction.active || transaction.player != player ||
        !valid_rdram_address(player, 0x1F4U)) {
        return;
    }

    const double scope_scale = jfc::modern_input::scoped_look_scale(
        ctx->r2 == 1,
        std::bit_cast<float>(static_cast<uint32_t>(MEM_W(0x1EC, player))));
    const bool boss_aim = boss_control_owns_aim(rdram, player);

    const int16_t yaw_before = static_cast<int16_t>(transaction.yaw_before +
        (boss_aim ? MEM_H(0x1CE, player) : 0));
    const int16_t pitch_before = static_cast<int16_t>(transaction.pitch_before +
        (boss_aim ? MEM_H(0x1E0, player) : 0));
    const DirectLookAngles angles = modern_direct_look_angles(
        transaction.horizontal_stick,
        transaction.vertical_stick,
        transaction.horizontal_mouse,
        transaction.vertical_mouse,
        yaw_before,
        pitch_before,
        transaction.elapsed_vi,
        scope_scale);
    MEM_H(0x11C, player) = angles.yaw;
    ModernPlayerScope* const player_scope = active_scope_for_player(
        rdram, player);
    int16_t target_yaw = 0, target_pitch = 0;
    if (boss_aim) {

        const uint32_t stack = static_cast<uint32_t>(ctx->r29);
        if (player_scope == nullptr || !valid_rdram_address(stack, 0x70U)) {
            std::fprintf(stderr, "JFC boss aim lost native player/stack ownership\n");
            std::exit(EXIT_FAILURE);
        }
        const int32_t lower = MEM_W(0x68, stack);
        const int32_t upper = MEM_W(0x6C, stack);

        const int32_t horizontal = MEM_H(0x62, stack);
        const int32_t vertical = MEM_H(0x66, stack);
        constexpr int32_t body_yaw_limit = 0x2800;
        if (lower < INT16_MIN || upper > INT16_MAX || lower > upper ||
            horizontal < 0 || horizontal >= 0x8000 - body_yaw_limit ||
            vertical < 0 || lower - vertical < INT16_MIN || upper + vertical > INT16_MAX) {
            std::fprintf(stderr, "JFC invalid native boss aim bounds: %d %d %d %d\n",
                lower, upper, horizontal, vertical);
            std::exit(EXIT_FAILURE);
        }
        const int32_t relative_yaw_before = static_cast<int16_t>(
            uint16_t(yaw_before) - uint16_t(player_scope->yaw_before));
        const int32_t yaw_delta = static_cast<int16_t>(uint16_t(angles.yaw) - uint16_t(yaw_before));
        const int32_t relative_yaw = std::clamp<int32_t>(relative_yaw_before + yaw_delta,
            -body_yaw_limit - horizontal, body_yaw_limit + horizontal);
        const int32_t body_yaw = std::clamp(relative_yaw, -body_yaw_limit, body_yaw_limit);
        const int32_t total_pitch = std::clamp<int32_t>(angles.pitch, lower - vertical, upper + vertical);
        const int32_t body_pitch = std::clamp(total_pitch, lower, upper);
        target_yaw = static_cast<int16_t>(relative_yaw - body_yaw);
        target_pitch = static_cast<int16_t>(total_pitch - body_pitch);
        MEM_H(0x11C, player) = static_cast<int16_t>(player_scope->yaw_before + body_yaw);
        MEM_H(0x1E2, player) = static_cast<int16_t>(body_pitch);

        MEM_H(0x1CE, player) = target_yaw;
        MEM_H(0x1E0, player) = target_pitch;
        write_movement_geometry_axes(rdram, player_scope->axes);
    }
    else if (player_scope != nullptr) {

        player_scope->direct_yaw = angles.yaw;
        player_scope->direct_yaw_pending = true;
        player_scope->direct_pitch = angles.pitch;
        player_scope->direct_pitch_pending = true;
        player_scope->center_reticle_pending = true;
    }

    if (valid_rdram_address(
            transaction.horizontal_target_pointer, sizeof(int16_t))) {
        MEM_H(0, transaction.horizontal_target_pointer) = target_yaw;
    }
    if (valid_rdram_address(
            transaction.vertical_target_pointer, sizeof(int16_t))) {
        MEM_H(0, transaction.vertical_target_pointer) = target_pitch;
    }
}

extern "C" void jfc_suppress_modern_level_camera(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t player = static_cast<uint32_t>(ctx->r16);
    ModernPlayerScope* scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || boss_control_owns_aim(rdram, player)) {
        return;
    }

    ctx->r2 = 0;
}

extern "C" float jfc_modern_aim_response(uint8_t* rdram, uint32_t player, float authored) {
    const auto* scope = active_scope_for_player(rdram,player);
    if (!scope || !scope->input.active || !boss_control_owns_aim(rdram,player)) return authored;
    const unsigned controller=MEM_BU(0,player);
    if (controller>=aims.size() || (aims[controller].mouse_x.load(std::memory_order_relaxed)==0 &&
        aims[controller].mouse_y.load(std::memory_order_relaxed)==0)) return authored;
    if (!std::isfinite(authored) || authored<0 || authored>1)
        throw std::runtime_error("Invalid native boss aim response gain");
    return 1.0f;
}

extern "C" void jfc_select_modern_attached_camera(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t player = static_cast<uint32_t>(ctx->r16);
    ModernPlayerScope* scope = active_scope_for_player(rdram, player);
    if (scope == nullptr || boss_control_owns_aim(rdram, player)) {
        return;
    }

    ctx->r3 = 0x0BU;
}

extern "C" void jfc_prepare_target_reticle(
    uint8_t* rdram,
    recomp_context* ctx
) {
    target_reticle_interpolation = false;
    if (!modern_controls.load(std::memory_order_relaxed)) {
        target_reticle_visible = true;
        return;
    }
    const uint32_t player = static_cast<uint32_t>(ctx->r5);
    if (!valid_rdram_address(player, 1U)) {
        target_reticle_visible = true;
        return;
    }
    const int controller = MEM_B(0, player);
    target_reticle_interpolation = controller == 0 && MEM_W(0, gameplay_mode_address) == 0;
    target_reticle_visible = boss_control_owns_aim(rdram, player) || controller < 0 ||
        static_cast<size_t>(controller) >= physical_aim_buttons.size() ||
        physical_aim_buttons[controller].load(std::memory_order_relaxed);
}

extern "C" int jfc_should_draw_target_reticle() {
    return target_reticle_visible ? 1 : 0;
}

extern "C" int jfc_should_interpolate_target_reticle() {
    return target_reticle_interpolation && modern_controls.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" void jfc_adjust_projection(uint8_t* rdram, recomp_context* ctx) {
    const bool gameplay_scene = MEM_W(0, gameplay_mode_address) == 0;
    const std::array commands{
        NativeGfxCommand{
            (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_ENABLE << 28) | RT64_EXTENDED_OPCODE},
        NativeGfxCommand{
            0x64000000U | G_EX_SETMODERNLIGHTINGSCENE_V1,
            static_cast<uint32_t>(gameplay_scene ? G_EX_ENABLED : G_EX_DISABLED)},
    };
    append_gfx_commands(rdram, commands);

    fpr far_plane{};
    far_plane.u32l = static_cast<uint32_t>(MEM_W(0x14, ctx->r29));
    if (std::isfinite(far_plane.fl) && far_plane.fl > 0.0f) {
        far_plane.fl = jfc::display_options::scale_far_plane(
            far_plane.fl,
            draw_distance_percent.load(std::memory_order_relaxed));
        MEM_W(0x14, ctx->r29) = static_cast<int32_t>(far_plane.u32l);
    }
}

extern "C" void jfc_adjust_fog(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t near_address = static_cast<uint32_t>(MEM_W(0x24, ctx->r29));
    const uint32_t far_address = static_cast<uint32_t>(MEM_W(0x28, ctx->r29));
    if (!valid_rdram_address(near_address, sizeof(int16_t)) ||
        !valid_rdram_address(far_address, sizeof(int16_t))) {
        return;
    }
    const auto range = jfc::display_options::adjust_fog(
        static_cast<int16_t>(MEM_H(0, near_address)),
        static_cast<int16_t>(MEM_H(0, far_address)),
        fog_strength_percent.load(std::memory_order_relaxed));
    MEM_H(0, near_address) = range.near;
    MEM_H(0, far_address) = range.far;
}

#define JFC_ENHANCEMENT_STATE_FIELDS(X) \
    X(pending_matrix_tag) X(pending_matrix_multiply) X(sprite_owner) \
    X(sequence_skip_pending) X(object_skip_pending) X(target_reticle_visible) \
    X(hud_number_filter_scope) X(weapon_panel_scope_active) \
    X(weapon_panel_content_scope_active) X(weapon_panel_display_list_start) \
    X(weapon_panel_content_display_list_start) X(weapon_panel_alpha) \
    X(weapon_panel_timer) X(weapon_panel_player) X(weapon_panel_text) \
    X(queued_pickup_text_scope_active) X(queued_pickup_text_aligned) \
    X(weapon_selection_scissor_scope_active) X(physical_transition_scope_active) \
    X(physical_transition_kind) X(physical_transition_native_start) \
    X(modern_player_scopes) X(modern_action_travel) \
    X(active_modern_input_controller) X(skip_borrowed_state_zero) X(manual_aim_scope)

struct jfc::state::PreparedEnhancements::Impl {
#define DECLARE_FIELD(name) decltype(::name) name{};
    JFC_ENHANCEMENT_STATE_FIELDS(DECLARE_FIELD)
#undef DECLARE_FIELD
    std::array<std::array<uint16_t, 4>, 4> routed{};
    bool modern_mode = false, revision_current = false, pause_tick = false;
    bool committed = false;
    std::array<ultramodern::renderer::CameraSamples, 2> cameras{{{{}, false}, {{}, false}}};
    ultramodern::renderer::CameraSample float_camera;
    uint32_t producer = 0;
    uint64_t generation = 1;
    jfc::AllocationGenerations allocations;
    std::array<std::vector<ultramodern::renderer::MatrixGeneration>, 2> matrices;
    std::array<bool, 2> matrices_complete{false, false};
};

namespace {
template<class Archive, class Sample> void transfer_camera_sample(Archive& a, Sample& s) {
    a.fields(s.matrixAddress, s.cameraId, s.generation);
    for (auto& value : s.view) a.fields(value);
    for (auto& value : s.projection) a.fields(value);
}

void validate_camera_sample(const ultramodern::renderer::CameraSample& sample, uint64_t generation,
    bool scratch = false) {
    if (scratch && sample.cameraId == 0) {
        if (sample.matrixAddress || sample.generation || sample.billboardScale != 0 ||
            std::any_of(sample.view.begin(), sample.view.end(), [](float v) { return v != 0; }) ||
            std::any_of(sample.projection.begin(), sample.projection.end(), [](float v) { return v != 0; }))
            throw std::runtime_error("Invalid empty saved camera producer");
        return;
    }
    if (!sample.cameraId || sample.cameraId > 0x10000000U ||
        !std::isfinite(sample.billboardScale) || sample.billboardScale < 0 ||
        !sample.generation || sample.generation > generation ||
        (sample.matrixAddress & 7U) || sample.matrixAddress > 0x800000U - 64 ||
        (scratch && sample.matrixAddress != 0))
        throw std::runtime_error("Invalid saved camera identity or matrix address");
    for (uint32_t i = 0; i < 16; ++i)
        if (!std::isfinite(sample.view[i]) || !std::isfinite(sample.projection[i]))
            throw std::runtime_error("Non-finite saved camera matrix");
    if (sample.view[3] != 0 || sample.view[7] != 0 || sample.view[11] != 0 || sample.view[15] != 1)
        throw std::runtime_error("Saved camera view is not affine");
}

template<class State> void validate_camera_state(const State& s) {
    if (!s.generation || s.generation == UINT64_MAX ||
        (s.producer && (s.producer < 0x80000000U || s.producer >= 0x90000000U || (s.producer & 3U))))
        throw std::runtime_error("Invalid saved camera lifecycle or producer");
    validate_camera_sample(s.float_camera, s.generation, true);
    for (const auto& bank : s.cameras) {
        if (bank.entries.size() > 0x800000U / 64)
            throw std::runtime_error("Invalid saved camera bank size");
        uint32_t previous = 0;
        bool first = true;
        for (const auto& sample : bank.entries) {
            validate_camera_sample(sample, s.generation);
            if (!first && sample.matrixAddress <= previous)
                throw std::runtime_error("Saved camera matrix addresses are not unique and sorted");
            previous = sample.matrixAddress;
            first = false;
        }
    }
}

template<class Archive, class State> void transfer_enhancement_state(Archive& a, State& s) {
    a.fields(s.pending_matrix_tag, s.pending_matrix_multiply, s.sprite_owner,
        s.sequence_skip_pending, s.object_skip_pending, s.target_reticle_visible,
        s.hud_number_filter_scope.prior, s.hud_number_filter_scope.active,
        s.weapon_panel_scope_active, s.weapon_panel_content_scope_active,
        s.weapon_panel_display_list_start, s.weapon_panel_content_display_list_start,
        s.weapon_panel_alpha, s.weapon_panel_timer, s.weapon_panel_player, s.weapon_panel_text,
        s.queued_pickup_text_scope_active, s.queued_pickup_text_aligned,
        s.weapon_selection_scissor_scope_active, s.physical_transition_scope_active,
        s.physical_transition_kind, s.physical_transition_native_start);
    for (auto& p : s.modern_player_scopes) {
        a.fields(p.owner, p.object, p.active, p.awaiting_input, p.native_input_lockout,
            p.state_before, p.facing_before, p.yaw_before, p.pitch_before);
        for (auto& position : p.position_before) a.fields(position);
        a.fields(p.isolated_l2_applied, p.borrowed_l2_tail_active, p.action_momentum_pending,
            p.action_momentum_before_forward_speed, p.action_momentum_before_lateral_speed,
            p.action_momentum_forward_speed, p.action_momentum_lateral_speed,
            p.action_momentum_timestep, p.action_momentum_buttons, p.action_momentum_heading,
            p.direct_pitch_pending, p.direct_pitch, p.direct_yaw_pending, p.direct_yaw,
            p.center_reticle_pending, p.action_latch_before, p.action_state_before);
        a.fields(p.input.active, p.input.keys, p.input.pressed, p.input.released,
            p.input.x, p.input.raw_x, p.input.y, p.input.raw_y);
        a.fields(p.axes.movement_x, p.axes.movement_raw_x, p.axes.movement_y, p.axes.movement_raw_y,
            p.axes.control_x, p.axes.control_raw_x, p.axes.control_y, p.axes.control_raw_y,
            p.axes.isolated_l2_horizontal_look, p.axes.isolated_l2_vertical_look,
            p.axes.native_movement_buttons, p.axes.isolated_l2_movement_buttons, p.axes.apply_isolated_l2_movement);
    }
    for (auto& travel : s.modern_action_travel) {
        a.fields(travel.owner, travel.object, travel.movement_heading, travel.facing, travel.yaw, travel.active);
    }
    a.fields(s.active_modern_input_controller, s.skip_borrowed_state_zero);
    auto& aim = s.manual_aim_scope;
    a.fields(aim.player, aim.horizontal_target_pointer, aim.vertical_target_pointer,
        aim.yaw_before, aim.pitch_before, aim.horizontal_filter_before, aim.vertical_filter_before,
        aim.horizontal_stick, aim.vertical_stick, aim.horizontal_mouse, aim.vertical_mouse,
        aim.elapsed_vi, aim.active);
    for (auto& controller : s.routed) for (auto& buttons : controller) a.fields(buttons);
    a.fields(s.modern_mode, s.revision_current, s.pause_tick);
}

template<class State> void validate_enhancement_state(const State& s) {
    const auto address = [](uint32_t value, uint32_t length) {
        if (value != 0 && (!valid_rdram_address(value, length) || (value & 3U) != 0))
            throw std::runtime_error("Invalid save-state enhancement owner address");
    };
    if (s.pending_matrix_tag > RT64::GBI_F3DDKR::MatrixTagIdMask ||
        s.active_modern_input_controller < -1 || s.active_modern_input_controller >= 4)
        throw std::runtime_error("Invalid save-state enhancement identity");
    address(s.sprite_owner, 4); address(s.weapon_panel_player, 4);

    if (s.weapon_panel_text != 0 && !valid_rdram_address(s.weapon_panel_text, 1))
        throw std::runtime_error("Invalid save-state panel text address");
    address(s.weapon_panel_display_list_start, 4);
    address(s.weapon_panel_content_display_list_start, 4);
    address(s.physical_transition_native_start, 4);
    for (const auto& p : s.modern_player_scopes) { address(p.owner, 0x5c4); address(p.object, 4); }
    for (const auto& t : s.modern_action_travel) { address(t.owner, 0x5c4); address(t.object, 4); }
    const auto& aim = s.manual_aim_scope;
    address(aim.player, 0x5c4); address(aim.horizontal_target_pointer, 4); address(aim.vertical_target_pointer, 4);
    if (!std::isfinite(aim.horizontal_stick) || !std::isfinite(aim.vertical_stick) ||
        !std::isfinite(aim.horizontal_mouse) || !std::isfinite(aim.vertical_mouse) ||
        !std::isfinite(aim.elapsed_vi) || aim.elapsed_vi <= 0)
        throw std::runtime_error("Invalid save-state manual-aim sample");
}
}

jfc::state::Bytes jfc::state::capture_enhancements() {
    PreparedEnhancements::Impl state;
#define CAPTURE_FIELD(name) state.name = ::name;
    JFC_ENHANCEMENT_STATE_FIELDS(CAPTURE_FIELD)
#undef CAPTURE_FIELD
    for (size_t i = 0; i < state.routed.size(); ++i) {
        state.routed[i] = {routed_movement_buttons[i].load(), routed_direct_action_buttons[i].load(),
            menu_direct_action_buttons[i].load(), suppressed_direct_action_buttons[i].load()};
    }
    state.modern_mode = modern_controls.load();
    state.revision_current = observed_modern_controls_revision == modern_controls_revision.load();
    state.pause_tick = instant_pause_menu_tick_requested.load();
    {
        const std::scoped_lock lock(allocation_generations_mutex, camera_samples_mutex);
        for (size_t bank = 0; bank < state.cameras.size(); ++bank)
            state.cameras[bank] = {camera_samples[bank], camera_samples_complete[bank]};
        state.float_camera = camera_float_sample;
        state.producer = camera_producer;
        state.generation = camera_generation;
        state.allocations = allocation_generations;
        state.matrices = matrix_generations;
        state.matrices_complete = matrix_generations_complete;
    }
    validate_enhancement_state(state);
    validate_camera_state(state);
    Writer out; out.u32(4); transfer_enhancement_state(out, state);
    out.fields(state.generation, state.producer);
    transfer_camera_sample(out, state.float_camera);
    for (auto& bank : state.cameras) {
        out.fields(bank.complete, uint32_t(bank.entries.size()));
        for (auto& sample : bank.entries) transfer_camera_sample(out, sample);
    }
    state.allocations.validate();
    const size_t generation_offset = out.bytes.size();
    out.fields(state.allocations.serial, uint32_t(state.allocations.ranges.size()));
    for (auto &range : state.allocations.ranges) out.fields(range.begin, range.end, range.generation);
    for (size_t bank = 0; bank < state.matrices.size(); ++bank) {
        out.fields(state.matrices_complete[bank], uint32_t(state.matrices[bank].size()));
        for (auto &matrix : state.matrices[bank]) out.fields(matrix.address, matrix.generation);
    }

    out.fields(state.float_camera.billboardScale);
    for (auto &bank : state.cameras)
        for (auto &sample : bank.entries) out.fields(sample.billboardScale);
    out.u32(uint32_t(out.bytes.size() - generation_offset));
    return std::move(out.bytes);
}
jfc::state::PreparedEnhancements::PreparedEnhancements(std::span<const uint8_t> saved)
    : impl(std::make_unique<Impl>()) {
    Reader in(saved);
    const auto schema = in.u32();
    if (schema < 1 || schema > 4) throw std::runtime_error("Unsupported enhancement state schema");
    transfer_enhancement_state(in, *impl);
    if (schema >= 2) {
        in.fields(impl->generation, impl->producer);
        transfer_camera_sample(in, impl->float_camera);
        for (auto& bank : impl->cameras) {
            in.fields(bank.complete);
            const uint32_t count = in.u32();
            if (count > 0x800000U / 64) throw std::runtime_error("Oversized saved camera bank");
            for (uint32_t i = 0; i < count; ++i) {
                ultramodern::renderer::CameraSample sample;
                transfer_camera_sample(in, sample);
                validate_camera_sample(sample, impl->generation);
                bank.entries.push_back(sample);
            }
        }
    }
    if (schema >= 3) {
        const size_t generation_bytes = in.remaining_bytes();
        in.fields(impl->allocations.serial);
        const uint32_t count = in.u32();
        if (count > jfc::AllocationGenerations::MemorySize || count > in.remaining_bytes() / 16)
            throw std::runtime_error("Oversized saved allocation generation table");
        impl->allocations.ranges.resize(count);
        for (auto &range : impl->allocations.ranges) in.fields(range.begin, range.end, range.generation);
        impl->allocations.validate();
        for (size_t bank = 0; bank < impl->matrices.size(); ++bank) {
            in.fields(impl->matrices_complete[bank]);
            const uint32_t matrices = in.u32();
            if (matrices > 0x800000U / 8 || matrices > in.remaining_bytes() / 12)
                throw std::runtime_error("Oversized saved matrix generation bank");
            impl->matrices[bank].resize(matrices);
            uint32_t previous = 0;
            for (uint32_t i = 0; i < matrices; ++i) {
                auto &matrix = impl->matrices[bank][i];
                in.fields(matrix.address, matrix.generation);
                if ((matrix.address & 7U) || matrix.address > 0x800000U - 64 ||
                    (i && matrix.address <= previous) || matrix.generation > impl->allocations.serial)
                    throw std::runtime_error("Invalid saved matrix generation ownership");
                previous = matrix.address;
            }
        }
        if (schema >= 4) {
            in.fields(impl->float_camera.billboardScale);
            for (auto &bank : impl->cameras)
                for (auto &sample : bank.entries) in.fields(sample.billboardScale);
        }
        const size_t consumed = generation_bytes - in.remaining_bytes();
        if (in.u32() != consumed) throw std::runtime_error("Invalid allocation generation section length");
    }

    in.end(); validate_enhancement_state(*impl); validate_camera_state(*impl);
}
jfc::state::PreparedEnhancements::~PreparedEnhancements() = default;
void jfc::state::PreparedEnhancements::commit() noexcept {
    if (impl->committed) std::terminate();

    jfc_invalidate_hud_metadata();
    jfc::enhancements::clear_mouse_aim();
    jfc::framebuffer_effects::invalidate_cursor();
#define RESTORE_FIELD(name) ::name = impl->name;
    JFC_ENHANCEMENT_STATE_FIELDS(RESTORE_FIELD)
#undef RESTORE_FIELD
    for (size_t i = 0; i < impl->routed.size(); ++i) {
        routed_movement_buttons[i].store(impl->routed[i][0]);
        routed_direct_action_buttons[i].store(impl->routed[i][1]);
        menu_direct_action_buttons[i].store(impl->routed[i][2]);
        suppressed_direct_action_buttons[i].store(impl->routed[i][3]);
    }
    observed_modern_controls_revision = impl->revision_current && impl->modern_mode == modern_controls.load()
        ? modern_controls_revision.load() : 0;
    instant_pause_menu_tick_requested.store(impl->pause_tick);
    {
        const std::scoped_lock lock(allocation_generations_mutex, camera_samples_mutex);
        for (size_t bank = 0; bank < impl->cameras.size(); ++bank) {
            camera_samples[bank].swap(impl->cameras[bank].entries);
            camera_samples_complete[bank] = impl->cameras[bank].complete;
        }
        camera_float_sample = impl->float_camera;
        camera_producer = impl->producer;
        camera_generation = impl->generation;
        std::swap(allocation_generations, impl->allocations);
        texture_scrolls.clear();
        matrix_generations.swap(impl->matrices);
        matrix_generations_complete = impl->matrices_complete;
    }
    impl->committed = true;
}
#undef JFC_ENHANCEMENT_STATE_FIELDS
