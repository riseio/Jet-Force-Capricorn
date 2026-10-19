#include "save_state_rendezvous.hpp"
#include "audio_output.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/state.hpp"

namespace jfc::state {
namespace {
std::atomic<Action> requested{Action::None};
std::atomic_bool ready_for_request{true};
std::atomic_bool pause_released{false};
std::promise<void> pause_completion;
Transaction* transaction_callback = nullptr;
Completion* completion_callback = nullptr;
Roots roots{};
Roots staged_roots{};
bool roots_staged = false;
GameState game_state;
std::unique_ptr<PreparedEnhancements> staged_enhancements;
std::unique_ptr<PreparedModernization> staged_modernization;
std::string owner_capture_error;

uint32_t parked = 0;
bool restore_pending = false;
const char* boundary_error = nullptr;
Action completed_action = Action::None;
std::string completion_error;
std::chrono::steady_clock::time_point deadline;
constexpr uint32_t all_roots = 7;
constexpr uint32_t game_bit = 1;
constexpr uint32_t audio_bit = 2;
constexpr uint32_t rdram_size = 8U * 1024 * 1024;

void complete() noexcept {
    if (completed_action != Action::Pause && completion_callback) completion_callback(completed_action,
        completion_error.empty() ? nullptr : completion_error.c_str());
    completed_action = Action::None;
    completion_error.clear();
    staged_enhancements.reset();
    staged_modernization.reset();
    ready_for_request.store(true, std::memory_order_release);
}

bool valid_address(uint32_t address, uint32_t length) {
    const uint32_t segment = address & 0xe0000000U;
    return (segment == 0x80000000U || segment == 0xa0000000U) &&
        length <= rdram_size && (address & 0x1fffffffU) <= rdram_size - length;
}

bool scheduler_drained(uint8_t* rdram, const recomp_context& ctx) {
    const uint32_t scheduler = static_cast<uint32_t>(ctx.r18);
    if (!valid_address(scheduler, 0x308)) {
        boundary_error = "Invalid scheduler at save-state boundary";
        return false;
    }

    for (uint32_t offset = 0x2e4; offset <= 0x2fc; offset += 4) {
        if (MEM_W(offset, scheduler) != 0) return false;
    }
    return true;
}

void release(uint8_t* rdram) {

    for (size_t i = 0; i < roots.size(); ++i) {
        if ((parked & (1U << i)) != 0) {
            ultramodern::schedule_running_thread(rdram, static_cast<int32_t>(roots[i].thread));
        }
    }
    requested.store(Action::None, std::memory_order_release);
    if (parked == 0) complete();

    ultramodern::check_running_queue(rdram);
}
}

void initialize_rendezvous(Transaction* transaction, Completion* completion) {
    if (ultramodern::is_game_started()) throw std::logic_error("State rendezvous must be installed before boot");
    transaction_callback = transaction;
    completion_callback = completion;
    ultramodern::threads::set_state_idle_callback(idle_checkpoint);
}

void stage_restore_roots(const Roots& saved) {
    if (parked != all_roots || requested.load(std::memory_order_acquire) != Action::Restore) {
        throw std::logic_error("Root restore preflight requires a complete rendezvous");
    }
    for (size_t i = 0; i < roots.size(); ++i) validate_root(saved[i], roots[i]);
    staged_roots = saved;
    roots_staged = true;
}

const GameState& captured_game_state() {
    if (parked != all_roots) throw std::logic_error("Game sidecar capture requires all roots parked");
    return game_state;
}
void stage_restore_game(const GameState& saved) {
    if (parked != all_roots || requested.load(std::memory_order_acquire) != Action::Restore)
        throw std::logic_error("Game sidecar preparation requires a restore rendezvous");
    auto enhancements = std::make_unique<PreparedEnhancements>(saved.enhancements);
    auto modernization = std::make_unique<PreparedModernization>(saved.modernization);
    staged_enhancements = std::move(enhancements);
    staged_modernization = std::move(modernization);
}

bool request(Action action) {
    if ((action != Action::Save && action != Action::Restore) || transaction_callback == nullptr || !ultramodern::is_game_initialized()) return false;
    bool expected = true;
    if (!ready_for_request.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return false;
    requested.store(action, std::memory_order_release);
    return true;
}

std::future<void> request_pause() {
    if (!ultramodern::is_game_initialized())
        throw std::runtime_error("Wait for the game to finish loading before installing textures.");
    std::promise<void> completion;
    auto result = completion.get_future();
    bool expected = true;
    if (!ready_for_request.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        throw std::runtime_error("A save or restore is in progress. Try again after it finishes.");
    pause_completion = std::move(completion);
    pause_released.store(false, std::memory_order_relaxed);
    requested.store(Action::Pause, std::memory_order_release);
    return result;
}

void release_pause() noexcept {
    pause_released.store(true, std::memory_order_release);
}

void idle_checkpoint(uint8_t* rdram) {
    const Action action = requested.load(std::memory_order_acquire);
    if (action == Action::None || (parked == 0 && boundary_error == nullptr)) return;
    std::string error;
    if (boundary_error) {
        error = boundary_error;
    } else if (parked == all_roots) {
        try {
            if (action == Action::Pause) {
                ultramodern::state::VideoPause video_pause(true);
                ultramodern::state::TimerPause timer_pause;
                pause_completion.set_value();
                while (!pause_released.load(std::memory_order_acquire) && !ultramodern::shutdown_token().stop_requested())
                    ultramodern::sleep_milliseconds(10);
            } else {
                roots_staged = false;
                staged_enhancements.reset();
                staged_modernization.reset();
                transaction_callback(action, rdram, roots);
                if (action == Action::Restore && (!roots_staged || !staged_enhancements || !staged_modernization)) {
                    throw std::logic_error("Restore transaction did not preflight native roots and game sidecars");
                }
                restore_pending = action == Action::Restore;
            }
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    } else if (std::chrono::steady_clock::now() >= deadline) {
        error = "Save-state rendezvous timed out; game state was not changed";
    } else {
        return;
    }
    completed_action = action;
    completion_error = std::move(error);
    if (action == Action::Pause && !completion_error.empty())
        pause_completion.set_exception(std::make_exception_ptr(std::runtime_error(completion_error)));
    boundary_error = nullptr;
    release(rdram);
}
}

extern "C" uint32_t jfc_state_requested() {
    return jfc::state::requested.load(std::memory_order_acquire) != jfc::state::Action::None;
}

extern "C" void jfc_state_root(uint8_t* rdram, recomp_context* ctx, uint32_t root, uint64_t* locals) noexcept(false) {
    using namespace jfc::state;
    if (root >= roots.size() || !jfc_state_requested() || boundary_error != nullptr) return;
    const uint32_t bit = 1U << root;
    if ((parked & bit) != 0) return;
    if (root == uint32_t(Root::Audio) && (parked & game_bit) == 0) return;
    if (root == uint32_t(Root::Scheduler) &&
        ((parked & (game_bit | audio_bit)) != (game_bit | audio_bit) || !scheduler_drained(rdram, *ctx))) return;
    const auto self = ultramodern::this_thread();
    const auto* thread = TO_PTR(OSThread, self);
    constexpr std::array<int32_t, 3> expected_ids{3, -4, 5};
    if (thread->id != expected_ids[root]) {
        boundary_error = "Unexpected thread at save-state root";
        return;
    }
    std::optional<AudioPlaybackPause> playback_pause;
    try {
        playback_pause.emplace();
        if (root == uint32_t(Root::Game) && requested.load(std::memory_order_acquire) != Action::Pause) {
            game_state.enhancements = capture_enhancements();
            game_state.modernization = capture_modernization();
        }
    } catch (const std::exception& failure) {
        owner_capture_error = failure.what();
        boundary_error = owner_capture_error.c_str();
        return;
    }
    if (parked == 0) {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        restore_pending = false;
    }
    roots[root] = capture_root(static_cast<uint32_t>(self), *ctx, locals, get_cop1_cs());
    parked |= bit;
    TO_PTR(OSThread, self)->state = OSThreadState::STOPPED;
    ultramodern::run_next_thread_and_wait(rdram);
    if (restore_pending) {
        if (root == uint32_t(Root::Game)) {
            staged_enhancements->commit();
            staged_modernization->commit();
        }
        restore_root(staged_roots[root], *ctx, locals);
        set_cop1_cs(staged_roots[root].cop1);
    }
    parked &= ~bit;
    playback_pause.reset();
    if (parked == 0) complete();
}
