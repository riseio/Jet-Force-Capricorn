#include "texture_launcher.hpp"
#include "texture_installation.hpp"
#include "texture_reconstruction.hpp"
#include "save_state_rendezvous.hpp"
#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompui/renderer.h"
#include "elements/ui_modal.h"
#include "ultramodern/ultramodern.hpp"
#include <fstream>
#include <stdexcept>

extern std::vector<recomp::GameEntry> supported_games;

namespace jfc::textures {
Launcher::Launcher(std::filesystem::path folder) : folder_(std::move(folder)) {
    refresh();
    recompui::renderer::set_builtin_texture_pack_path(installed_ ? pack_path(folder_) : std::filesystem::path{});
    recompui::register_launcher_init_callback([this](auto* menu) { init(menu); });
    recompui::register_ui_update_callback([this] { update(); });
}

Launcher::~Launcher() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    if (pause_owned_) state::release_pause();
    recompui::register_launcher_init_callback({});
    recompui::register_ui_update_callback({});
}

void Launcher::init(recompui::LauncherMenu* launcher) {
    const auto& game = supported_games.at(0);
    menu_ = launcher->init_game_options_menu(game.game_id, game.mod_game_id,
        game.display_name, game.thumbnail_bytes);
    menu_->add_start_game_or_load_rom_option();
    menu_->add_setup_controls_option();
    menu_->add_settings_option();
    menu_->add_mods_option();
    action_ = menu_->add_option("Install enhanced textures", [this] { choose(); });
    exit_ = menu_->add_exit_option();
    status_ = recompui::get_current_context().create_element<recompui::Label>(menu_, "",
        recompui::LabelStyle::Small);
    status_->set_text_align(recompui::TextAlign::Center);

    for (auto* option : menu_->get_options()) option->set_padding(8.0f);
    refresh_launcher();
}

void Launcher::init_graphics(recompui::ContextId context, recompui::Element* parent) {
    parent->set_flex_direction(recompui::FlexDirection::Column);
    graphics_action_ = context.create_element<recompui::Button>(parent, action_title(),
        recompui::ButtonStyle::Secondary, recompui::ButtonSize::Medium);
    graphics_action_->set_debug_id("enhanced_texture_install_action");
    graphics_action_->add_pressed_callback([this] { choose(); });
    graphics_status_ = context.create_element<recompui::Label>(parent, "", recompui::LabelStyle::Small);
    refresh_graphics();
}

void Launcher::refresh() {
    installed_ = !installed_pack(folder_).empty();
    recompui::config::graphics::set_enhanced_textures_available(installed_);
    std::error_code error;
    present_ = std::filesystem::exists(pack_path(folder_), error);
    status_text_ = !installed_ && present_
        ? "Existing textures need verification. Install will check and reuse them."
        : installed_ ? "Enhanced textures installed. Choose On or Off in Graphics."
                     : "Original textures are ready to play. Enhanced textures are optional.";
    views_dirty_ = true;
}

std::string Launcher::action_title() const {
    if (busy_) return running_ == Action::Remove ? "Removing enhanced textures..."
        : "Installing enhanced textures...";
    return installed_ ? "Remove enhanced textures" : "Install enhanced textures";
}

void Launcher::refresh_launcher() {
    for (auto* option : menu_->get_options()) option->set_enabled(!busy_ || option == exit_);
    action_->set_enabled(!busy_);
    action_->set_title(action_title());
    status_->set_text(status_text_);
}

void Launcher::refresh_graphics() {
    graphics_action_->set_text(action_title());
    graphics_action_->set_enabled(!busy_);
    graphics_status_->set_text(status_text_);
}

void Launcher::refresh_views() {
    if (menu_) {
        auto context = recompui::get_launcher_context_id();
        context.open();
        refresh_launcher();
        context.close();
    }
    if (graphics_action_) {
        auto context = recompui::config::get_config_context_id();
        context.open();
        refresh_graphics();
        context.close();
    }
    if (busy_ && progress_modal_) {
        auto context = progress_modal_->modal_root_context;
        context.open();
        progress_status_->set_text(status_text_);
        progress_cancel_->set_enabled(running_ == Action::Install && !publication_.valid() && !cancelling_);
        context.close();
    }
    views_dirty_ = false;
}

void Launcher::confirm_removal() {
    if (busy_) return;
    recompui::open_choice_prompt("Remove enhanced textures?",
        "This removes the generated enhanced texture pack. Your ROM, saves, settings and mods are kept. You can install the pack again later.",
        "Remove", "Keep textures", [this] { pending_ = Action::Remove; }, {},
        recompui::ButtonStyle::Danger, recompui::ButtonStyle::Secondary, true);
}

