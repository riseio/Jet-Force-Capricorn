#ifndef JFC_INPUT_PROMPTS_HPP
#define JFC_INPUT_PROMPTS_HPP

namespace jfc::input_prompts {

enum class PromptProfileDevice {
    None,
    Controller,
    Keyboard,
};

constexpr PromptProfileDevice select_profile_device(
    PromptProfileDevice assigned,
    bool controller_connected
) {
    if (assigned == PromptProfileDevice::Controller ||
        assigned == PromptProfileDevice::Keyboard) {
        return assigned;
    }
    return controller_connected
        ? PromptProfileDevice::Controller
        : PromptProfileDevice::Keyboard;
}

void refresh_bindings(bool controller_connected);
PromptProfileDevice current_device() noexcept;

}

#endif
