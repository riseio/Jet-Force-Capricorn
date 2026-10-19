/*
PromptFont (assets/promptfont/promptfont.ttf)
Copyright 2018-2023, Yukari Hafner
Copyright 2011-2016, Severin Meyer

This Font Software is licensed under the SIL Open Font License,
Version 1.1. This license is copied below, and is also available
with a FAQ at <http://scripts.sil.org/OFL>


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
*/
#include "input_prompts.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <stdexcept>

#include "SDL.h"

#include "jfc_recomp.h"
#include "native_context.hpp"
#include "librecomp/addresses.hpp"
#include "recompinput/players.h"
#include "recompinput/profiles.h"

extern "C" void fontPrintXY(uint8_t*, recomp_context*);

namespace {

constexpr size_t binding_capacity = 128;

constexpr size_t prompt_capacity = 1024;
constexpr size_t binding_words = (binding_capacity + sizeof(uint64_t) - 1U) /
    sizeof(uint64_t);

enum class PromptBinding : size_t {
    Fire,
    Action,
    Interact,
    Start,
    Back,
    Count
};

struct AtomicBindingLabel {
    std::atomic_uint sequence{0};
    std::array<std::atomic_uint64_t, binding_words> storage{};
};

std::array<AtomicBindingLabel, static_cast<size_t>(PromptBinding::Count)>
    binding_labels{};
std::atomic<jfc::input_prompts::PromptProfileDevice> prompt_device{
    jfc::input_prompts::PromptProfileDevice::Keyboard};

constexpr std::string_view controller_button_name(
    SDL_GameControllerButton button
) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return "A";
    case SDL_CONTROLLER_BUTTON_B: return "B";
    case SDL_CONTROLLER_BUTTON_X: return "X";
    case SDL_CONTROLLER_BUTTON_Y: return "Y";
    case SDL_CONTROLLER_BUTTON_BACK: return "VIEW";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "HOME";
    case SDL_CONTROLLER_BUTTON_START: return "MENU";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "LS";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "RS";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-PAD UP";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-PAD DOWN";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-PAD LEFT";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-PAD RIGHT";
    case SDL_CONTROLLER_BUTTON_MISC1: return "OPTIONS";
    case SDL_CONTROLLER_BUTTON_PADDLE1: return "R4";
    case SDL_CONTROLLER_BUTTON_PADDLE2: return "L4";
    case SDL_CONTROLLER_BUTTON_PADDLE3: return "R5";
    case SDL_CONTROLLER_BUTTON_PADDLE4: return "L5";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "TOUCHPAD";
    default: return {};
    }
}

constexpr std::string_view controller_axis_name(int32_t encoded_axis) {
    if (encoded_axis == 0) {
        return {};
    }
    const bool positive = encoded_axis > 0;
    const auto axis = static_cast<SDL_GameControllerAxis>(
        (positive ? encoded_axis : -encoded_axis) - 1);
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX: return positive ? "LS RIGHT" : "LS LEFT";
    case SDL_CONTROLLER_AXIS_LEFTY: return positive ? "LS DOWN" : "LS UP";
    case SDL_CONTROLLER_AXIS_RIGHTX: return positive ? "RS RIGHT" : "RS LEFT";
    case SDL_CONTROLLER_AXIS_RIGHTY: return positive ? "RS DOWN" : "RS UP";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "LT";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "RT";
    default: return {};
    }
}

std::array<char, binding_capacity> sanitize_label(std::string_view value) {
    std::array<char, binding_capacity> output{};
    size_t write = 0;
    for (const unsigned char c : value) {
        if (write + 1U >= output.size()) {
            break;
        }
        if (c >= 0x20U && c <= 0x7EU) {
            output[write++] = static_cast<char>(std::toupper(c));
        }
    }
    return output;
}

bool label_empty(const std::array<char, binding_capacity>& label) {
    return label[0] == '\0';
}