void Launcher::choose() {
    if (busy_) return;
    if (installed_) { confirm_removal(); return; }
    auto id = supported_games.at(0).game_id;
    if (!recomp::is_rom_valid(id)) {
        recompui::open_info_prompt("Select your ROM first",
            "Use Load ROM before installing enhanced textures.", "OK", {});
        return;
    }
    recompui::open_choice_prompt("Install enhanced textures?",
        "Prepare enhanced textures from your ROM. This needs about 2 GB of free space and can take several minutes. Gameplay will pause until installation finishes or you cancel. Existing verified textures are reused. Closing the game cancels unfinished work safely.",
        "Install", "Not now", [this] { pending_ = Action::Install; }, {});
}

void Launcher::cancel() {
    if (!busy_ || running_ != Action::Install || publication_.valid() || cancelling_) return;
    cancelling_ = true;
    worker_.request_stop();
    status_text_ = "Cancelling installation and cleaning up unfinished work...";
    views_dirty_ = true;
}

void Launcher::open_progress() {
    if (!progress_modal_) {
        progress_modal_ = recompui::Modal::create_modal();
        auto context = progress_modal_->modal_root_context;
        context.open();
        auto* body = progress_modal_->get_body();
        body->set_flex_direction(recompui::FlexDirection::Column);
        body->set_justify_content(recompui::JustifyContent::Center);
        body->set_align_items(recompui::AlignItems::Center);
        body->set_padding(32.0f);
        progress_title_ = context.create_element<recompui::Label>(body, "", recompui::LabelStyle::Large);
        progress_status_ = context.create_element<recompui::Label>(body, "", recompui::LabelStyle::Normal);
        progress_status_->set_margin(24.0f);
        auto* explanation = context.create_element<recompui::Label>(body,
            "Gameplay stays paused until this operation finishes.\nClosing the game safely cancels unfinished installation.",
            recompui::LabelStyle::Small);
        explanation->set_text_align(recompui::TextAlign::Center);
        progress_cancel_ = context.create_element<recompui::Button>(body, "Cancel installation", recompui::ButtonStyle::Secondary);
        progress_cancel_->set_margin(24.0f);
        progress_cancel_->set_debug_id("enhanced_texture_cancel");
        progress_cancel_->add_pressed_callback([this] { cancel(); });
        auto* quit = context.create_element<recompui::Button>(body, "Quit game", recompui::ButtonStyle::Secondary);
        quit->add_pressed_callback([] { recompui::open_quit_game_prompt(); });
        progress_modal_->set_menu_action_callback(recompui::MenuAction::Back, [this] { cancel(); }, "Cancel installation");
        progress_modal_->set_menu_action_callback(recompui::MenuAction::Toggle, [this] { cancel(); });
        context.set_autofocus_element(progress_cancel_);
        context.close();
    }
    auto context = progress_modal_->modal_root_context;
    context.open();
    progress_title_->set_text(running_ == Action::Install ? "Installing enhanced textures" : "Removing enhanced textures");
    progress_status_->set_text(status_text_);
    progress_modal_->open();
    context.close();
}

void Launcher::begin(Action action) {
    busy_ = true;
    running_ = action;
    cancelling_ = false;
    result_.store(Result::Working, std::memory_order_relaxed);
    { std::lock_guard lock(progress_mutex_); progress_ = {}; }
    shown_progress_ = {};
    error_.clear();
    status_text_ = action == Action::Install ? "Checking existing textures, then preparing missing data..."
        : "Releasing enhanced textures before removing the generated pack...";
    views_dirty_ = true;
    open_progress();
    try {
        if (ultramodern::is_game_started()) {
            pause_ = state::request_pause();
            pause_owned_ = true;
            status_text_ = "Pausing gameplay...";
        } else start_operation();
    } catch (const std::exception& error) {
        error_ = error.what();
        finish(Result::Failed);
    }
}

void Launcher::start_operation() {
    if (cancelling_) { finish(Result::Cancelled); return; }
    status_text_ = running_ == Action::Install ? "Checking existing textures..."
        : "Releasing enhanced textures before removing the generated pack...";
    views_dirty_ = true;
    if (running_ == Action::Remove) {
        recompui::config::graphics::set_enhanced_textures_available(false);
        publication_ = recompui::renderer::set_builtin_texture_pack_path({});
    } else {
        start_worker(running_);
    }
}

