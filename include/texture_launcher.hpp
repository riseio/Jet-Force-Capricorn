#pragma once
#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <mutex>
#include "texture_reconstruction.hpp"

namespace recompui { class LauncherMenu; class GameOptionsMenu; class GameOption; class Label; class Element; class Button; class ContextId; class Modal; }
namespace jfc::textures {
class Launcher {
public:
    explicit Launcher(std::filesystem::path folder);
    ~Launcher();
    Launcher(const Launcher&) = delete;
    Launcher& operator=(const Launcher&) = delete;
    void init_graphics(recompui::ContextId context, recompui::Element* parent);
private:
    enum class Action { None, Install, Remove };
    enum class Result { Working, Success, Cancelled, Failed };
    void init(recompui::LauncherMenu* menu);
    void update();
    void refresh();
    void choose();
    void confirm_removal();
    void begin(Action action);
    void start_operation();
    void cancel();
    void open_progress();
    void start_worker(Action action);
    void finish(Result result);
    void refresh_views();
    void refresh_launcher();
    void refresh_graphics();
    std::string action_title() const;
    std::filesystem::path folder_;
    recompui::GameOptionsMenu* menu_{};
    recompui::GameOption* action_{};
    recompui::GameOption* exit_{};
    recompui::Label* status_{};
    recompui::Button* graphics_action_{};
    recompui::Label* graphics_status_{};
    recompui::Modal* progress_modal_{};
    recompui::Label* progress_title_{};
    recompui::Label* progress_status_{};
    recompui::Button* progress_cancel_{};
    bool installed_{};
    bool present_{};
    bool busy_{};
    bool views_dirty_{};
    bool cancelling_{};
    bool pause_owned_{};
    Action pending_ = Action::None;
    Action running_ = Action::None;
    PreparationProgress shown_progress_{};
    PreparationProgress progress_{};
    std::mutex progress_mutex_;
    std::atomic<Result> result_{Result::Working};
    std::string error_;
    std::string status_text_;
    std::future<bool> publication_;
    std::future<void> pause_;
    std::jthread worker_;
};
}
