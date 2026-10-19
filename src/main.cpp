#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "nfd.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "SDL_syswm.h"
#endif

#include "jfc_enhancements.hpp"
#include "crash_reporter.hpp"
#include "input_prompts.hpp"
#include "texture_launcher.hpp"
#include "jfc_modernization.hpp"
#include "weapon_qol.hpp"
#include "jfc_recomp.h"
#include "cpu_framebuffer_effects.hpp"
#include "modern_input.hpp"
#include "quick_switch_input.hpp"

#include "steam_deck_grips.hpp"
#include "save_state_rendezvous.hpp"
#include "save_state_memory.hpp"
#include "save_state_overlays.hpp"
#include "save_state_service.hpp"
#include "audio_output.hpp"
#include "audio_settings.hpp"
#include "camera_settings.hpp"
#include "librecomp/state.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "recompinput/input_events.h"
#include "recompinput/input_binding.h"
#include "recompinput/input_state.h"
#include "recompinput/profiles.h"
#include "recompinput/recompinput.h"
#include "weapon_binding_migration.hpp"
#include "recompui/config.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "elements/ui_document.h"
#include "elements/ui_label.h"
#include "game_hud.hpp"
#include "boss_health.hpp"
#include "ultramodern/ultramodern.hpp"
#include "common/rt64_performance_profiler.h"
#include "ultramodern/rt64_guest_vi_bridge.hpp"
#include "common/rt64_profiler_sha256.h"
#include "util/file.h"

#ifndef JFC_VERSION
#define JFC_VERSION "development"
#endif
#ifndef JFC_VERSION_MAJOR
#define JFC_VERSION_MAJOR 0
#endif
#ifndef JFC_VERSION_MINOR
#define JFC_VERSION_MINOR 0
#endif
#ifndef JFC_VERSION_PATCH
#define JFC_VERSION_PATCH 0
#endif
#ifndef JFC_CONFIG_SCHEMA_VERSION
#define JFC_CONFIG_SCHEMA_VERSION 0
#endif

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* context);
extern RspUcodeFunc jfg_audio;
#include "local_aot.hpp"
void jfc_load_bootstrap_overlay_data(uint8_t* rdram, recomp_context* context);
extern std::vector<recomp::GameEntry> supported_games;

extern "C" void jfc_complete_cpu_framebuffer(uint8_t* rdram, recomp_context* ctx, uint32_t address) {
    try {
        const uint32_t segment = address & 0xE0000000U;
        if (segment != 0x80000000U && segment != 0xA0000000U)
            throw std::invalid_argument("CPU image has no native RDRAM owner");
        const uint32_t stack = ctx ? uint32_t(ctx->r29) : 0;
        if (stack < 0x80000040U || stack > 0x80800000U)
            throw std::invalid_argument("No native stack for CPU image completion");
        ultramodern::renderer::complete_cpu_framebuffer(rdram, stack - 0x40U, address & 0x1FFFFFFFU,
            jfc::framebuffer_effects::pending());
        jfc::framebuffer_effects::completed();
    } catch (const ultramodern::thread_terminated&) {
        throw;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "JFC CPU image completion at 0x%08X: %s\n", address, error.what());
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }
}

extern "C" void jfc_wait_render_admission(uint8_t* rdram, recomp_context* ctx) {
    try {
        const uint32_t stack = ctx ? uint32_t(ctx->r29) : 0;
        if (stack < 0x80000040U || stack > 0x80800000U)
            throw std::invalid_argument("No native stack for render admission");
        RT64::PerformanceScope scope(RT64::PerformanceCategory::Wait, "Guest.RenderAdmission");
        const uint32_t taskKind = uint32_t(ctx->r6);
        if (taskKind != 0 && taskKind != 3 && taskKind != 4 && taskKind != 5)
            throw std::invalid_argument("Unsupported native graphics producer kind");
        ultramodern::renderer::wait_for_render_admission(rdram, stack - 0x40U, taskKind != 0);
    } catch (const ultramodern::thread_terminated&) {
        throw;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "JFC render admission: %s\n", error.what());
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }
}

SDL_Window* window = nullptr;

extern "C" jfc_actor_profile jfc_profile_actor_begin(
    uint8_t* rdram, uint32_t object) {
    jfc_actor_profile scope{};
    if (!RT64::PerformanceProfiler::enabled()) {
        return scope;
    }

    const auto valid = [](uint32_t address, uint32_t size) {
        return (address & 3U) == 0 && address >= 0x80000000U &&
            address <= 0x80800000U - size;
    };
    scope.object = object;
    scope.type = UINT32_MAX;
    scope.level = MEM_HU(0, 0x800A323CU);
    if (valid(object, 0x6CU)) {
        uint32_t actor = MEM_W(0x68, object);
        actor = valid(actor, 0x28U) ? MEM_W(0x24, actor) : 0;
        actor = valid(actor, 0x6CU) ? MEM_W(0x68, actor) : 0;
        if (valid(actor, 0x2EU)) {
            scope.type = MEM_HU(0x2C, actor);
        }
    }
    scope.start_ns = RT64::PerformanceProfiler::nowNanoseconds();
    return scope;
}

extern "C" void jfc_profile_actor_end(jfc_actor_profile scope, const char* name) {
    if (scope.start_ns != 0) {
        RT64::PerformanceProfiler::recordDuration(RT64::PerformanceCategory::CPU,
            name, scope.start_ns, RT64::PerformanceProfiler::nowNanoseconds(),
            scope.object, scope.type, scope.level);
    }
}

namespace {

void initialize_game_thread(uint8_t*, recomp_context* ctx) {
    constexpr gpr status_fr = 0x04000000;
    cop0_status_write(ctx, cop0_status_read(ctx) | status_fr);
}

constexpr std::u8string_view game_id = u8"jet-force-gemini-us-rev0";
constexpr char program_title[] = "Jet Force Gemini Recompiled " JFC_VERSION;
constexpr char modern_controls_option[] = "modern_controls";
constexpr char mouse_acceleration_option[] = "mouse_acceleration";
constexpr char joystick_sensitivity_option[] = "joystick_sensitivity";
constexpr char joystick_sensitivity_x_option[] = "joystick_sensitivity_x";
constexpr char joystick_sensitivity_y_option[] = "joystick_sensitivity_y";
constexpr char invert_right_stick_y_option[] = "invert_right_stick_y";
constexpr char invert_left_stick_x_option[] = "invert_left_stick_x";
constexpr char invert_left_stick_y_option[] = "invert_left_stick_y";
constexpr char invert_right_stick_x_option[] = "invert_right_stick_x";
constexpr std::array stick_inversion_options{invert_left_stick_x_option, invert_left_stick_y_option,
    invert_right_stick_x_option, invert_right_stick_y_option};
void load_stick_inversions(const recomp::config::Config& general) {
    recompinput::set_stick_axis_inversions(
        std::get<bool>(general.get_option_value(invert_left_stick_x_option)),
        std::get<bool>(general.get_option_value(invert_left_stick_y_option)),
        std::get<bool>(general.get_option_value(invert_right_stick_x_option)),
        std::get<bool>(general.get_option_value(invert_right_stick_y_option)));
}
constexpr char qol_config_id[] = "qol";
constexpr char cutscene_skipping_option[] = "cutscene_skipping";
constexpr char invulnerable_tribals_option[] = "invulnerable_tribals";
constexpr char persistent_tribal_rescues_option[] = "persistent_tribal_rescues";
constexpr char final_tribal_requirement_option[] = "final_tribal_requirement";
constexpr char skip_seen_landings_option[] = "skip_seen_landings";
constexpr char quick_character_switching_option[] = "quick_character_switching";
constexpr char skip_seen_character_travel_option[] = "skip_seen_character_travel";
constexpr char sprint_mode_option[] = "sprint_mode";
constexpr char double_pistol_recharge_option[] = "double_pistol_recharge";
constexpr char double_ammo_pickups_option[] = "double_ammo_pickups";
constexpr char unlimited_lives_option[] = "unlimited_lives";
constexpr char mission_complete_refill_option[] = "mission_complete_refill";
constexpr char double_enemy_drops_option[] = "double_enemy_drops";
constexpr char double_robot_mission_time_option[] = "double_robot_mission_time";
constexpr char hide_companion_option[] = "hide_companion";
constexpr char half_boss_health_option[] = "half_boss_health";
constexpr char boss_health_hud_option[] = "boss_health_hud";
constexpr char boss_weak_point_effect_option[] = "boss_weak_point_effect";
constexpr char mantis_final_vulnerability_option[] = "mantis_final_vulnerability";
constexpr char boss_pistol_damage_option[] = "boss_pistol_damage";
constexpr char boss_phase_ammo_option[] = "boss_phase_ammo";
constexpr char fast_throws_option[] = "fast_throws";
constexpr char halo_health_option[] = "halo_health";
constexpr char persistent_dropped_pickups_option[] = "persistent_dropped_pickups";
constexpr char persistent_corpses_option[] = "persistent_corpses";
constexpr char shotgun_upgrade_option[] = "shotgun_upgrade";
constexpr char flamethrower_upgrade_option[] = "flamethrower_upgrade";
constexpr char shocker_upgrade_option[] = "shocker_upgrade";
constexpr char modern_health_option[] = "modern_health";
constexpr char room_counters_option[] = "room_counters";
constexpr char area_pickups_option[] = "area_pickups";
constexpr char weapon_shortcuts_hud_option[] = "weapon_shortcuts_hud";
constexpr char ship_part_scanner_option[] = "ship_part_scanner";
constexpr char guidance_text_option[] = "guidance_text";
constexpr char draw_distance_option[] = "draw_distance";
constexpr char fog_strength_option[] = "fog_strength";
constexpr char full_hud_migration_option[] = "full_hud_migration";
constexpr char modernization_schema_option[] = "modernization_schema";

std::string make_settings_build_label() {
    return JFC_VERSION;
}

std::string settings_build_details() {
    const auto wrap_hash = [](const std::string& value) {
        return value.substr(0, 32) + "\n" + value.substr(32);
    };
    std::string details = "Version: " JFC_VERSION "\nMain commit:\n" JFC_MAIN_COMMIT
        "\nSource fingerprint:\n" + wrap_hash(JFC_SOURCE_FINGERPRINT);
    try {
        std::filesystem::path artifact;
        bool packaged = false;
#ifdef _WIN32
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetEnvironmentVariableW(L"JFC_PORTABLE_ARTIFACT", path.data(), DWORD(path.size()));
        if (length >= path.size()) throw std::runtime_error("Artifact path is too long");
        if (length != 0) {
            artifact = path.data();
            packaged = true;
        } else {
            const DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
            if (size == 0 || size >= path.size()) throw std::runtime_error("Cannot locate the running host");
            artifact = path.data();
        }
#else
        if (const char* image = std::getenv("APPIMAGE"); image && *image) {
            artifact = image;
            packaged = true;
        } else {
            artifact = std::filesystem::read_symlink("/proc/self/exe");
        }
#endif

        if (!std::filesystem::is_regular_file(artifact)) throw std::runtime_error("Artifact is not a regular file");
        const auto size = std::filesystem::file_size(artifact);
        const auto modified = std::filesystem::last_write_time(artifact);
        if (size == 0 || size > 2ULL * 1024 * 1024 * 1024) throw std::runtime_error("Artifact size is unsupported");
        std::ifstream input(artifact, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open artifact");
        RT64::ProfilerDetail::Sha256 hash;
        std::array<char, 65536> buffer;
        for (uint64_t remaining = size; remaining != 0;) {
            const auto count = std::min<uint64_t>(remaining, buffer.size());
            input.read(buffer.data(), std::streamsize(count));
            if (uint64_t(input.gcount()) != count) throw std::runtime_error("Cannot read complete artifact");
            hash.update(buffer.data(), size_t(count));
            remaining -= count;
        }
        if (input.peek() != std::char_traits<char>::eof() ||
            std::filesystem::file_size(artifact) != size ||
            std::filesystem::last_write_time(artifact) != modified)
            throw std::runtime_error("Artifact changed while reading");
        details += packaged ? "\nArtifact SHA256:\n" : "\nHost executable SHA256:\n";
        details += wrap_hash(hash.hex());
    } catch (const std::exception& error) {
        details += "\nArtifact SHA256 unavailable: ";
        details += error.what();
    }
    return details;
}

void queue_samples(int16_t* samples, size_t sample_count) {
    queue_audio_samples(samples, sample_count, recompui::config::sound::get_main_volume());
}

std::u8string startup_game;
std::atomic<uint64_t> vi_number = 0;

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint("SDL_MOUSE_RELATIVE_MODE_WARP", "0");
    SDL_SetHint("SDL_MOUSE_RELATIVE_SYSTEM_SCALE", "0");
    if (SDL_InitSubSystem(
            SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
            SDL_INIT_HAPTIC) != 0) {
        throw std::runtime_error(
            std::string("Failed to initialize SDL video/input: ") + SDL_GetError());
    }

    recompinput::discover_connected_controllers();
    return nullptr;
}

ultramodern::renderer::WindowHandle create_window(
    ultramodern::gfx_callbacks_t::gfx_data_t
) {
    uint32_t window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifndef _WIN32
    window_flags |= SDL_WINDOW_VULKAN;
#endif
    window = SDL_CreateWindow(
        program_title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1600,
        900,
        window_flags);
    if (window == nullptr) {
        throw std::runtime_error(
            std::string("Failed to create window: ") + SDL_GetError());
    }
#ifdef _WIN32
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE) {
        throw std::runtime_error(
            std::string("Failed to query native window: ") + SDL_GetError());
    }
    return {info.info.win.window, GetCurrentThreadId()};
#else
    return {window};
#endif
}