std::array<char, binding_capacity> ascii_binding_name(
    const recompinput::InputField& binding
) {
    using recompinput::InputType;
    if (binding.modifier_count()) {
        std::array<char, binding_capacity> output{};
        size_t used = 0;
        for (size_t i = 0; i <= binding.modifier_count(); ++i) {
            const auto atom = i < binding.modifier_count() ? binding.modifiers[i] : binding.atom();
            const auto label = ascii_binding_name({atom.input_type, atom.input_id});
            const int written = std::snprintf(output.data() + used, output.size() - used,
                "%s%s", i ? "+" : "", label.data());
            if (written < 0 || size_t(written) >= output.size() - used) return {};
            used += size_t(written);
        }
        return output;
    }
    switch (binding.input_type) {
    case InputType::ControllerDigital:
        return sanitize_label(controller_button_name(
            static_cast<SDL_GameControllerButton>(binding.input_id)));
    case InputType::ControllerAnalog:
        return sanitize_label(controller_axis_name(binding.input_id));
    case InputType::Keyboard: {
        const char* name = SDL_GetScancodeName(
            static_cast<SDL_Scancode>(binding.input_id));
        return sanitize_label(name == nullptr ? std::string_view{} :
            std::string_view{name});
    }
    case InputType::Mouse: {
        std::array<char, binding_capacity> output{};
        if (binding.input_id > 0 && binding.input_id <= 99) {
            std::snprintf(output.data(), output.size(), "MOUSE %d",
                binding.input_id);
        }
        return output;
    }
    case InputType::MouseWheel:
        return sanitize_label(binding.input_id == 1 ? "WHEEL UP" :
            binding.input_id == -1 ? "WHEEL DOWN" : "");
    case InputType::None:
    default:
        return {};
    }
}

void publish_label(
    PromptBinding binding,
    const std::array<char, binding_capacity>& label
) {
    auto& destination = binding_labels[static_cast<size_t>(binding)];
    destination.sequence.fetch_add(1U, std::memory_order_acq_rel);
    for (size_t word = 0; word < destination.storage.size(); ++word) {
        uint64_t packed = 0;
        const size_t begin = word * sizeof(uint64_t);
        const size_t count = std::min(sizeof(uint64_t), label.size() - begin);
        std::memcpy(&packed, label.data() + begin, count);
        destination.storage[word].store(packed, std::memory_order_relaxed);
    }
    destination.sequence.fetch_add(1U, std::memory_order_release);
}

std::array<char, binding_capacity> read_label(PromptBinding binding) {
    std::array<char, binding_capacity> result{};
    auto& source = binding_labels[static_cast<size_t>(binding)];
    for (;;) {
        const unsigned before = source.sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        for (size_t word = 0; word < source.storage.size(); ++word) {
            const uint64_t packed =
                source.storage[word].load(std::memory_order_relaxed);
            const size_t begin = word * sizeof(uint64_t);
            const size_t count = std::min(sizeof(uint64_t), result.size() - begin);
            std::memcpy(result.data() + begin, &packed, count);
        }
        if (before == source.sequence.load(std::memory_order_acquire)) {
            if (result[0] == '\0') {
                std::memcpy(result.data(), "UNBOUND", sizeof("UNBOUND"));
            }
            return result;
        }
    }
}

std::array<char, binding_capacity> profile_binding_label(
    int profile,
    recompinput::GameInput input
) {
    if (profile < 0 ||
        profile >= recompinput::profiles::get_input_profile_count()) {
        return {};
    }
    for (size_t index = 0; index < recompinput::num_bindings_per_input;
         ++index) {
        auto label = ascii_binding_name(recompinput::profiles::get_input_binding(
            profile, input, index));
        if (!label_empty(label)) {
            return label;
        }
    }
    return {};
}

std::array<char, binding_capacity> resolve_binding_label(
    recompinput::GameInput input,
    bool controller_connected
) {
    const recompinput::InputDevice assigned_device =
        recompinput::players::get_player_input_device(0);
    const auto assigned = assigned_device == recompinput::InputDevice::Controller
        ? jfc::input_prompts::PromptProfileDevice::Controller
        : assigned_device == recompinput::InputDevice::Keyboard
            ? jfc::input_prompts::PromptProfileDevice::Keyboard
            : jfc::input_prompts::PromptProfileDevice::None;
    const auto selected = jfc::input_prompts::select_profile_device(
        assigned, controller_connected);
    int profile = -1;
    if (assigned != jfc::input_prompts::PromptProfileDevice::None) {
        profile = recompinput::profiles::get_input_profile_for_player(
            0, assigned_device);
    }
    else if (selected == jfc::input_prompts::PromptProfileDevice::Controller) {
        profile = recompinput::profiles::get_sp_controller_profile_index();
    }
    else {
        profile = recompinput::profiles::get_sp_keyboard_profile_index();
    }

    return profile_binding_label(profile, input);
}