void Launcher::start_worker(Action action) {
    const auto rom_path = folder_ / supported_games.at(0).stored_filename();
    try {
        worker_ = std::jthread([this, action, rom_path](std::stop_token cancel) {
            std::stop_source stop;
            std::stop_callback user_cancel(cancel, [&] { stop.request_stop(); });
            std::stop_callback shutdown(ultramodern::shutdown_token(), [&] { stop.request_stop(); });
            Result result = Result::Success;
            try {
                if (stop.stop_requested()) throw PreparationCancelled{};
                if (action == Action::Remove) {
                    remove_pack(folder_);
                } else {
                    std::ifstream file(rom_path, std::ios::binary | std::ios::ate);
                    if (!file || file.tellg() != 33554432)
                        throw std::runtime_error("The selected ROM is missing or has the wrong size. Select it again with Load ROM.");
                    std::vector<uint8_t> rom(33554432);
                    file.seekg(0);
                    if (!file.read(reinterpret_cast<char*>(rom.data()), rom.size()))
                        throw std::runtime_error("Cannot read the selected ROM for texture installation.");
                    prepare_pack(rom, folder_, {}, stop.get_token(), [this](PreparationProgress progress) {
                        std::lock_guard lock(progress_mutex_);
                        progress_ = progress;
                    });
                }
            } catch (const PreparationCancelled&) {
                result = Result::Cancelled;
            } catch (const std::exception& error) {
                error_ = error.what();
                result = Result::Failed;
            }
            result_.store(result, std::memory_order_release);
        });
    } catch (const std::exception& error) {
        error_ = error.what();
        result_.store(Result::Failed, std::memory_order_release);
    }
}

void Launcher::update() {
    if (pending_ != Action::None && !busy_) {
        const auto action = pending_;
        pending_ = Action::None;
        begin(action);
    }
    if (views_dirty_) refresh_views();
    if (!busy_) return;
    if (pause_.valid()) {
        if (pause_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try { pause_.get(); start_operation(); }
        catch (const std::exception& error) { error_ = error.what(); finish(Result::Failed); }
        return;
    }
    if (publication_.valid()) {
        if (publication_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            if (!publication_.get()) {
                error_ = "The renderer could not update the texture pack. Files have been kept. Try again.";
                finish(Result::Failed);
            } else if (running_ == Action::Remove) {
                start_worker(Action::Remove);
            } else {
                finish(Result::Success);
            }
        } catch (const std::exception& error) {
            error_ = error.what();
            finish(Result::Failed);
        }
        return;
    }
    PreparationProgress progress;
    { std::lock_guard lock(progress_mutex_); progress = progress_; }
    if (!cancelling_ && progress != shown_progress_) {
        shown_progress_ = progress;
        if (progress.stage == PreparationStage::Preparing)
            status_text_ = "Prepared " + std::to_string(progress.completed) + " textures...";
        else
            status_text_ = std::string(progress.stage == PreparationStage::Checking ? "Checking existing textures: " : "Verifying completed textures: ") +
                std::to_string(progress.total ? progress.completed * 100 / progress.total : 0) + "%";
        views_dirty_ = true;
    }
    const auto result = result_.load(std::memory_order_acquire);
    if (result == Result::Working) return;
    if (worker_.joinable()) worker_.join();
    if (result == Result::Success && running_ == Action::Install) {
        auto path = installed_pack(folder_);
        if (path.empty()) {
            error_ = "The generated texture pack could not be verified. Install again to repair it.";
            finish(Result::Failed);
            return;
        }
        result_.store(Result::Working, std::memory_order_relaxed);
        publication_ = recompui::renderer::set_builtin_texture_pack_path(std::move(path));
        status_text_ = "Publishing installed textures to the renderer...";
        views_dirty_ = true;
        return;
    }
    finish(result);
}

void Launcher::finish(Result result) {
    busy_ = false;
    if (pause_owned_) { state::release_pause(); pause_owned_ = false; }
    if (progress_modal_) {
        auto context = progress_modal_->modal_root_context;
        context.open();
        progress_modal_->close();
        context.close();
    }
    refresh();
    if (result == Result::Failed) {
        installed_ = false;
        recompui::config::graphics::set_enhanced_textures_available(false);
        status_text_ = error_;
        recompui::open_info_prompt("Enhanced textures", error_, "OK", {});
    } else if (result == Result::Cancelled) {
        status_text_ = "Installation cancelled. Original textures are ready to play.";
    } else if (running_ == Action::Remove) {
        status_text_ = "Enhanced textures removed. Original textures are ready to play.";
    }
}
}
