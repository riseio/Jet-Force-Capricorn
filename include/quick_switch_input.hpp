#pragma once

namespace jfc {

class QuickSwitchInput {
    bool controller_was_down_ = true;
    bool keyboard_was_down_ = true;
    bool controller_qualified_ = false;
    bool keyboard_qualified_ = false;
public:
    bool sample(bool enabled, bool controller_down, bool modifier, bool keyboard_down,
                bool keyboard_modifier = false) {
        if (!enabled) {
            controller_was_down_ = keyboard_was_down_ = true;
            controller_qualified_ = keyboard_qualified_ = false;
            return false;
        }
        if (!controller_down || !modifier) controller_qualified_ = false;
        else if (!controller_was_down_) controller_qualified_ = true;
        if (!keyboard_down || !keyboard_modifier) keyboard_qualified_ = false;
        else if (!keyboard_was_down_) keyboard_qualified_ = true;
        controller_was_down_ = controller_down;
        keyboard_was_down_ = keyboard_down;
        return controller_qualified_ || keyboard_qualified_;
    }
};

class StickSwitchInput {
    bool armed_ = false;
    bool owned_ = false;
public:
    bool sample(bool enabled, bool left, bool right) {
        if (!enabled) { armed_ = false; owned_ = false; return false; }
        if (!left && !right) { armed_ = true; owned_ = false; }
        if (armed_ && left && right) { armed_ = false; owned_ = true; }
        return owned_;
    }
    bool owns_clicks() const { return owned_; }
};

}