bool native_text_equals(
    uint8_t* rdram,
    uint32_t address,
    std::string_view expected
) {
    if (address < 0x80000000U ||
        address - 0x80000000U + expected.size() + 1U > recomp::mem_size) {
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (MEM_BU(static_cast<uint32_t>(i), address) !=
            static_cast<uint8_t>(expected[i])) {
            return false;
        }
    }
    return MEM_BU(static_cast<uint32_t>(expected.size()), address) == 0;
}

struct PromptReplacement {
    PromptBinding binding;
    const char* format;
};

bool prompt_replacement(
    uint8_t* rdram,
    uint32_t address,
    PromptReplacement& replacement
) {
    if (native_text_equals(rdram, address, "Save Game")) {
        replacement = {PromptBinding::Fire, "%s  Save Game"};
        return true;
    }
    if (native_text_equals(rdram, address, "Press the A button")) {
        replacement = {PromptBinding::Interact, "Press %s"};
        return true;
    }
    if (native_text_equals(rdram, address, "PRESS START")) {
        replacement = {PromptBinding::Start, "PRESS %s"};
        return true;
    }
    if (native_text_equals(rdram, address, "Push START button")) {
        replacement = {PromptBinding::Start, "Press %s"};
        return true;
    }
    return false;
}

struct BindingToken {
    std::string_view text;
    PromptBinding binding;
};

constexpr std::array binding_tokens{
    BindingToken{"Z Button", PromptBinding::Fire},
    BindingToken{"Z button", PromptBinding::Fire},
    BindingToken{"A Button", PromptBinding::Action},
    BindingToken{"A button", PromptBinding::Action},
    BindingToken{"START button", PromptBinding::Start},
};

bool valid_native_byte(uint32_t address) {
    return address >= 0x80000000U &&
        static_cast<size_t>(address - 0x80000000U) < recomp::mem_size;
}

bool native_text_starts_with(
    uint8_t* rdram,
    uint32_t address,
    std::string_view expected
) {
    if (address < 0x80000000U) {
        return false;
    }
    const size_t offset = static_cast<size_t>(address - 0x80000000U);
    if (offset > recomp::mem_size || expected.size() > recomp::mem_size - offset) {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (MEM_BU(static_cast<uint32_t>(index), address) !=
            static_cast<uint8_t>(expected[index])) {
            return false;
        }
    }
    return true;
}

bool copy_with_binding_tokens(
    uint8_t* rdram,
    uint32_t address,
    std::array<char, prompt_capacity>& output
) {
    size_t read = 0;
    size_t write = 0;
    bool replaced = false;
    while (read < prompt_capacity) {
        if (read > UINT32_MAX - address ||
            !valid_native_byte(address + static_cast<uint32_t>(read))) {
            return false;
        }
        const uint8_t value = MEM_BU(static_cast<uint32_t>(read), address);
        if (value == 0U) {
            output[write] = '\0';
            return replaced;
        }

        const BindingToken* matched = nullptr;
        for (const BindingToken& token : binding_tokens) {
            if (native_text_starts_with(
                    rdram, address + static_cast<uint32_t>(read), token.text)) {
                matched = &token;
                break;
            }
        }

        if (matched == nullptr) {
            if (write + 1U >= output.size()) {
                return false;
            }
            output[write++] = static_cast<char>(value);
            ++read;
            continue;
        }

        const auto label = read_label(matched->binding);
        const size_t label_length = std::strlen(label.data());
        if (label_length >= output.size() - write) {
            return false;
        }
        std::memcpy(output.data() + write, label.data(), label_length);
        write += label_length;
        read += matched->text.size();
        replaced = true;
    }
    return false;
}

bool build_adapted_prompt(
    uint8_t* rdram,
    uint32_t address,
    std::array<char, prompt_capacity>& prompt
) {
    PromptReplacement replacement{};
    if (prompt_replacement(rdram, address, replacement)) {
        const auto label = read_label(replacement.binding);
        const int written = std::snprintf(
            prompt.data(), prompt.size(), replacement.format, label.data());
        return written >= 0 && static_cast<size_t>(written) < prompt.size();
    }
    return copy_with_binding_tokens(rdram, address, prompt);
}

