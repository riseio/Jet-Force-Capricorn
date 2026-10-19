#pragma once

#include <utility>
#include <algorithm>
#include "recompinput/recompinput.h"

namespace jfc {

using BindingAlternatives = std::array<recompinput::InputField, recompinput::num_bindings_per_input>;

inline BindingAlternatives expand_held_bindings(const BindingAlternatives& triggers,
                                               const BindingAlternatives& modifiers) {
    BindingAlternatives result{};
    size_t count = 0;
    const auto append = [&](recompinput::InputField field) {
        if (std::find(result.begin(), result.begin() + count, field) != result.begin() + count) return;
        if (count == result.size()) throw std::invalid_argument("Too many combinations in legacy shortcut bindings");
        result[count++] = field;
    };
    for (const auto& trigger : triggers) {
        if (trigger.is_empty()) continue;
        if (trigger.modifier_count()) { append(trigger); continue; }
        for (const auto& modifier : modifiers) {
            if (modifier.is_empty()) continue;
            if (trigger == modifier) append(trigger);
            else if (modifier.input_type == recompinput::InputType::MouseWheel)
                append(modifier.holding(trigger));
            else append(trigger.holding(modifier));
        }
    }
    return result;
}

inline bool migrate_throwable_binding(recompinput::InputDevice device,
    recompinput::InputField& primary, recompinput::InputField& alternate) {
    using recompinput::InputField;
    if (!alternate.is_empty()) return false;
    if (device == recompinput::InputDevice::Keyboard && primary == InputField::keyboard(SDL_SCANCODE_G))
        primary = InputField::keyboard(SDL_SCANCODE_T);
    else if (device == recompinput::InputDevice::Controller &&
             primary == InputField::controller_digital(SDL_CONTROLLER_BUTTON_Y))
        primary = InputField::controller_digital(SDL_CONTROLLER_BUTTON_B);
    else return false;
    return true;
}

inline bool migrate_shortcut_binding(recompinput::InputDevice device,
    recompinput::GameInput input, recompinput::InputField& primary,
    recompinput::InputField& alternate) {
    using recompinput::GameInput;
    using recompinput::InputField;
    if (!alternate.is_empty()) return false;
    InputField old{}, replacement{};
    if (device == recompinput::InputDevice::Keyboard) {
        if (input == GameInput::CROUCH) {
            old = InputField::keyboard(SDL_SCANCODE_LCTRL);
            replacement = InputField::keyboard(SDL_SCANCODE_C);
        } else if (input == GameInput::QUICK_SWITCH) {
            old = InputField::keyboard(SDL_SCANCODE_Q);
            replacement = InputField::keyboard(SDL_SCANCODE_G);
        } else return false;
    } else if (device == recompinput::InputDevice::Controller) {
        switch (input) {
        case GameInput::QUICK_SWITCH:
            old = InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            replacement = InputField::controller_digital(SDL_CONTROLLER_BUTTON_Y); break;
        case GameInput::L:
            old = InputField::controller_digital(SDL_CONTROLLER_BUTTON_Y);
            replacement = InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
        case GameInput::DPAD_UP: old = InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_UP); break;
        case GameInput::DPAD_RIGHT: old = InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); break;
        case GameInput::DPAD_LEFT: old = InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_LEFT); break;
        default: return false;
        }
    } else return false;
    if (primary != old) return false;
    primary = replacement;
    return true;
}

inline bool migrate_weapon_binding_pair(recompinput::InputDevice device,
    recompinput::InputField& next, recompinput::InputField& next_alt,
    recompinput::InputField& previous, recompinput::InputField& previous_alt) {
    using recompinput::InputField;
    if (!next_alt.is_empty() || !previous_alt.is_empty()) return false;
    if (device == recompinput::InputDevice::Controller &&
        next == InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
        previous == InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) {
        std::swap(next, previous);
        return true;
    }
    if (device == recompinput::InputDevice::Keyboard &&
        next == InputField::keyboard(SDL_SCANCODE_E) &&
        previous == InputField::keyboard(SDL_SCANCODE_R)) {
        next_alt = InputField::mouse_wheel(false);
        previous_alt = InputField::mouse_wheel(true);
        return true;
    }
    return false;
}

}