void apply_deck_snapshot(const jfc::deck_grips::Edges& deck_edges) {
    const auto bit = [](SDL_GameControllerButton button) { return 1U << button; };
    const uint32_t available = (deck_edges.upper_available ? bit(SDL_CONTROLLER_BUTTON_PADDLE1) |
        bit(SDL_CONTROLLER_BUTTON_PADDLE2) | bit(SDL_CONTROLLER_BUTTON_BACK) : 0) |
        (deck_edges.lower_available ? bit(SDL_CONTROLLER_BUTTON_PADDLE3) | bit(SDL_CONTROLLER_BUTTON_PADDLE4) : 0);
    const uint32_t held = (deck_edges.r4_held ? bit(SDL_CONTROLLER_BUTTON_PADDLE1) : 0) |
        (deck_edges.l4_held ? bit(SDL_CONTROLLER_BUTTON_PADDLE2) : 0) |
        (deck_edges.r5_held ? bit(SDL_CONTROLLER_BUTTON_PADDLE3) : 0) |
        (deck_edges.l5_held ? bit(SDL_CONTROLLER_BUTTON_PADDLE4) : 0) |
        (deck_edges.select_held ? bit(SDL_CONTROLLER_BUTTON_BACK) : 0);
    recompinput::set_controller_button_overlay(0x28DE, 0x1205, available, held);
    static uint32_t previous_raw = 0;
    for (unsigned button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
        if (!((previous_raw ^ held) & (1U << button))) continue;
        SDL_Event event{};
        event.type = (held & (1U << button)) ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        event.cbutton.which = recompinput::controller_overlay_source();
        event.cbutton.button = uint8_t(button);
        recompinput::sdl_event_filter(nullptr, &event, true);
    }
    previous_raw = held;
}

void update_gfx(void*) {

    recompinput::handle_events();
    jfc::deck_grips::poll(apply_deck_snapshot);
    const bool visuals_edge = recompinput::take_binding_press(recompinput::GameInput::TOGGLE_VISUALS);
    if (visuals_edge && !recompinput::binding::is_binding() && !recompinput::game_input_disabled()) {
        recompinput::pulse_visual_enhancement_toggle(recompinput::VisualToggleSource::SDL, "binding");
    }
    const bool save_edge = recompinput::take_binding_press(recompinput::GameInput::SAVE_STATE);
    const bool restore_edge = recompinput::take_binding_press(recompinput::GameInput::RESTORE_STATE);
    if (!recompinput::game_input_disabled() && ultramodern::is_game_started()) {

        if (restore_edge) jfc::state::restore();
        else if (save_edge) jfc::state::save();
    }

    static auto state_context = recompui::ContextId::null();
    static recompui::Label* state_label = nullptr;
    static std::chrono::steady_clock::time_point state_notice_until{};
    std::string state_title, state_message;
    if (jfc::state::take_notification(state_title, state_message)) {
        auto previous = recompui::try_close_current_context();
        if (state_context == recompui::ContextId::null()) {
            state_context = recompui::create_context();
            state_context.set_captures_input(false);
            state_context.set_captures_mouse(false);
            state_context.open();
            state_label = state_context.create_element<recompui::Label>(
                state_context.get_root_element(), "", recompui::LabelStyle::Small);
            state_label->set_position(recompui::Position::Absolute);
            state_label->set_left(24, recompui::Unit::Dp);
            state_label->set_bottom(24, recompui::Unit::Dp);
            state_label->set_max_width(600, recompui::Unit::Dp);
            state_label->set_padding(16, recompui::Unit::Dp);
            state_label->set_background_color(recompui::theme::color::BGOverlay);
        } else state_context.open();
        state_label->set_text(state_title + ": " + state_message);
        state_context.close();
        if (previous != recompui::ContextId::null()) previous.open();
        if (!recompui::is_context_shown(state_context)) recompui::show_context(state_context, "");
        state_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    }
    if (state_context != recompui::ContextId::null() &&
        std::chrono::steady_clock::now() >= state_notice_until && recompui::is_context_shown(state_context))
        recompui::hide_context(state_context);
    int output_width = 0;
    int output_height = 0;
    if (window != nullptr) {
        SDL_GetWindowSize(window, &output_width, &output_height);
    }
    const auto graphics = ultramodern::renderer::get_graphics_config();
    const bool expanded =
        graphics.ar_option != ultramodern::renderer::AspectRatio::Original;
    const float output_aspect = expanded && output_width > 0 && output_height > 0
        ? static_cast<float>(output_width) / static_cast<float>(output_height)
        : 4.0f / 3.0f;
    jfc::enhancements::set_output_aspect_ratio(output_aspect);

}

ultramodern::input::connected_device_info_t connected_device(int controller) {
    const bool connected = controller == 0;
    return {
        connected ? ultramodern::input::Device::Controller
                  : ultramodern::input::Device::None,
        connected ? ultramodern::input::Pak::RumblePak
                  : ultramodern::input::Pak::None,
    };
}

bool has_connected_game_controller() {
    const int joystick_count = SDL_NumJoysticks();
    for (int index = 0; index < joystick_count; ++index) {
        if (SDL_IsGameController(index) == SDL_TRUE) {
            return true;
        }
    }
    return false;
}

bool get_input(int controller, uint16_t* buttons, float* x, float* y) {
    bool result = false;
    const bool modern = jfc::enhancements::modern_controls_enabled();
    recompinput::set_right_analog_suppressed(modern);
    result = recompinput::profiles::get_n64_input(controller, buttons, x, y);
    recompinput::set_right_analog_suppressed(false);
    if (controller == 0) {
        jfc::input_prompts::refresh_bindings(
            has_connected_game_controller());
        const auto action_active = [](recompinput::GameInput input,
                                      recompinput::InputDevice source = recompinput::InputDevice::COUNT) {
            for (recompinput::InputDevice device : {
                     recompinput::InputDevice::Controller,
                     recompinput::InputDevice::Keyboard}) {
                if (source != recompinput::InputDevice::COUNT && device != source) continue;
                if (recompinput::binding_frame_pressed(0, input, device)) return true;
                const int profile = recompinput::profiles::get_input_profile_for_player(
                    0, device);
                if (profile < 0 ||
                    profile >= recompinput::profiles::get_input_profile_count()) {
                    continue;
                }
                for (size_t binding = 0;
                     binding < recompinput::num_bindings_per_input; ++binding) {
                    const auto field = recompinput::profiles::get_input_binding(profile, input, binding);
                    if (field.input_type != recompinput::InputType::MouseWheel && recompinput::get_input_digital(0, field)) {
                        return true;
                    }
                }
            }
            return false;
        };
        const bool actions_enabled = result &&
            !recompinput::game_input_disabled();

        if (actions_enabled &&
            (recompinput::get_input_digital(0, recompinput::InputField::keyboard(SDL_SCANCODE_SPACE)) ||
             recompinput::get_input_digital(0, recompinput::InputField::keyboard(SDL_SCANCODE_RETURN)) ||
             recompinput::get_input_digital(0, recompinput::InputField::keyboard(SDL_SCANCODE_KP_ENTER)))) {
            *buttons |= jfc::modern_input::cutscene_skip;
        }
        const bool switch_pad = action_active(recompinput::GameInput::QUICK_SWITCH,
            recompinput::InputDevice::Controller);
        const bool jump_active = actions_enabled && (action_active(recompinput::GameInput::JUMP));
        const bool crouch_active = actions_enabled && (action_active(recompinput::GameInput::CROUCH));
        jfc::enhancements::sample_direct_actions(
            0, jump_active, crouch_active);
        if (actions_enabled && (action_active(recompinput::GameInput::INTERACT))) {
            *buttons |= jfc::modern_input::interact;
        }

        if (jump_active) {
            *buttons |= jfc::modern_input::a;
        }
        if (crouch_active) {
            *buttons |= jfc::modern_input::b;
        }
        static jfc::QuickSwitchInput quick_switch;
        static jfc::QuickSwitchInput grenade;
        static jfc::QuickSwitchInput assign_throwable;
        static jfc::QuickSwitchInput stick_switch;
        static std::array<jfc::QuickSwitchInput, 4> recall, assign;
        const bool stick_chord = stick_switch.sample(actions_enabled,
            action_active(recompinput::GameInput::QUICK_SWITCH_STICKS, recompinput::InputDevice::Controller), true,
            action_active(recompinput::GameInput::QUICK_SWITCH_STICKS, recompinput::InputDevice::Keyboard), true);
        const bool qualified_switch = quick_switch.sample(actions_enabled,
            switch_pad,
            true,
            action_active(recompinput::GameInput::QUICK_SWITCH, recompinput::InputDevice::Keyboard),
            true);
        const bool quick_switch_active = qualified_switch || stick_chord;
        uint16_t weapon_actions = grenade.sample(actions_enabled,
            action_active(recompinput::GameInput::QUICK_GRENADE, recompinput::InputDevice::Controller),
            true,
            action_active(recompinput::GameInput::QUICK_GRENADE, recompinput::InputDevice::Keyboard),
            true) ? 0x100U : 0U;
        if (assign_throwable.sample(actions_enabled,
            action_active(recompinput::GameInput::ASSIGN_THROWABLE, recompinput::InputDevice::Controller), true,
            action_active(recompinput::GameInput::ASSIGN_THROWABLE, recompinput::InputDevice::Keyboard), true))
            weapon_actions |= 0x200U;
        constexpr std::array slot_inputs{recompinput::GameInput::WEAPON_SLOT_1,
            recompinput::GameInput::WEAPON_SLOT_2, recompinput::GameInput::WEAPON_SLOT_3,
            recompinput::GameInput::WEAPON_SLOT_4};
        constexpr std::array assign_inputs{recompinput::GameInput::ASSIGN_WEAPON_SLOT_1,
            recompinput::GameInput::ASSIGN_WEAPON_SLOT_2, recompinput::GameInput::ASSIGN_WEAPON_SLOT_3,
            recompinput::GameInput::ASSIGN_WEAPON_SLOT_4};
        for (unsigned slot = 0; slot < slot_inputs.size(); ++slot) {
            const bool pad = action_active(slot_inputs[slot], recompinput::InputDevice::Controller);
            const bool key = action_active(slot_inputs[slot], recompinput::InputDevice::Keyboard);
            if (recall[slot].sample(actions_enabled, pad, true, key, true))
                weapon_actions |= 1U << slot;
            if (assign[slot].sample(actions_enabled,
                action_active(assign_inputs[slot], recompinput::InputDevice::Controller), true,
                action_active(assign_inputs[slot], recompinput::InputDevice::Keyboard), true))
                weapon_actions |= 0x10U << slot;
        }

        jfc::modernization::sample_weapon_actions(weapon_actions);
        jfc::modernization::sample_actions(
            actions_enabled && (action_active(recompinput::GameInput::SPRINT)),
            quick_switch_active);
    }

    if (!result || !modern || recompinput::game_input_disabled()) {
        jfc::enhancements::sample_aim(controller, 0, 0, 0, 0);
        return result;
    }

    float aim_x = 0.0f;
    float aim_y = 0.0f;
        recompinput::get_right_analog(controller, &aim_x, &aim_y);
        aim_y = jfc::modern_input::from_sdl_vertical(aim_y);
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    if (controller == 0) {
        recompinput::get_mouse_deltas(&mouse_x, &mouse_y);
        mouse_y = jfc::modern_input::from_sdl_vertical(mouse_y);
    }
    jfc::enhancements::sample_aim(
        controller, aim_x, aim_y, mouse_x, mouse_y);
    return result;
}