uint32_t publish_adapted_prompt(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t source_address,
    bool draw
) {
    std::array<char, prompt_capacity> prompt{};
    if (!build_adapted_prompt(rdram, source_address, prompt)) {
        return 0;
    }

    const size_t length = std::strlen(prompt.data());

    constexpr uint32_t argument_area = 0x20;
    const uint32_t frame_size = (argument_area + uint32_t(length) + 1U + 15U) & ~15U;
    const uint32_t stack = uint32_t(ctx->r29);
    const uint32_t incoming_size = draw ? 0x14U : 0x10U;
    if ((stack & 7U) != 0 || stack < 0x80000000U + frame_size ||
        stack > 0x80800000U - incoming_size) {
        throw std::runtime_error("Invalid native stack for binding prompt");
    }
    const uint32_t frame = stack - frame_size;
    for (uint32_t offset = 0; offset < incoming_size; offset += 4)
        MEM_W(offset, frame) = MEM_W(offset, stack);
    const uint32_t prompt_address = frame + argument_area;
    for (size_t index = 0; index <= length; ++index) {
        MEM_B(static_cast<uint32_t>(index), prompt_address) = prompt[index];
    }
    ctx->r29 = int32_t(frame);
    return prompt_address;
}

}

namespace jfc::input_prompts {

void refresh_bindings(bool controller_connected) {
    publish_label(PromptBinding::Fire,
        resolve_binding_label(recompinput::GameInput::Z, controller_connected));
    publish_label(PromptBinding::Action,
        resolve_binding_label(recompinput::GameInput::A, controller_connected));
    publish_label(PromptBinding::Interact,
        resolve_binding_label(recompinput::GameInput::INTERACT, controller_connected));
    publish_label(PromptBinding::Start,
        resolve_binding_label(
            recompinput::GameInput::START, controller_connected));
    publish_label(PromptBinding::Back,
        resolve_binding_label(recompinput::GameInput::CROUCH, controller_connected));
    const auto assigned = recompinput::players::get_player_input_device(0);
    prompt_device.store(select_profile_device(
        assigned == recompinput::InputDevice::Controller ? PromptProfileDevice::Controller :
        assigned == recompinput::InputDevice::Keyboard ? PromptProfileDevice::Keyboard :
        PromptProfileDevice::None, controller_connected), std::memory_order_relaxed);
}

PromptProfileDevice current_device() noexcept {
    return prompt_device.load(std::memory_order_relaxed);
}

}

extern "C" void jfc_draw_robot_binding(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t row = uint32_t(ctx->r17);
    if ((row & 1U) || !valid_native_byte(row) || !valid_native_byte(row + 1))
        throw std::runtime_error("Invalid robot mission prompt row");
    PromptBinding binding;
    switch (MEM_HU(0, row)) {
    case 497: case 499: case 502: case 504: binding = PromptBinding::Interact; break;
    case 498: case 500: case 503: binding = PromptBinding::Back; break;
    case 501: binding = PromptBinding::Fire; break;
    default: throw std::runtime_error("Unknown robot mission prompt action");
    }
    const auto label = read_label(binding);
    const uint32_t stack = uint32_t(ctx->r29);
    constexpr uint32_t frame_size = 0x60;
    if ((stack & 7U) || stack < 0x80000000U + frame_size || stack >= 0x80800000U)
        throw std::runtime_error("Invalid native stack for robot mission prompt");
    recomp_context call;
    jfc::runtime::copy_context(call, *ctx);
    call.r29 = int32_t(stack - frame_size);
    call.r4 = ctx->r21;
    call.r5 = 200;
    call.r6 = ctx->r19;
    call.r7 = uint32_t(call.r29) + 0x20;
    MEM_W(0x10, call.r29) = 9;
    for (size_t i = 0; i < label.size(); ++i) MEM_B(i, call.r7) = label[i];
    fontPrintXY(rdram, &call);
}

extern "C" void jfc_adapt_native_prompt_text(
    uint8_t* rdram,
    recomp_context* ctx
) {
    if (ctx == nullptr) {
        return;
    }
    const uint32_t prompt_address = publish_adapted_prompt(
        rdram, ctx, static_cast<uint32_t>(ctx->r7), true);
    if (prompt_address != 0U) {
        ctx->r7 = prompt_address;
    }
}

extern "C" void jfc_adapt_native_prompt_width_text(
    uint8_t* rdram,
    recomp_context* ctx
) {
    if (ctx == nullptr) {
        return;
    }
    const uint32_t prompt_address = publish_adapted_prompt(
        rdram, ctx, static_cast<uint32_t>(ctx->r4), false);
    if (prompt_address != 0U) {
        ctx->r4 = prompt_address;
    }
}

extern "C" void jfc_hide_legacy_save_prompt_icon(
    uint8_t*,
    recomp_context* ctx
) {

    ctx->r6 = static_cast<uint32_t>(-1024);
}
