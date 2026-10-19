#pragma once

#include <cstddef>
#include <cstdint>

namespace jfc::deck_grips {

inline constexpr uint16_t btn_gripl = 0x224;
inline constexpr uint16_t btn_gripr = 0x225;
inline constexpr uint16_t btn_gripl2 = 0x226;
inline constexpr uint16_t btn_gripr2 = 0x227;
inline constexpr uint16_t btn_select = 0x13A;
inline constexpr size_t raw_reader_capacity = 8;

struct Edges {
    bool l4 = false;
    bool r4 = false;
    bool select = false;
    bool select_held = false;
    bool l5 = false;
    bool r5 = false;
    bool l5_held = false;
    bool r5_held = false;
    bool lower_available = false;
    bool l4_held = false;
    bool upper_available = false;
    bool r4_held = false;
};

inline Edges parse_key_event(uint16_t type, uint16_t code, int32_t value) {
    constexpr uint16_t ev_key = 0x01;
    if (type != ev_key || value != 1) {
        return {};
    }
    Edges result{code == btn_gripl, code == btn_gripr, code == btn_select};
    result.l5 = code == btn_gripl2;
    result.r5 = code == btn_gripr2;
    return result;
}

inline bool is_deck_report(const uint8_t* report, size_t size) {
    return size == 64 && report != nullptr && report[0] == 1 &&
        report[1] == 0 && report[2] == 9 && report[3] == 64;
}

inline Edges parse_report(
    const uint8_t* report,
    size_t size,
    bool& l4_held,
    bool& r4_held,
    bool& select_held,
    bool& l5_held,
    bool& r5_held
) {
    if (!is_deck_report(report, size)) {
        return {};
    }
    const bool l4 = (report[13] & (1U << 1)) != 0;
    const bool r4 = (report[13] & (1U << 2)) != 0;

    const bool select = (report[9] & (1U << 4)) != 0;

    const bool l5 = (report[9] & (1U << 7)) != 0;
    const bool r5 = (report[10] & 1U) != 0;
    Edges result{
        l4 && !l4_held,
        r4 && !r4_held,
        select && !select_held,
    };
    result.l5 = l5 && !l5_held;
    result.select_held = select;
    result.r5 = r5 && !r5_held;
    result.l5_held = l5;
    result.r5_held = r5;
    result.lower_available = true;
    result.l4_held = l4;
    result.r4_held = r4;
    result.upper_available = true;
    l4_held = l4;
    r4_held = r4;
    select_held = select;
    l5_held = l5;
    r5_held = r5;
    return result;
}

using SnapshotConsumer = void (*)(const Edges&);
Edges poll(SnapshotConsumer consume = nullptr);

}