void configure_controls() {
    using recompinput::GameInput;
    using recompinput::InputField;
    recompinput::set_game_input_name(GameInput::X_AXIS_NEG, "Move Left");
    recompinput::set_game_input_name(GameInput::X_AXIS_POS, "Move Right");
    recompinput::set_game_input_name(GameInput::Y_AXIS_POS, "Move Forward");
    recompinput::set_game_input_name(GameInput::Y_AXIS_NEG, "Move Back");
    recompinput::set_game_input_name(
        GameInput::A, "Next Weapon / Action / In-Game Select");
    recompinput::set_game_input_name(
        GameInput::B, "Previous Weapon / In-Game Back");
    recompinput::set_game_input_name(GameInput::Z, "Fire");
    recompinput::set_game_input_name(GameInput::R, "Aim");
    recompinput::set_game_input_name(GameInput::C_LEFT, "Sidestep Left");
    recompinput::set_game_input_name(GameInput::C_RIGHT, "Sidestep Right");
    recompinput::set_game_input_name(GameInput::C_UP, "Legacy C-Up");
    recompinput::set_game_input_name(GameInput::C_DOWN, "Legacy C-Down");
    recompinput::set_game_input_name(GameInput::JUMP, "Jump / Stand");
    recompinput::set_game_input_name(GameInput::CROUCH, "Crouch");
    recompinput::set_game_input_name(GameInput::SPRINT, "Sprint");
    recompinput::set_game_input_name(
        GameInput::QUICK_SWITCH, "Quick Character Switch");
    recompinput::set_game_input_name(GameInput::QUICK_GRENADE, "Quick Throwable");
    recompinput::set_game_input_description(
        GameInput::X_AXIS_NEG,
        "The left stick walks and strafes; the right stick turns and looks.");
    recompinput::set_game_input_description(
        GameInput::R,
        "The left trigger enters fine aim; the right stick aims while held.");
    recompinput::set_game_input_description(
        GameInput::A,
        "The original game's forward weapon, action, and select input. Default: L1 / LB / Wheel Down; physical A is Jump and menu accept.");
    recompinput::set_game_input_description(
        GameInput::B,
        "The original game's backward weapon and back input. Default: R1 / RB / Wheel Up; physical B is Crouch and menu back.");
    recompinput::set_game_input_description(
        GameInput::C_UP,
        "Original C-Up input for legacy controls. Modern Jump / Stand is bound separately.");
    recompinput::set_game_input_description(
        GameInput::C_DOWN,
        "Original C-Down input for legacy controls. Modern Crouch is bound separately.");
    recompinput::set_game_input_description(
        GameInput::JUMP,
        "Jump or stand during gameplay. Default controller binding: A.");
    recompinput::set_game_input_description(
        GameInput::CROUCH,
        "Crouch or crawl during gameplay. Default controller binding: B.");
    recompinput::set_game_input_description(GameInput::INTERACT,
        "Talk, open chests, activate devices and advance dialogue. Default: X / F. Does not cycle weapons; X can still be held to sprint.");
    recompinput::set_default_mapping_for_controller(GameInput::TOGGLE_VISUALS,
        {InputField::controller_digital(SDL_CONTROLLER_BUTTON_BACK)});
    recompinput::set_default_mapping_for_keyboard(GameInput::TOGGLE_VISUALS,
        {InputField::keyboard(SDL_SCANCODE_GRAVE)});
    recompinput::set_game_input_description(GameInput::TOGGLE_VISUALS,
        "Switch between original and enhanced textures. Default: View / Select or the grave key.");
    recompinput::set_default_mapping_for_controller(GameInput::INTERACT,
        {InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_WEST)});
    recompinput::set_default_mapping_for_keyboard(GameInput::INTERACT,
        {InputField::keyboard(SDL_SCANCODE_F)});
    recompinput::set_game_input_description(
        GameInput::SPRINT,
        "Hold X / Shift for grounded sprint when Sprint Mode is Hold. Also supports toggle mode.");
    recompinput::set_game_input_description(
        GameInput::QUICK_SWITCH,
        "Hold L4 and press B, click both sticks together, or press Ctrl+T to load the next unlocked character's saved location. Enable Quick Character Switching in QOL.");
    recompinput::set_game_input_description(GameInput::QUICK_SWITCH_MODIFIER,
        "Hold L4 / Ctrl before a weapon slot to assign it, or Y / G to assign the equipped throwable. L4+B / Ctrl+T changes character.");
    recompinput::set_game_input_description(GameInput::QUICK_GRENADE,
        "Use the assigned throwable and return to your weapon. Default: Y / G.");
    recompinput::set_game_input_description(GameInput::ASSIGN_THROWABLE,
        "Assign the equipped throwable. Guns and flares cannot be assigned. Default: hold L4 and press Y / Ctrl+G.");
    recompinput::set_default_mapping_for_keyboard(
        GameInput::Z, {InputField::mouse(SDL_BUTTON_LEFT)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::R, {InputField::mouse(SDL_BUTTON_RIGHT)});
    recompinput::set_default_mapping_for_controller(
        GameInput::Z,
        {InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)});
    recompinput::set_default_mapping_for_controller(
        GameInput::R,
        {InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERLEFT)});
    recompinput::set_default_mapping_for_controller(
        GameInput::A,
        {InputField::controller_digital(
             SDL_CONTROLLER_BUTTON_LEFTSHOULDER)});
    recompinput::set_default_mapping_for_controller(
        GameInput::B,
        {InputField::controller_digital(
             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)});
    recompinput::set_default_mapping_for_controller(
        GameInput::L,
        {InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSTICK)});
    recompinput::set_default_mapping_for_controller(
        GameInput::DPAD_DOWN,
        {InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSTICK)});
    for (auto input : {GameInput::DPAD_UP, GameInput::DPAD_RIGHT, GameInput::DPAD_LEFT})
        recompinput::set_default_mapping_for_controller(input, std::vector<InputField>{});
    recompinput::set_default_mapping_for_controller(
        GameInput::C_LEFT, std::vector<InputField>{});
    recompinput::set_default_mapping_for_controller(
        GameInput::C_RIGHT, std::vector<InputField>{});
    recompinput::set_default_mapping_for_controller(
        GameInput::C_UP, std::vector<InputField>{});
    recompinput::set_default_mapping_for_controller(
        GameInput::C_DOWN, std::vector<InputField>{});
    recompinput::set_default_mapping_for_controller(
        GameInput::JUMP,
        {InputField::controller_digital(
            recompinput::SDL_CONTROLLER_BUTTON_SOUTH)});
    recompinput::set_default_mapping_for_controller(
        GameInput::CROUCH,
        {InputField::controller_digital(
            recompinput::SDL_CONTROLLER_BUTTON_EAST)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::A, {InputField::keyboard(SDL_SCANCODE_E), InputField::mouse_wheel(false)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::B, {InputField::keyboard(SDL_SCANCODE_R), InputField::mouse_wheel(true)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::C_UP, std::vector<InputField>{});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::C_DOWN, std::vector<InputField>{});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::JUMP, {InputField::keyboard(SDL_SCANCODE_SPACE)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::CROUCH, {InputField::keyboard(SDL_SCANCODE_C)});
    recompinput::set_default_mapping_for_controller(
        GameInput::TOGGLE_MENU,
        {InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE1)});
    recompinput::set_default_mapping_for_controller(
        GameInput::ACCEPT_MENU,
        {InputField::controller_digital(
            recompinput::SDL_CONTROLLER_BUTTON_SOUTH)});
    recompinput::set_default_mapping_for_controller(
        GameInput::BACK_MENU,
        {InputField::controller_digital(
            recompinput::SDL_CONTROLLER_BUTTON_EAST)});
    recompinput::set_default_mapping_for_controller(
        GameInput::APPLY_MENU,
        {InputField::controller_digital(SDL_CONTROLLER_BUTTON_START)});
    recompinput::set_default_mapping_for_controller(
        GameInput::SPRINT,
        {InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_WEST)});
    recompinput::set_default_mapping_for_controller(
        GameInput::QUICK_SWITCH,
        {InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_EAST).holding(
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE2))});
    recompinput::set_default_mapping_for_controller(
        GameInput::QUICK_SWITCH_MODIFIER,
        {InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE2)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::SPRINT, {InputField::keyboard(SDL_SCANCODE_LSHIFT)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::QUICK_SWITCH, {InputField::keyboard(SDL_SCANCODE_T).holding(InputField::keyboard(SDL_SCANCODE_LCTRL)),
                                 InputField::keyboard(SDL_SCANCODE_T).holding(InputField::keyboard(SDL_SCANCODE_RCTRL))});
    recompinput::set_default_mapping_for_controller(GameInput::QUICK_SWITCH_STICKS,
        {InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSTICK).holding(
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSTICK)),
         InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSTICK).holding(
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSTICK))});
    recompinput::set_default_mapping_for_keyboard(GameInput::QUICK_SWITCH_STICKS, {});
    recompinput::set_game_input_disabled(GameInput::QUICK_SWITCH_MODIFIER, true);
    recompinput::set_default_mapping_for_keyboard(GameInput::QUICK_SWITCH_MODIFIER,
        {InputField::keyboard(SDL_SCANCODE_LCTRL), InputField::keyboard(SDL_SCANCODE_RCTRL)});
    recompinput::set_default_mapping_for_controller(GameInput::QUICK_GRENADE,
        {InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_NORTH)});
    recompinput::set_default_mapping_for_keyboard(GameInput::QUICK_GRENADE,
        {InputField::keyboard(SDL_SCANCODE_G)});
    recompinput::set_default_mapping_for_controller(GameInput::ASSIGN_THROWABLE,
        {InputField::controller_digital(recompinput::SDL_CONTROLLER_BUTTON_NORTH).holding(
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE2))});
    recompinput::set_default_mapping_for_keyboard(GameInput::ASSIGN_THROWABLE,
        {InputField::keyboard(SDL_SCANCODE_G).holding(InputField::keyboard(SDL_SCANCODE_LCTRL)),
         InputField::keyboard(SDL_SCANCODE_G).holding(InputField::keyboard(SDL_SCANCODE_RCTRL))});
    constexpr std::array slot_inputs{GameInput::WEAPON_SLOT_1, GameInput::WEAPON_SLOT_2,
        GameInput::WEAPON_SLOT_3, GameInput::WEAPON_SLOT_4};
    constexpr std::array slot_buttons{SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT};
    constexpr std::array slot_keys{SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4};
    constexpr std::array assign_inputs{GameInput::ASSIGN_WEAPON_SLOT_1, GameInput::ASSIGN_WEAPON_SLOT_2,
        GameInput::ASSIGN_WEAPON_SLOT_3, GameInput::ASSIGN_WEAPON_SLOT_4};
    for (unsigned slot = 0; slot < slot_inputs.size(); ++slot) {
        recompinput::set_default_mapping_for_controller(slot_inputs[slot],
            {InputField::controller_digital(slot_buttons[slot])});
        recompinput::set_default_mapping_for_keyboard(slot_inputs[slot], {InputField::keyboard(slot_keys[slot])});
        recompinput::set_default_mapping_for_controller(assign_inputs[slot],
            {InputField::controller_digital(slot_buttons[slot]).holding(InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE2))});
        recompinput::set_default_mapping_for_keyboard(assign_inputs[slot],
            {InputField::keyboard(slot_keys[slot]).holding(InputField::keyboard(SDL_SCANCODE_LCTRL)),
             InputField::keyboard(slot_keys[slot]).holding(InputField::keyboard(SDL_SCANCODE_RCTRL))});
        recompinput::set_game_input_description(slot_inputs[slot],
            "Select the assigned weapon. Assign Weapon Slot has its own binding below. Saved separately for each character.");
        recompinput::set_game_input_description(assign_inputs[slot], "Assign your equipped weapon to this shortcut.");
    }
    recompinput::set_default_mapping_for_keyboard(
        GameInput::SAVE_STATE, {InputField::keyboard(SDL_SCANCODE_F6)});
    recompinput::set_default_mapping_for_keyboard(
        GameInput::RESTORE_STATE, {InputField::keyboard(SDL_SCANCODE_F9)});
    recompinput::set_default_mapping_for_controller(
        GameInput::SAVE_STATE, {InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE4).holding(
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE2))});
    recompinput::set_default_mapping_for_controller(
        GameInput::RESTORE_STATE, {InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE3).holding(
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE2))});
    recompinput::set_game_input_description(GameInput::SAVE_STATE,
        "Save the complete game state to disk (hold L4 and press L5 / keyboard F6). Separate from ordinary game saves.");
    recompinput::set_game_input_description(GameInput::RESTORE_STATE,
        "Restore the manual state, including after restarting the game (hold L4 and press R5 / keyboard F9).");
}

void migrate_default_bindings(const std::filesystem::path& config_path) {
    using recompinput::GameInput;
    using recompinput::InputField;

    bool changed = false;
    const int controller_profile =
        recompinput::profiles::get_sp_controller_profile_index();
    const int keyboard_profile =
        recompinput::profiles::get_sp_keyboard_profile_index();
    const auto migrate_pair = [&](int profile, GameInput input,
                                  InputField from_primary,
                                  InputField from_secondary,
                                  InputField to_primary,
                                  InputField to_secondary) {
        if (profile < 0) {
            return;
        }
        InputField& primary =
            recompinput::profiles::get_input_binding(profile, input, 0);
        InputField& secondary =
            recompinput::profiles::get_input_binding(profile, input, 1);
        if (primary == from_primary && secondary == from_secondary) {
            primary = to_primary;
            secondary = to_secondary;
            changed = true;
        }
    };
    const auto migrate_single = [&](int profile, GameInput input,
                                    InputField from, InputField to) {
        migrate_pair(profile, input, from, {}, to, {});
    };
    const auto digital = [](SDL_GameControllerButton button) {
        return InputField::controller_digital(button);
    };
    const auto analog = [](SDL_GameControllerAxis axis, bool positive) {
        return InputField::controller_analog(axis, positive);
    };
    const InputField west = digital(recompinput::SDL_CONTROLLER_BUTTON_WEST);
    const InputField east = digital(recompinput::SDL_CONTROLLER_BUTTON_EAST);
    const InputField south = digital(recompinput::SDL_CONTROLLER_BUTTON_SOUTH);
    const InputField north = digital(recompinput::SDL_CONTROLLER_BUTTON_NORTH);
    const InputField start = digital(SDL_CONTROLLER_BUTTON_START);
    const InputField r4 = digital(SDL_CONTROLLER_BUTTON_PADDLE1);
    const InputField right_x_negative =
        analog(SDL_CONTROLLER_AXIS_RIGHTX, false);
    const InputField right_x_positive =
        analog(SDL_CONTROLLER_AXIS_RIGHTX, true);
    const InputField right_y_negative =
        analog(SDL_CONTROLLER_AXIS_RIGHTY, false);
    const InputField right_y_positive =
        analog(SDL_CONTROLLER_AXIS_RIGHTY, true);
    const InputField trigger_left =
        analog(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true);
    const InputField trigger_right =
        analog(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true);
    const InputField right_stick = digital(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    const InputField right_shoulder =
        digital(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    const InputField left_shoulder =
        digital(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

    migrate_pair(controller_profile, GameInput::A, south, west, west, {});
    migrate_pair(controller_profile, GameInput::A, south, {}, west, {});
    migrate_pair(controller_profile, GameInput::A, west, {}, west, {});
    migrate_pair(controller_profile, GameInput::B, east, north, north, {});
    migrate_pair(controller_profile, GameInput::B, west, {}, north, {});
    migrate_pair(controller_profile, GameInput::B, east, {}, north, {});
    migrate_pair(controller_profile, GameInput::B, north, {}, north, {});
    migrate_single(controller_profile, GameInput::ACCEPT_MENU, west, south);
    migrate_single(controller_profile, GameInput::BACK_MENU, north, east);
    migrate_single(controller_profile, GameInput::BACK_MENU, west, east);
    migrate_single(controller_profile, GameInput::Z, trigger_left, trigger_right);
    migrate_single(controller_profile, GameInput::R, trigger_right, trigger_left);
    migrate_pair(controller_profile, GameInput::C_LEFT, west, {}, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_LEFT,
        right_x_negative, north, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_LEFT,
        right_x_negative, {}, {}, {});
    migrate_pair(controller_profile, GameInput::C_RIGHT, north, {}, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_RIGHT,
        right_x_positive, east, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_RIGHT,
        right_x_positive, {}, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_UP,
        right_shoulder, {}, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_UP,
        right_y_negative, right_stick, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_UP,
        south, right_y_negative, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_UP,
        south, {}, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_DOWN,
        left_shoulder, {}, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_DOWN,
        right_y_positive, right_shoulder, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_DOWN,
        east, right_y_positive, {}, {});
    migrate_pair(
        controller_profile, GameInput::C_DOWN,
        east, {}, {}, {});
    migrate_pair(
        controller_profile, GameInput::APPLY_MENU,
        north, start, start, {});

    migrate_single(
        keyboard_profile, GameInput::A,
        InputField::keyboard(SDL_SCANCODE_SPACE),
        InputField::keyboard(SDL_SCANCODE_E));
    migrate_single(
        keyboard_profile, GameInput::B,
        InputField::keyboard(SDL_SCANCODE_LSHIFT),
        InputField::keyboard(SDL_SCANCODE_R));
    migrate_single(
        keyboard_profile, GameInput::C_UP,
        InputField::keyboard(SDL_SCANCODE_UP),
        {});
    migrate_single(
        keyboard_profile, GameInput::C_UP,
        InputField::keyboard(SDL_SCANCODE_SPACE),
        {});
    migrate_single(
        keyboard_profile, GameInput::C_DOWN,
        InputField::keyboard(SDL_SCANCODE_DOWN),
        {});
    migrate_single(
        keyboard_profile, GameInput::C_DOWN,
        InputField::keyboard(SDL_SCANCODE_LCTRL),
        {});

    if (controller_profile >= 0) {
        InputField& toggle_primary = recompinput::profiles::get_input_binding(
            controller_profile, GameInput::TOGGLE_MENU, 0);
        InputField& toggle_secondary = recompinput::profiles::get_input_binding(
            controller_profile, GameInput::TOGGLE_MENU, 1);
        const auto known_toggle = [&](const InputField& binding) {
            return binding.is_empty() || binding == r4 ||
                binding == digital(SDL_CONTROLLER_BUTTON_BACK) ||
                binding == digital(SDL_CONTROLLER_BUTTON_START) ||
                binding == east ||
                binding == digital(SDL_CONTROLLER_BUTTON_RIGHTSTICK) ||
                binding == digital(SDL_CONTROLLER_BUTTON_PADDLE2);
        };
        if (known_toggle(toggle_primary) && known_toggle(toggle_secondary) &&
            (toggle_primary != r4 || !toggle_secondary.is_empty())) {
            toggle_primary = r4;
            toggle_secondary = {};
            changed = true;
        }
    }

    const auto add_new_default = [&](int profile,
                                     recompinput::InputDevice device,
                                     GameInput input) {
        if (profile < 0) {
            return;
        }
        bool empty = true;
        for (size_t binding = 0;
             binding < recompinput::num_bindings_per_input; ++binding) {
            empty &= recompinput::profiles::get_input_binding(
                         profile, input, binding).is_empty();
        }
        if (empty) {
            recompinput::profiles::reset_input_binding(profile, device, input);
            changed = true;
        }
    };
    for (GameInput input : {GameInput::JUMP, GameInput::CROUCH,
                            GameInput::SPRINT, GameInput::QUICK_SWITCH}) {
        add_new_default(
            controller_profile, recompinput::InputDevice::Controller, input);
        add_new_default(
            keyboard_profile, recompinput::InputDevice::Keyboard, input);
    }

    migrate_single(controller_profile, GameInput::A, west, right_shoulder);
    migrate_single(controller_profile, GameInput::B, north, left_shoulder);
    migrate_single(controller_profile, GameInput::SPRINT,
        digital(SDL_CONTROLLER_BUTTON_LEFTSTICK), west);
    migrate_single(controller_profile, GameInput::QUICK_SWITCH,
        right_stick, digital(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    migrate_single(controller_profile, GameInput::L, left_shoulder, north);
    migrate_single(controller_profile, GameInput::DPAD_DOWN,
        digital(SDL_CONTROLLER_BUTTON_DPAD_DOWN), right_stick);
    if (changed && !recompinput::profiles::save_controls_config(
            config_path / "controls.json")) {
        throw std::runtime_error("Failed to persist controller binding migration");
    }
}

void migrate_held_bindings(const std::filesystem::path& config_path) {
    using recompinput::GameInput;
    using recompinput::InputField;
    for (int profile = 0; profile < recompinput::profiles::get_input_profile_count(); ++profile) {
        const auto get = [&](GameInput input) {
            jfc::BindingAlternatives result{};
            for (size_t i = 0; i < result.size(); ++i)
                result[i] = recompinput::profiles::get_input_binding(profile, input, i);
            return result;
        };
        const auto set = [&](GameInput input, const jfc::BindingAlternatives& fields) {
            for (size_t i = 0; i < fields.size(); ++i)
                recompinput::profiles::set_input_binding(profile, input, i, fields[i]);
        };
        const auto modifiers = get(GameInput::QUICK_SWITCH_MODIFIER);
        set(GameInput::QUICK_SWITCH, jfc::expand_held_bindings(get(GameInput::QUICK_SWITCH), modifiers));
        constexpr std::array sources{GameInput::WEAPON_SLOT_1, GameInput::WEAPON_SLOT_2,
            GameInput::WEAPON_SLOT_3, GameInput::WEAPON_SLOT_4, GameInput::QUICK_GRENADE};
        constexpr std::array destinations{GameInput::ASSIGN_WEAPON_SLOT_1, GameInput::ASSIGN_WEAPON_SLOT_2,
            GameInput::ASSIGN_WEAPON_SLOT_3, GameInput::ASSIGN_WEAPON_SLOT_4, GameInput::ASSIGN_THROWABLE};
        for (size_t i = 0; i < sources.size(); ++i)
            set(destinations[i], jfc::expand_held_bindings(get(sources[i]), modifiers));
        if (recompinput::profiles::get_input_profile_device(profile) == recompinput::InputDevice::Controller &&
            !recompinput::profiles::is_input_profile_custom(profile)) {
            for (auto action : {GameInput::SAVE_STATE, GameInput::RESTORE_STATE}) {
                auto fields = get(action);
                const auto paddle = action == GameInput::SAVE_STATE ? SDL_CONTROLLER_BUTTON_PADDLE4 : SDL_CONTROLLER_BUTTON_PADDLE3;
                if (fields[0] == InputField::controller_digital(paddle) &&
                    std::all_of(fields.begin() + 1, fields.end(), [](const auto& field) { return field.is_empty(); })) {
                    fields[0] = fields[0].holding(InputField::controller_digital(SDL_CONTROLLER_BUTTON_PADDLE2));
                    set(action, fields);
                }
            }
        }
    }
    if (!recompinput::profiles::save_controls_config(config_path / "controls.json"))
        throw std::runtime_error("Failed to persist held input binding migration");
}

void migrate_weapon_direction_bindings(const std::filesystem::path& config_path) {
    using recompinput::GameInput;
    using recompinput::InputField;
    bool changed = false;
    for (int profile = 0; profile < recompinput::profiles::get_input_profile_count(); ++profile) {
        if (recompinput::profiles::is_input_profile_custom(profile)) continue;
        auto& next = recompinput::profiles::get_input_binding(profile, GameInput::A, 0);
        auto& next_alt = recompinput::profiles::get_input_binding(profile, GameInput::A, 1);
        auto& previous = recompinput::profiles::get_input_binding(profile, GameInput::B, 0);
        auto& previous_alt = recompinput::profiles::get_input_binding(profile, GameInput::B, 1);
        const auto device = recompinput::profiles::get_input_profile_device(profile);
        changed |= jfc::migrate_weapon_binding_pair(device, next, next_alt, previous, previous_alt);
    }
    if (changed && !recompinput::profiles::save_controls_config(config_path / "controls.json")) {
        throw std::runtime_error("Failed to persist weapon direction binding migration");
    }
}

void migrate_shortcut_bindings(const std::filesystem::path& config_path) {
    using recompinput::GameInput;
    using recompinput::InputField;
    bool changed = false;
    for (int profile = 0; profile < recompinput::profiles::get_input_profile_count(); ++profile) {
        if (recompinput::profiles::is_input_profile_custom(profile)) continue;
        const auto device = recompinput::profiles::get_input_profile_device(profile);

        auto& switch_key = recompinput::profiles::get_input_binding(profile, GameInput::QUICK_SWITCH, 0);
        auto& switch_alt = recompinput::profiles::get_input_binding(profile, GameInput::QUICK_SWITCH, 1);
        auto& modifier = recompinput::profiles::get_input_binding(profile, GameInput::QUICK_SWITCH_MODIFIER, 0);
        auto& modifier_alt = recompinput::profiles::get_input_binding(profile, GameInput::QUICK_SWITCH_MODIFIER, 1);
        if (device == recompinput::InputDevice::Keyboard &&
            switch_key == InputField::keyboard(SDL_SCANCODE_Q) && switch_alt.is_empty() &&
            modifier.is_empty() && modifier_alt.is_empty()) {
            modifier = InputField::keyboard(SDL_SCANCODE_LCTRL);
            modifier_alt = InputField::keyboard(SDL_SCANCODE_RCTRL);
            changed = true;
        }
        for (auto input : {GameInput::QUICK_SWITCH, GameInput::CROUCH, GameInput::L,
                           GameInput::DPAD_UP, GameInput::DPAD_RIGHT, GameInput::DPAD_LEFT}) {
            auto& primary = recompinput::profiles::get_input_binding(profile, input, 0);
            auto& alternate = recompinput::profiles::get_input_binding(profile, input, 1);
            changed |= jfc::migrate_shortcut_binding(device, input, primary, alternate);
        }
    }
    if (changed && !recompinput::profiles::save_controls_config(config_path / "controls.json"))
        throw std::runtime_error("Failed to persist shortcut binding migration");
}

RspUcodeFunc* rsp_microcode(const OSTask* task) {
    if (task != nullptr && task->t.type == M_AUDTASK) {
        return jfg_audio;
    }
    std::fprintf(
        stderr,
        "Unknown non-graphics RSP task: %" PRIu32 "\n",
        task == nullptr ? 0U : task->t.type);
    return nullptr;
}

void on_vi() {
    RT64::PerformanceProfiler::recordRuntimeVI();
    if (!startup_game.empty()) {

        recompui::hide_all_contexts();
        recomp::start_game(startup_game, {});
        startup_game.clear();
    }

    const uint64_t current_vi = ultramodern::is_game_initialized() ? ++vi_number : 0;
    jfc::crash_reporter::set_vi(current_vi);
    recompinput::update_rumble();
}

std::string game_thread_name(const OSThread* thread) {
    return thread == nullptr ? "JFG" : "JFG " + std::to_string(thread->id);
}

const char* rom_error(recomp::RomValidationError error) {
    switch (error) {
    case recomp::RomValidationError::Good:
        return "none";
    case recomp::RomValidationError::FailedToOpen:
        return "the file could not be opened";
    case recomp::RomValidationError::NotARom:
        return "the selected file is not an N64 ROM";
    case recomp::RomValidationError::IncorrectRomSize:
        return "the ROM must be exactly 33,554,432 bytes; it may be truncated or modified";
    case recomp::RomValidationError::IncorrectByteOrder:
        return "the ROM byte order is unsupported; use a valid .z64, .v64 or .n64 image";
    case recomp::RomValidationError::IncorrectRom:
        return "the ROM is not Jet Force Gemini";
    case recomp::RomValidationError::IncorrectVersion:
        return "the ROM content does not match the supported US revision 0 image; modified images are unsupported";
    case recomp::RomValidationError::NotYet:
        return "this ROM revision is not supported yet";
    case recomp::RomValidationError::OtherError:
        return "the ROM could not be validated";
    }
    return "the ROM could not be validated";
}

void load_enhancement_config(
    recomp::config::Config& general,
    recomp::config::Config& graphics,
    recomp::config::Config& qol
) {
    jfc::enhancements::set_modern_controls(
        std::get<bool>(general.get_option_value(modern_controls_option)));
    jfc::enhancements::set_mouse_acceleration(
        std::get<bool>(general.get_option_value(mouse_acceleration_option)));
    load_stick_inversions(general);
    jfc::enhancements::set_joystick_sensitivity(
        static_cast<int>(std::get<double>(general.get_option_value(
            joystick_sensitivity_option))),
        static_cast<int>(std::get<double>(general.get_option_value(
            joystick_sensitivity_x_option))),
        static_cast<int>(std::get<double>(general.get_option_value(
            joystick_sensitivity_y_option))));
    jfc::enhancements::set_draw_distance_percent(static_cast<int>(std::get<double>(
        graphics.get_option_value(draw_distance_option))));
    jfc::enhancements::set_fog_strength_percent(static_cast<int>(std::get<double>(
        graphics.get_option_value(fog_strength_option))));
}

void require_config_options(
    const recomp::config::Config& config,
    std::initializer_list<std::string> option_ids
) {
    for (const std::string& option_id : option_ids) {
        if (!config.has_option(option_id)) {
            throw std::runtime_error(
                "Configuration tab '" + config.id +
                "' is missing required option '" + option_id + "'");
        }
    }
}

void validate_config_schema(
    const recomp::config::Config& general,
    const recomp::config::Config& graphics,
    const recomp::config::Config& qol
) {
    require_config_options(general, {
        modern_controls_option,
        mouse_acceleration_option,
        recompui::config::general::options::mouse_sensitivity,
        joystick_sensitivity_option,
        joystick_sensitivity_x_option,
        joystick_sensitivity_y_option,
        invert_left_stick_x_option, invert_left_stick_y_option, invert_right_stick_x_option,
        invert_right_stick_y_option,
        modernization_schema_option,
    });
    require_config_options(graphics, {
        recompui::config::graphics::options::lighting_option,
        recompui::config::graphics::options::texture_option,
        draw_distance_option,
        fog_strength_option,
        full_hud_migration_option,
    });
    require_config_options(qol, {
        cutscene_skipping_option,
        invulnerable_tribals_option,
        persistent_tribal_rescues_option,
        final_tribal_requirement_option,
        skip_seen_landings_option,
        quick_character_switching_option,
        skip_seen_character_travel_option,
        sprint_mode_option,
        double_pistol_recharge_option,
        double_ammo_pickups_option,
        unlimited_lives_option,
        mission_complete_refill_option,
        double_enemy_drops_option,
        double_robot_mission_time_option,
        hide_companion_option,
        half_boss_health_option,
        boss_health_hud_option,
        boss_weak_point_effect_option,
        mantis_final_vulnerability_option,
        boss_pistol_damage_option,
        boss_phase_ammo_option,
        fast_throws_option,
        halo_health_option,
        persistent_dropped_pickups_option,
        persistent_corpses_option,
        shotgun_upgrade_option,
        flamethrower_upgrade_option,
        shocker_upgrade_option,
        ship_part_scanner_option,
        guidance_text_option,
        modern_health_option,
        room_counters_option,
        area_pickups_option,
        weapon_shortcuts_hud_option,
    });

}

void migrate_config_schema(
    recomp::config::Config& general,
    recomp::config::Config& graphics,
    recomp::config::Config& qol
) {
    const int stored_schema = static_cast<int>(std::get<double>(
        general.get_option_value(modernization_schema_option)));
    if (stored_schema > JFC_CONFIG_SCHEMA_VERSION) {
        throw std::runtime_error(
            "Configuration schema " + std::to_string(stored_schema) +
            " is newer than this build supports (" +
            std::to_string(JFC_CONFIG_SCHEMA_VERSION) + ")");
    }
    if (stored_schema == JFC_CONFIG_SCHEMA_VERSION) {
        return;
    }

    if (stored_schema < 4 && std::get<double>(general.get_option_value(
            recompui::config::general::options::mouse_sensitivity)) == 0.0) {
        general.set_option_value(
            recompui::config::general::options::mouse_sensitivity,
            100.0);
    }

    general.set_option_value(
        modernization_schema_option,
        static_cast<double>(JFC_CONFIG_SCHEMA_VERSION));
    if (!graphics.save_config() || !qol.save_config() || !general.save_config()) {
        throw std::runtime_error(
            "Failed to persist the modernisation configuration migration");
    }

}

int run_application(
    int argc,
    char** argv,
    const std::filesystem::path& command_line_rom
) {
    recompui::programconfig::set_program_name(program_title);
    recompui::programconfig::set_program_id(u8"JetForceCapricorn");
    jfc::modernization::set_settings_open_request([] {
        recompinput::request_config_open("pause_options");
    });
    const std::string settings_build_label = make_settings_build_label();
    recompui::programconfig::set_build_label(settings_build_label);
    recompui::programconfig::set_build_details(settings_build_details);

    const std::filesystem::path config_path =
        recompui::file::get_app_folder_path();
    std::error_code error;
    std::filesystem::create_directories(config_path, error);
    if (error) {
        std::fprintf(stderr, "Failed to create config directory: %s\n",
                     error.message().c_str());
        return EXIT_FAILURE;
    }
    jfc::crash_reporter::install(config_path);
    recomp::overlays::register_function_lookup_failure_callback(
        &jfc::crash_reporter::set_function_lookup_failure);
    jfc::modernization::set_save_metadata_error_reporter([](const char* message) {
        recompui::queue_ui_action([text = std::string(message)] {
            recompui::message_box(text.c_str());
        });
    });
    if (!jfc::modernization::initialize_save_metadata(config_path)) {
        return EXIT_FAILURE;
    }
    recomp::register_config_path(config_path);
    jfc::state::initialize(config_path);
    recompui::register_primary_font("LatoLatin-Regular.ttf", "LatoLatin");
    recompui::register_extra_font("LatoLatin-Bold.ttf");

    jfc::textures::Launcher texture_launcher(config_path);
    recomp::config::Config& graphics = recompui::config::create_graphics_tab(
        recompui::config::graphics::tab_name,
        [&texture_launcher](recompui::ContextId context, recompui::Element* parent) {
            texture_launcher.init_graphics(context, parent);
        }, jfc::camera_settings::create_resets);
    jfc::camera_settings::add_options(graphics);
    graphics.add_number_option(
        draw_distance_option,
        "Draw Distance",
        "Scales the game's camera far plane.",
        100, 400, 25, 0, true, 100);
    graphics.add_number_option(
        fog_strength_option,
        "Fog Strength",
        "Blends environmental fog from disabled to the original level value.",
        0, 100, 5, 0, true, 100);
    graphics.add_bool_option(
        full_hud_migration_option,
        "",
        "",
        false,
        true);
    recomp::config::Config& qol = recompui::config::create_config_tab(
        "QOL", qol_config_id, false);
    qol.add_bool_option(modern_health_option,
        "Modern Health Display",
        "Shows full reserve containers separately from the current container's fill. Health, damage and upgrades are unchanged.",
        false);
    qol.add_bool_option(room_counters_option,
        "Room Collectible Counters",
        "Shows Mizar tokens and Tribals still missing in this room. Lost Tribals remain missing until rescued; they do not mark a room cleared.",
        true);
    qol.add_bool_option(area_pickups_option,
        "Area Collectible Tracker",
        "Shows original icons for keys, spaceship parts and Floyd's robot components remaining in this area, including chests and event rewards. Collected categories disappear.",
        true);
    qol.add_bool_option(weapon_shortcuts_hud_option,
        "Weapon Shortcut HUD",
        "Shows assigned weapons at the top right: a D-pad for controllers, or four numbered key slots for keyboard controls.",
        true);
    qol.add_bool_option(
        cutscene_skipping_option,
        "Cutscene Skipping",
        "Allows Space, Enter, or Start to skip an active cutscene through the game's normal cleanup path.",
        true);
    qol.add_bool_option(
        invulnerable_tribals_option,
        "Invulnerable Tribals",
        "Prevents exact Tribal actors from crossing their shared lethal-health boundary.",
        false);
    qol.add_bool_option(unlimited_lives_option,
        "Unlimited Lives",
        "Always restarts after death as if an extra life were available. Continues are not consumed or displayed. Off restores the original rules.",
        true);
    qol.add_bool_option(
        persistent_tribal_rescues_option,
        "Persistent Tribal Rescues",
        "Keeps individually rescued Tribals rescued across level revisits in this save slot.",
        true);
    qol.add_enum_option(
        final_tribal_requirement_option,
        "Final Tribal Requirement",
        "Changes only the rescued-Tribal threshold for final access; the true 100% flag is unchanged.",
        {
            {jfc::modernization::FinalTribalRequirement::Original,
             "original", "Original"},
            {jfc::modernization::FinalTribalRequirement::Percent70,
             "70_percent", "70%"},
            {jfc::modernization::FinalTribalRequirement::Percent50,
             "50_percent", "50%"},
            {jfc::modernization::FinalTribalRequirement::Percent25,
             "25_percent", "25%"},
        },
        jfc::modernization::FinalTribalRequirement::Original);
    qol.add_bool_option(
        skip_seen_landings_option,
        "Skip Seen Ship Landings",
        "Skips a ship landing only after that exact landing has completed in this save slot.",
        false);
    qol.add_bool_option(
        quick_character_switching_option,
        "Quick Character Switching",
        "Uses the game's normal level transition to load the next unlocked character's saved location.",
        false);
    qol.add_bool_option(
        skip_seen_character_travel_option,
        "Skip Seen Character Travel",
        "Skips repeat character-selection travel independently of ship landings.",
        false);
    qol.add_enum_option(
        sprint_mode_option,
        "Sprint Mode",
        "Hold X / Shift to sprint at up to 1.5x grounded movement speed. Off preserves normal movement; Toggle is also available.",
        {
            {jfc::modernization::SprintMode::Off, "off", "Off"},
            {jfc::modernization::SprintMode::Hold, "hold", "Hold"},
            {jfc::modernization::SprintMode::Toggle, "toggle", "Toggle"},
        },
        jfc::modernization::SprintMode::Off);
    qol.add_bool_option(
        double_pistol_recharge_option,
        "Double Pistol Recharge",
        "Recharges the default pistol twice as fast. Firing rate and other weapons are unchanged.",
        false);
    qol.add_bool_option(
        double_ammo_pickups_option,
        "Double Ammo Pickups",
        "Doubles ammo from small, large, enemy-drop and already-owned weapon pickups, up to normal ammo capacity. Capacity upgrades are unchanged.",
        false);
    qol.add_bool_option(
        persistent_dropped_pickups_option,
        "Persistent Dropped Pickups",
        "Dropped health, ammo and collectible heads no longer expire while on the map. Collection and map cleanup still remove them; ordinary debris still fades.",
        false);
    qol.add_bool_option(shotgun_upgrade_option,
        "Upgraded Shotgun",
        "Fires six fully charged energy spheres with a wider spread on each press. Uses one round per blast.",
        true);
    qol.add_bool_option(flamethrower_upgrade_option,
        "Upgraded Flamethrower",
        "Uses half as much fuel, deals 50% more burning damage and staggers enemies whenever their native hurt reaction is available.",
        true);
    qol.add_bool_option(shocker_upgrade_option,
        "Upgraded Shocker",
        "Extends beam range fourfold, arcs through up to three adjacent enemies and raises the ammo reserve to at least 100. Earned capacity upgrades are preserved.",
        true);
    qol.add_bool_option(persistent_corpses_option,
        "Persistent Corpses",
        "Keeps corpses visible until the area is unloaded. Turning this off resumes fading. Native memory cleanup can still remove bodies when needed.",
        false);
    qol.add_bool_option(mission_complete_refill_option,
        "Mission Complete Refill",
        "Restores health and all owned weapon ammo to current capacity when you dismiss the mission-complete screen. Ordinary doors and deaths are unchanged.",
        true);
    qol.add_bool_option(double_enemy_drops_option,
        "Double Enemy Drop Chance",
        "Doubles random enemy weapon/ammo drop chance from one in three to two in three. Guaranteed rewards, item amounts and non-dropping enemies are unchanged.",
        true);
    qol.add_bool_option(fast_throws_option,
        "Fast Throws",
        "Completes grenade and other throwable animations in half the time, including release and recovery. Projectile speed, fuse and charge time are unchanged.",
        true);
    qol.add_bool_option(double_robot_mission_time_option,
        "Double Robot Mission Time",
        "Doubles the time limit when starting or retrying a Floyd mission. Flight speed and medal times are unchanged; an active mission keeps its current limit.",
        false);
    qol.add_bool_option(hide_companion_option,
        "Hide Floyd Companion",
        "Hides the following robot, its shadow and companion crosshair. Floyd missions and story scenes remain visible; gameplay and saves are unchanged.",
        false);
    qol.add_bool_option(half_boss_health_option,
        "Half Boss Health",
        "Bosses and their weak points start with half their normal health. Applies to new fights and new boss phases; existing saved health, ordinary enemies, weapon damage and vulnerability windows are unchanged.",
        false);
    qol.add_bool_option(boss_health_hud_option,
        "Boss Target Health",
        "Shows the aimed-at boss body part's remaining health and whether it is vulnerable, protected or destroyed. The bar is relative to original full health, so half-health parts start half full. Different weapons deal different damage.",
        false);
    qol.add_bool_option(boss_pistol_damage_option,
        "Double Pistol Boss Damage",
        "Player pistol shots deal twice their normal damage to bosses. Other weapons, ordinary enemies and protected weak points are unchanged.",
        true);
    qol.add_bool_option(boss_phase_ammo_option,
        "Boss Phase Ammo Refill",
        "Restores 25% of each owned weapon's capacity when a boss phase or weak point is defeated, rounded up and capped at capacity. Opening a damage window does not refill ammo.",
        true);
    qol.add_bool_option(boss_weak_point_effect_option,
        "Boss Weak-point Halos",
        "Marks damageable boss regions with a pulsing green halo. It follows the moving weak point and disappears while protected or destroyed; normal lighting and hit flashes are unchanged.",
        true);
    qol.add_bool_option(mantis_final_vulnerability_option,
        "Open Mechantis Final Phase",
        "After each mantis loses all its weapons, its final head/body weak point can take damage throughout movement and attacks. Earlier phases, attack animations and the defeat sequence are unchanged.",
        true);
    qol.add_bool_option(halo_health_option,
        "Halo-style Health",
        "After 30 gameplay seconds without damage, restores missing health in 5-second steps over the next 60 seconds. Damage restarts the delay. Pauses and cutscenes stop the timer; respawning or changing areas/characters starts a fresh delay.",
        false);
    qol.add_enum_option(
        ship_part_scanner_option,
        "Ship Part Scanner",
        "Guides you toward missing ship parts. Off disables the scanner; Audio keeps pings without visual hints. Visual modes also require Show Guidance Text.",
        {
            {jfc::modernization::ShipPartScanner::Off, "off", "Off"},
            {jfc::modernization::ShipPartScanner::Audio, "audio", "Audio"},
            {jfc::modernization::ShipPartScanner::Visual, "visual", "Visual"},
            {jfc::modernization::ShipPartScanner::AudioVisual,
             "audio_visual", "Audio + Visual"},
        },
        jfc::modernization::ShipPartScanner::Off);
    qol.add_bool_option(guidance_text_option,
        "Show Guidance Text",
        "Shows bottom-left SCAN ship-part hints and final Tribal-requirement guidance. Off hides this text entirely without changing scanner audio or game progression.",
        false);
    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_mouse_sensitivity = true;
    recomp::config::Config& general =
        recompui::config::create_general_tab(general_options, "Controls");
    general.add_bool_option(
        modern_controls_option,
        "Modern Twin-Stick Controls",
        "Left stick moves and strafes; right stick or mouse turns and looks.",
        true);
    general.add_bool_option(
        mouse_acceleration_option,
        "Mouse Acceleration",
        "Increases aim speed with faster mouse movement. Disable for linear raw input.",
        false);
    general.add_number_option(
        joystick_sensitivity_option,
        "Right Stick Sensitivity",
        "Adjusts both right-stick axes.",
        25, 100, 5, 0, true, 100);
    general.add_number_option(
        joystick_sensitivity_x_option,
        "Right Stick Horizontal Sensitivity",
        "Fine-tunes horizontal right-stick movement after the general setting.",
        50, 150, 5, 0, true, 100);
    general.add_number_option(
        joystick_sensitivity_y_option,
        "Right Stick Vertical Sensitivity",
        "Fine-tunes vertical right-stick movement after the general setting.",
        50, 150, 5, 0, true, 100);
    constexpr std::array inversion_labels{"Invert Left Stick X Axis", "Invert Left Stick Y Axis",
        "Invert Right Stick X Axis", "Invert Right Stick Y Axis"};
    for (size_t i = 0; i < stick_inversion_options.size(); ++i)
        general.add_bool_option(stick_inversion_options[i], inversion_labels[i],
            "Reverses this joystick axis during gameplay. Mouse, keyboard and menu navigation are unchanged.", false);
    general.add_number_option(
        modernization_schema_option,
        "",
        "",
        0, JFC_CONFIG_SCHEMA_VERSION, 1, 0, false, 0, true);
    configure_controls();
    recompui::config::create_controls_tab("Bindings");
    jfc::audio_settings::create();
    recompui::config::finalize();
    auto& loaded_general = recompui::config::get_general_config();
    auto& loaded_graphics = recompui::config::get_graphics_config();
    auto& loaded_qol = recompui::config::get_config(qol_config_id);
    validate_config_schema(loaded_general, loaded_graphics, loaded_qol);
    const int stored_schema = static_cast<int>(std::get<double>(
        loaded_general.get_option_value(modernization_schema_option)));
    if (stored_schema < 5) {

        migrate_default_bindings(config_path);
    }
    if (stored_schema < 6) {
        migrate_weapon_direction_bindings(config_path);
    }
    if (stored_schema < 7) {
        migrate_shortcut_bindings(config_path);
    }
    if (stored_schema < 8) {
        bool changed = false;
        for (int profile = 0; profile < recompinput::profiles::get_input_profile_count(); ++profile) {
            if (recompinput::profiles::is_input_profile_custom(profile)) continue;
            changed |= jfc::migrate_throwable_binding(recompinput::profiles::get_input_profile_device(profile),
                recompinput::profiles::get_input_binding(profile, recompinput::GameInput::QUICK_SWITCH, 0),
                recompinput::profiles::get_input_binding(profile, recompinput::GameInput::QUICK_SWITCH, 1));
        }
        if (changed && !recompinput::profiles::save_controls_config(config_path / "controls.json"))
            throw std::runtime_error("Failed to persist character chord migration");
    }
    if (stored_schema < 9) migrate_held_bindings(config_path);
    migrate_config_schema(loaded_general, loaded_graphics, loaded_qol);
    if (!std::get<bool>(loaded_graphics.get_option_value(
            full_hud_migration_option))) {
        if (std::get<uint32_t>(loaded_graphics.get_option_value(
                recompui::config::graphics::options::hr_option)) ==
                static_cast<uint32_t>(
                    ultramodern::renderer::HUDRatioMode::Clamp16x9)) {
            loaded_graphics.set_option_value(
                recompui::config::graphics::options::hr_option,
                static_cast<uint32_t>(
                    ultramodern::renderer::HUDRatioMode::Full));
        }
        loaded_graphics.set_option_value(full_hud_migration_option, true);
        loaded_graphics.save_config();
    }
    load_enhancement_config(loaded_general, loaded_graphics, loaded_qol);
    loaded_general.add_option_change_callback(
        modern_controls_option,
        [](auto value, auto, auto) {
            jfc::enhancements::set_modern_controls(std::get<bool>(value));
        });
    loaded_general.add_option_change_callback(
        mouse_acceleration_option,
        [](auto value, auto, auto) {
            jfc::enhancements::set_mouse_acceleration(std::get<bool>(value));
        });
    for (auto option : stick_inversion_options)
        loaded_general.add_option_change_callback(option, [&loaded_general](auto, auto, auto) {
            load_stick_inversions(loaded_general);
        });
    const auto update_joystick_sensitivity = [&loaded_general](auto, auto, auto) {
        jfc::enhancements::set_joystick_sensitivity(
            static_cast<int>(std::get<double>(loaded_general.get_option_value(
                joystick_sensitivity_option))),
            static_cast<int>(std::get<double>(loaded_general.get_option_value(
                joystick_sensitivity_x_option))),
            static_cast<int>(std::get<double>(loaded_general.get_option_value(
                joystick_sensitivity_y_option))));
    };
    loaded_general.add_option_change_callback(
        joystick_sensitivity_option, update_joystick_sensitivity);
    loaded_general.add_option_change_callback(
        joystick_sensitivity_x_option, update_joystick_sensitivity);
    loaded_general.add_option_change_callback(
        joystick_sensitivity_y_option, update_joystick_sensitivity);
    loaded_graphics.add_option_change_callback(
        draw_distance_option,
        [](auto value, auto, auto) {
            jfc::enhancements::set_draw_distance_percent(
                static_cast<int>(std::get<double>(value)));
        });
    loaded_graphics.add_option_change_callback(
        fog_strength_option,
        [](auto value, auto, auto) {
            jfc::enhancements::set_fog_strength_percent(
                static_cast<int>(std::get<double>(value)));
        });
    loaded_qol.add_option_change_callback(
        cutscene_skipping_option,
        [](auto value, auto, auto) {
            jfc::enhancements::set_cutscene_skipping(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(
        invulnerable_tribals_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_invulnerable_tribals(
                std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(
        persistent_tribal_rescues_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_persistent_tribal_rescues(
                std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(
        final_tribal_requirement_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_final_tribal_requirement(
                static_cast<jfc::modernization::FinalTribalRequirement>(
                    std::get<uint32_t>(value)));
        });
    loaded_qol.add_option_change_callback(
        skip_seen_landings_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_skip_seen_landings(
                std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(
        quick_character_switching_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_quick_character_switching(
                std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(
        skip_seen_character_travel_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_skip_seen_character_travel(
                std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(
        sprint_mode_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_sprint_mode(
                static_cast<jfc::modernization::SprintMode>(
                    std::get<uint32_t>(value)));
        });
    loaded_qol.add_option_change_callback(
        ship_part_scanner_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_ship_part_scanner(
                static_cast<jfc::modernization::ShipPartScanner>(
                    std::get<uint32_t>(value)));
        });
    loaded_qol.add_option_change_callback(guidance_text_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_guidance_text(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(double_pistol_recharge_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_double_pistol_recharge(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(double_ammo_pickups_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_double_ammo_pickups(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(unlimited_lives_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_unlimited_lives(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(mission_complete_refill_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_mission_complete_refill(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(double_enemy_drops_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_double_enemy_drops(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(fast_throws_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_fast_throws(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(double_robot_mission_time_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_double_robot_mission_time(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(hide_companion_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_hide_companion(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(half_boss_health_option,
        [](auto value, auto, auto) {
            jfc::boss::set_half_health(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(boss_health_hud_option,
        [](auto value, auto, auto) {
            jfc::boss::set_target_hud(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(boss_weak_point_effect_option,
        [](auto value, auto, auto) {
            jfc::boss::set_weak_point_effect(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(mantis_final_vulnerability_option,
        [](auto value, auto, auto) {
            jfc::boss::set_mantis_final_vulnerability(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(halo_health_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_halo_health(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(boss_pistol_damage_option,
        [](auto value, auto, auto) {
            jfc::boss::set_double_pistol_damage(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(boss_phase_ammo_option,
        [](auto value, auto, auto) {
            jfc::boss::set_phase_ammo_refill(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(persistent_dropped_pickups_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_persistent_dropped_pickups(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(shotgun_upgrade_option,
        [](auto value, auto, auto) {
            jfc::weapons::set_shotgun_upgrade(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(flamethrower_upgrade_option,
        [](auto value, auto, auto) {
            jfc::weapons::set_flamethrower_upgrade(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(shocker_upgrade_option,
        [](auto value, auto, auto) {
            jfc::weapons::set_shocker_upgrade(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(persistent_corpses_option,
        [](auto value, auto, auto) {
            jfc::modernization::set_persistent_corpses(std::get<bool>(value));
        });
    loaded_qol.add_option_change_callback(modern_health_option,
        [](auto value, auto, auto) { jfc::hud::set_modern_health(std::get<bool>(value)); });
    loaded_qol.add_option_change_callback(room_counters_option,
        [](auto value, auto, auto) { jfc::hud::set_room_counters(std::get<bool>(value)); });
    jfc::hud::set_modern_health(std::get<bool>(loaded_qol.get_option_value(modern_health_option)));
    jfc::hud::set_room_counters(std::get<bool>(loaded_qol.get_option_value(room_counters_option)));
    loaded_qol.add_option_change_callback(area_pickups_option,
        [](auto value, auto, auto) { jfc::hud::set_area_pickups(std::get<bool>(value)); });
    jfc::hud::set_area_pickups(std::get<bool>(loaded_qol.get_option_value(area_pickups_option)));
    loaded_qol.add_option_change_callback(weapon_shortcuts_hud_option,
        [](auto value, auto, auto) { jfc::hud::set_weapon_shortcuts(std::get<bool>(value)); });
    jfc::hud::set_weapon_shortcuts(std::get<bool>(loaded_qol.get_option_value(weapon_shortcuts_hud_option)));
    jfc::modernization::set_double_pistol_recharge(std::get<bool>(
        loaded_qol.get_option_value(double_pistol_recharge_option)));
    jfc::modernization::set_double_ammo_pickups(std::get<bool>(
        loaded_qol.get_option_value(double_ammo_pickups_option)));
    jfc::modernization::set_unlimited_lives(std::get<bool>(
        loaded_qol.get_option_value(unlimited_lives_option)));
    jfc::modernization::set_mission_complete_refill(std::get<bool>(
        loaded_qol.get_option_value(mission_complete_refill_option)));
    jfc::modernization::set_double_enemy_drops(std::get<bool>(
        loaded_qol.get_option_value(double_enemy_drops_option)));
    jfc::modernization::set_fast_throws(std::get<bool>(
        loaded_qol.get_option_value(fast_throws_option)));
    jfc::modernization::set_double_robot_mission_time(std::get<bool>(
        loaded_qol.get_option_value(double_robot_mission_time_option)));
    jfc::modernization::set_hide_companion(std::get<bool>(
        loaded_qol.get_option_value(hide_companion_option)));
    jfc::boss::set_half_health(std::get<bool>(loaded_qol.get_option_value(half_boss_health_option)));
    jfc::boss::set_target_hud(std::get<bool>(loaded_qol.get_option_value(boss_health_hud_option)));
    jfc::boss::set_weak_point_effect(std::get<bool>(loaded_qol.get_option_value(boss_weak_point_effect_option)));
    jfc::boss::set_mantis_final_vulnerability(std::get<bool>(loaded_qol.get_option_value(mantis_final_vulnerability_option)));
    jfc::boss::set_double_pistol_damage(std::get<bool>(loaded_qol.get_option_value(boss_pistol_damage_option)));
    jfc::boss::set_phase_ammo_refill(std::get<bool>(loaded_qol.get_option_value(boss_phase_ammo_option)));
    jfc::modernization::set_halo_health(std::get<bool>(
        loaded_qol.get_option_value(halo_health_option)));
    jfc::modernization::set_persistent_dropped_pickups(std::get<bool>(
        loaded_qol.get_option_value(persistent_dropped_pickups_option)));
    jfc::weapons::set_shotgun_upgrade(std::get<bool>(
        loaded_qol.get_option_value(shotgun_upgrade_option)));
    jfc::weapons::set_flamethrower_upgrade(std::get<bool>(
        loaded_qol.get_option_value(flamethrower_upgrade_option)));
    jfc::weapons::set_shocker_upgrade(std::get<bool>(
        loaded_qol.get_option_value(shocker_upgrade_option)));
    jfc::modernization::set_persistent_corpses(std::get<bool>(
        loaded_qol.get_option_value(persistent_corpses_option)));
    jfc::enhancements::set_cutscene_skipping(
        std::get<bool>(loaded_qol.get_option_value(cutscene_skipping_option)));
    jfc::modernization::set_invulnerable_tribals(
        std::get<bool>(loaded_qol.get_option_value(invulnerable_tribals_option)));
    jfc::modernization::set_persistent_tribal_rescues(
        std::get<bool>(loaded_qol.get_option_value(
            persistent_tribal_rescues_option)));
    jfc::modernization::set_final_tribal_requirement(
        static_cast<jfc::modernization::FinalTribalRequirement>(
            std::get<uint32_t>(loaded_qol.get_option_value(
                final_tribal_requirement_option))));
    jfc::modernization::set_skip_seen_landings(
        std::get<bool>(loaded_qol.get_option_value(skip_seen_landings_option)));
    jfc::modernization::set_quick_character_switching(
        std::get<bool>(loaded_qol.get_option_value(
            quick_character_switching_option)));
    jfc::modernization::set_skip_seen_character_travel(
        std::get<bool>(loaded_qol.get_option_value(
            skip_seen_character_travel_option)));
    jfc::modernization::set_sprint_mode(
        static_cast<jfc::modernization::SprintMode>(
            std::get<uint32_t>(loaded_qol.get_option_value(
                sprint_mode_option))));
    jfc::modernization::set_ship_part_scanner(
        static_cast<jfc::modernization::ShipPartScanner>(
            std::get<uint32_t>(loaded_qol.get_option_value(
                ship_part_scanner_option))));
    jfc::modernization::set_guidance_text(std::get<bool>(
        loaded_qol.get_option_value(guidance_text_option)));

    recompinput::players::set_single_player_mode(true);
    recompui::register_ui_exports();

    for (const recomp::GameEntry& game : supported_games) {
        if (!recomp::register_game(game)) {
            std::fprintf(stderr, "Failed to register Jet Force Gemini\n");
            return EXIT_FAILURE;
        }
    }
    if (argc > 2) {
        std::fprintf(stderr, "Usage: %s [path-to-supported-rom]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        std::u8string selected_game(game_id);
        const auto result =
            recomp::select_rom(command_line_rom, selected_game);
        if (result != recomp::RomValidationError::Good) {
            std::fprintf(stderr, "ROM validation failed: %s\n", rom_error(result));
            return EXIT_FAILURE;
        }
        startup_game = std::move(selected_game);
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 || !initialize_audio()) {
        std::fprintf(stderr, "Audio disabled: %s\n", SDL_GetError());
    }

    const recomp::rsp::callbacks_t rsp_callbacks{rsp_microcode};
    const ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context =
            [](uint8_t* rdram,
               ultramodern::renderer::WindowHandle handle,
               bool developer_mode) {
                auto renderer = recompui::renderer::create_render_context(
                    rdram,
                    handle,

                    ultramodern::renderer::PresentationMode::PresentEarly,
                    developer_mode);
                if (renderer && renderer->valid()) {
                    renderer->enable_cpu_framebuffer_completion();
                }
                return renderer;
            },
    };
    const ultramodern::audio_callbacks_t audio_callbacks{
        queue_samples, audio_queue_state, set_frequency, set_audio_activity, shutdown_audio};
    const ultramodern::input::callbacks_t input_callbacks{
        [] { recompinput::poll_inputs(); recompinput::begin_binding_frame(); },
        get_input,
        recompinput::set_rumble,
        connected_device,
    };
    const ultramodern::gfx_callbacks_t gfx_callbacks{
        create_gfx, create_window, update_gfx};
    RT64::ProfilerGuestVI::install();
    const ultramodern::events::callbacks_t events_callbacks{
        on_vi, nullptr, jfc::enhancements::graphics_task_is_frame,
        jfc::enhancements::graphics_task_framebuffer,
        jfc::enhancements::graphics_task_metadata,
        [](ultramodern::events::RspTaskEvent event, uint64_t token, uint32_t task) {
            static constexpr const char *names[] = {"Runtime.RSP.GraphicsStart", "Runtime.RSP.GraphicsResume",
                "Runtime.RSP.Yield", "Runtime.RSP.GraphicsComplete", "Runtime.RSP.AudioStart", "Runtime.RSP.AudioComplete"};
            RT64::PerformanceProfiler::recordCriticalInstant(RT64::PerformanceCategory::CPU,
                names[static_cast<uint32_t>(event)], token, task);
        },
        [](ultramodern::events::GraphicsTaskEvent event, uint64_t token, uint32_t displayList) {
            using Event = ultramodern::events::GraphicsTaskEvent;
            switch (event) {
            case Event::Submitted: RT64::PerformanceProfiler::recordRuntimeGraphicsTaskSubmitted(token, displayList); break;
            case Event::Started: RT64::PerformanceProfiler::recordRuntimeGraphicsTaskStarted(token, displayList); break;
            case Event::Completed: RT64::PerformanceProfiler::recordRuntimeGraphicsTaskCompleted(token, displayList); break;
            }
        }};
    const ultramodern::error_handling::callbacks_t error_callbacks{
        recompui::message_box};
    const ultramodern::threads::callbacks_t thread_callbacks{game_thread_name};
    recomp::start(
        {JFC_VERSION_MAJOR, JFC_VERSION_MINOR, JFC_VERSION_PATCH},
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        events_callbacks,
        error_callbacks,
        thread_callbacks);
    return EXIT_SUCCESS;
}

}

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = 0x44830C263C2DE108ULL,
        .internal_name = "JET FORCE GEMINI",
        .display_name = "Jet Force Gemini",
        .game_id = std::u8string(game_id),
        .mod_game_id = {},
        .save_type = recomp::SaveType::Flashram,
        .thumbnail_bytes = {},
        .is_enabled = false,
        .expected_rom_size = 33'554'432,
        .allow_byteswapped = true,
        .has_compressed_code = false,
        .entrypoint_address = 0xFFFFFFFF80000400ULL,
        .entrypoint = recomp_entrypoint,
        .thread_create_callback = initialize_game_thread,
        .on_init_callback = jfc_load_bootstrap_overlay_data,
        .prepare_callback = static_cast<bool(*)()>(jfc::local_aot::prepare),
    },
};

int main(int argc, char** argv) {
    std::error_code path_error;
#ifdef _WIN32

    int native_argc = 0;
    auto native_argv = CommandLineToArgvW(GetCommandLineW(), &native_argc);
    if (native_argv == nullptr) {
        std::fprintf(stderr, "Failed to read native command line\n");
        return EXIT_FAILURE;
    }
    const std::filesystem::path rom_argument = native_argc == 2
        ? std::filesystem::path{native_argv[1]} : std::filesystem::path{};
    LocalFree(native_argv);
#else
    const std::filesystem::path rom_argument = argc == 2
        ? std::filesystem::path{argv[1]} : std::filesystem::path{};
#endif
    const std::filesystem::path command_line_rom = argc == 2
        ? std::filesystem::absolute(rom_argument, path_error)
        : std::filesystem::path{};
    if (path_error) {
        std::fprintf(stderr, "Failed to resolve ROM path: %s\n",
                     path_error.message().c_str());
        return EXIT_FAILURE;
    }

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "1");
    SDL_SetHint("SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "1");
#ifdef _WIN32
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", 0);
#endif
    if (SDL_Init(0) != 0) {
        std::fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    char* base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        std::fprintf(stderr, "Failed to locate application assets: %s\n",
                     SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    std::filesystem::path asset_path{std::u8string{
        reinterpret_cast<const char8_t*>(base_path)}};
#ifdef __linux__

    if (const char* appdir = std::getenv("APPDIR"); appdir && *appdir) {
        asset_path = std::filesystem::path{appdir} / "usr/bin";
    }
#endif
    std::filesystem::current_path(asset_path, path_error);
    SDL_free(base_path);
    if (path_error) {
        std::fprintf(stderr, "Failed to open application directory: %s\n",
                     path_error.message().c_str());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    constexpr const char* required_assets[] = {
        "assets/recomp.rcss",
        "assets/NotoEmoji-Regular.ttf",
        "assets/LatoLatin-Regular.ttf",
        "assets/LatoLatin-Bold.ttf",
        "assets/promptfont/promptfont.ttf",
        "assets/branding/rt64.json",
        "assets/branding/jet-force-capricorn-logo-v1.png",
    };
    for (const char* asset : required_assets) {
        if (!std::filesystem::is_regular_file(asset)) {
            std::fprintf(stderr,
                         "Required application asset is missing: %s. "
                         "Launch the complete application package, not the standalone binary.\n",
                         asset);
            SDL_Quit();
            return EXIT_FAILURE;
        }
    }

    if (NFD_Init() != NFD_OKAY) {
        std::fprintf(stderr, "Failed to initialize file dialogs: %s\n", NFD_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    int result = EXIT_FAILURE;
    try {
        result = run_application(argc, argv, command_line_rom);
    }
    catch (const std::exception& exception) {
        std::fprintf(stderr, "Fatal error: %s\n", exception.what());
    }
    try { jfc::modernization::flush_save_metadata(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "QoL save did not complete: %s\n", error.what());
        result = EXIT_FAILURE;
    }
    shutdown_audio();
    NFD_Quit();
    SDL_Quit();
    return result;
}
