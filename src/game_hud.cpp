#include "game_hud.hpp"
#include "boss_health.hpp"
#include "input_prompts.hpp"
#include "native_context.hpp"
#include "native_rom_data.hpp"
#include "jfc_modernization.hpp"
#include "jfc_recomp.h"
#include "../include/rt64_extended_gbi.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <stdexcept>

extern "C" void fontUseFont(uint8_t*, recomp_context*);
extern "C" void fontColour(uint8_t*, recomp_context*);
extern "C" void fontBackground(uint8_t*, recomp_context*);
extern "C" void fontPrintXY(uint8_t*, recomp_context*);
extern "C" void fontStringWidth(uint8_t*, recomp_context*);
extern "C" void frontDrawObj(uint8_t*, recomp_context*);
extern "C" void mainGetGameCharacter(uint8_t*, recomp_context*);
extern "C" void loadFrontEndItem(uint8_t*, recomp_context*);
extern "C" void freeFrontEndItem(uint8_t*, recomp_context*);
extern "C" void texDPInit(uint8_t*, recomp_context*);
extern "C" void texDPTextureX(uint8_t*, recomp_context*);

namespace {

std::atomic_bool modern_health{false};
std::atomic_bool room_counters{true};
std::atomic_bool area_pickups{false};
std::atomic_bool weapon_shortcuts_hud{true};
constexpr uint32_t display_list = 0x800FF398U;
constexpr uint32_t font_window = 0x80103FA8U;
constexpr uint32_t font_in_use = 0x80103BA0U;
constexpr uint32_t frontend_items = 0x800FF3F8U;
constexpr uint32_t frontend_loaded = 0x800FF6C8U;

constexpr int token_item = 160;
constexpr int token_variant = 0;
constexpr int tribal_item = 46;
constexpr int key_item = 145;

struct AreaMetadata {
    uint8_t* memory = nullptr;
    std::array<uint32_t, 5> identity{};
    jfc::hud::AreaPickups definitions;
    bool ready = false;
} area_metadata;
struct TribalMetadata {
    uint8_t* memory = nullptr;
    uint32_t level = 0, list = 0;
    int count = 0;
    uint16_t mask = 0;
    bool ready = false, valid = false;
} tribal_metadata;
int counter_column_width = 0;
std::array<char, 6> measured_count{};
int measured_count_width = 0;

using jfc::native_data::IconBounds;
enum class TextAlign : uint32_t { Left = 0, Right = 1, Center = 4 };

bool readable(uint32_t address, uint32_t size, uint32_t alignment = 1) {
    constexpr uint32_t rdram_size = 8U * 1024U * 1024U;
    const uint32_t segment = address & 0xE0000000U;
    const uint32_t physical = address & 0x1FFFFFFFU;
    return (segment == 0x80000000U || segment == 0xA0000000U) &&
        (address % alignment) == 0 && size <= rdram_size &&
        physical <= rdram_size - size;
}

void command(uint8_t* rdram, uint32_t w0, uint32_t w1) {
    const auto cursor = static_cast<uint32_t>(MEM_W(0, display_list));
    if (!readable(cursor, 8, 8)) {
        std::fprintf(stderr, "JFC HUD invalid display-list cursor %08X\n", cursor);
        std::abort();
    }
    MEM_W(0, cursor) = w0;
    MEM_W(4, cursor) = w1;
    MEM_W(0, display_list) = cursor + 8;
}

constexpr std::array state_pushes{
    G_EX_PUSHSCISSOR_V1, G_EX_PUSHVIEWPORT_V1, G_EX_PUSHPROJMATRIX_V1,
    G_EX_PUSHGEOMETRYMODE_V1, G_EX_PUSHOTHERMODE_V1, G_EX_PUSHCOMBINE_V1,
    G_EX_PUSHENVCOLOR_V1, G_EX_PUSHBLENDCOLOR_V1, G_EX_PUSHFOGCOLOR_V1,
    G_EX_PUSHFILLCOLOR_V1, G_EX_PUSHPRIMCOLOR_V1};
constexpr std::array state_pops{
    G_EX_POPPRIMCOLOR_V1, G_EX_POPFILLCOLOR_V1, G_EX_POPFOGCOLOR_V1,
    G_EX_POPBLENDCOLOR_V1, G_EX_POPENVCOLOR_V1, G_EX_POPCOMBINE_V1,
    G_EX_POPOTHERMODE_V1, G_EX_POPGEOMETRYMODE_V1, G_EX_POPPROJMATRIX_V1,
    G_EX_POPVIEWPORT_V1, G_EX_POPSCISSOR_V1};

class NativeHudDraw {
    uint8_t* rdram;
    recomp_context call;
    std::array<uint8_t, 40> previous_window{};
    uint8_t previous_font;
    uint32_t text_address;
public:
    NativeHudDraw(uint8_t* memory, const recomp_context& context, bool right, bool bottom = false, bool top = false, bool center = false)
        : rdram(memory), call{}, previous_font(MEM_BU(0, font_in_use)) {
        jfc::runtime::copy_context(call, context);
        const auto stack = static_cast<uint32_t>(context.r29);
        if (stack < 0x200 || !readable(stack - 0x200, 0x200, 8)) {
            std::fputs("JFC HUD invalid native call stack\n", stderr);
            std::abort();
        }

        call.r29 = stack - 0x100;
        text_address = static_cast<uint32_t>(call.r29) + 0x40;
        for (size_t i = 0; i < previous_window.size(); ++i)
            previous_window[i] = MEM_BU(i, font_window);
        command(rdram, (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            (RT64_HOOK_OP_PUSH << 28) | RT64_EXTENDED_OPCODE);
        command(rdram, 0x64000000U | G_EX_UIALPHASCOPE_V1, 1);
        for (auto op : state_pushes) command(rdram, 0x64000000U | op, 0);
        const uint32_t origin = center ? G_EX_ORIGIN_CENTER : right ? G_EX_ORIGIN_RIGHT : G_EX_ORIGIN_LEFT;
        command(rdram, 0x64000000U | G_EX_SETRECTASPECT_V1, G_EX_ASPECT_ADJUST);
        command(rdram, 0x64000000U | G_EX_SETDRAWSCALE_V1,
            G_EX_SCALE_ONE | (origin << 9) |
            (((!top && (right || bottom)) ? G_EX_SCALE_ORIGIN_BOTTOM : G_EX_SCALE_ORIGIN_TOP) << 21));
        command(rdram, 0x64000000U | G_EX_SETRECTALIGN_V1, origin | (origin << 12));

        const uint32_t x_offset = center ? (uint32_t(uint16_t(-160 * 4)) << 16) :
            right ? (uint32_t(uint16_t(-320 * 4)) << 16) : 0;
        command(rdram, x_offset, x_offset);
        command(rdram, 0x64000000U | G_EX_SETVIEWPORTALIGN_V1, origin);
        command(rdram, 0, 0);
        command(rdram, 0x64000000U | G_EX_SETSCISSORALIGN_V1,
            G_EX_ORIGIN_LEFT | (G_EX_ORIGIN_RIGHT << 12));
        command(rdram, 0, 0);
        command(rdram, 0, 0x7FFF7FFFU);
        command(rdram, 0x64000000U | G_EX_SETSCISSOR_V1, G_EX_ORIGIN_RIGHT << 14);
        command(rdram, 0, 240 * 4);
        call.r4 = 2;
        fontUseFont(rdram, &call);
        call.r4 = call.r5 = call.r6 = call.r7 = 0;
        fontBackground(rdram, &call);
    }
    ~NativeHudDraw() {

        call.r4 = 0;
        texDPInit(rdram, &call);
        command(rdram, 0xE7000000U, 0);
        for (auto op : state_pops) command(rdram, 0x64000000U | op, 0);
        command(rdram, 0x64000000U | G_EX_UIALPHASCOPE_V1, 0);
        command(rdram, (RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER,
            RT64_HOOK_OP_POP << 28);
        for (size_t i = 0; i < previous_window.size(); ++i)
            MEM_B(i, font_window) = previous_window[i];
        MEM_B(0, font_in_use) = previous_font;
    }
    void box(int x, int y, int width, int height, uint32_t rgb) {
        if (width <= 0 || height <= 0) return;
        const uint32_t rgba16 = ((rgb >> 19) & 31) << 11 |
            ((rgb >> 11) & 31) << 6 | ((rgb >> 3) & 31) << 1 | 1;
        command(rdram, 0xE7000000U, 0);
        command(rdram, 0xEF300000U, 0);
        command(rdram, 0xF7000000U, rgba16 | (rgba16 << 16));
        command(rdram, 0xF6000000U | ((x + width - 1) * 4 << 12) |
            ((y + height - 1) * 4), (x * 4 << 12) | (y * 4));
    }
    void cell(int x, int y, int fixed, uint32_t rgb) {

        box(x, y, 6, 1, rgb); box(x, y + 5, 6, 1, rgb);
        box(x, y + 1, 1, 4, rgb); box(x + 5, y + 1, 1, 4, rgb);
        box(x + 1, y + 1, (4 * std::clamp(fixed, 0, 256) + 255) / 256, 4, rgb);
    }
    void icon(uint32_t texture, int x, int y, int width, int height) {
        if (!readable(texture, 32, 4) || MEM_BU(0, texture) == 0 ||
            MEM_BU(1, texture) == 0) {
            std::fputs("JFC HUD: invalid native counter texture\n", stderr);
            std::abort();
        }
        call.r4 = 0; texDPInit(rdram, &call);
        call.r4 = display_list; call.r5 = texture; call.r6 = 5; call.r7 = 0;
        texDPTextureX(rdram, &call);
        command(rdram, 0xE7000000U, 0);

        command(rdram, 0xEF002CF0U, 0x00504240U);
        command(rdram, 0xFCFFFFFFU, 0xFFFCF279U);
        command(rdram, 0xE4000000U | ((x + width) * 4 << 12) |
            ((y + height) * 4), (x * 4 << 12) | (y * 4));
        command(rdram, 0xB3000000U, 0);
        command(rdram, 0xB2000000U,
            ((MEM_BU(0, texture) * 1024 / width) << 16) |
            (MEM_BU(1, texture) * 1024 / height));
    }
    void prepare_text(const char* value) {
        const size_t length = std::strlen(value);
        if (length >= 64) {
            std::fputs("JFC HUD label exceeds native scratch capacity\n", stderr);
            std::abort();
        }
        for (size_t i = 0; i <= length; ++i) MEM_B(i, text_address) = value[i];
    }
    int text_width(const char* value) {
        prepare_text(value);
        call.r4 = text_address; call.r5 = 2; call.r6 = 1;
        fontStringWidth(rdram, &call);
        const int width = static_cast<int>(call.r2);
        if (width < 0 || width > 320) throw std::runtime_error("Invalid native HUD text width");
        return width;
    }
    int numeric_width() {
        if (counter_column_width != 0) return counter_column_width;

        char value[3]{};
        int width = text_width("--");
        for (char digit = '0'; digit <= '9'; ++digit) {
            std::fill_n(value, 2, digit);
            width = std::max(width, text_width(value));
        }
        return counter_column_width = width;
    }
    void text(int x, int centre_y, const char* value, TextAlign align = TextAlign::Left) {
        prepare_text(value);
        call.r4 = 128; call.r5 = 255; call.r6 = 192; call.r7 = 255;
        MEM_W(0x10, call.r29) = 255;
        fontColour(rdram, &call);
        call.r4 = display_list; call.r5 = x; call.r6 = centre_y; call.r7 = text_address;
        MEM_W(0x10, call.r29) = 8 | uint32_t(align);
        fontPrintXY(rdram, &call);
    }
    void model_icon(int item, int variant, IconBounds bounds, int width, int height,
        int right_x, int centre_y) {

        constexpr uint32_t record = 0x800FF780U + 50 * 32;
        const uint32_t object = MEM_W(item * 4, frontend_items);
        if (!readable(object, 0x4C, 4)) {
            char error[192];
            std::snprintf(error, sizeof(error),
                "Invalid native HUD model: item=%d variant=%d object=%08X loaded=%u level=%u heap_end=%08X",
                item, variant, object, unsigned(MEM_BU(item, frontend_loaded)),
                unsigned(MEM_HU(0, 0x800A323CU)), uint32_t(MEM_W(0, 0x800FDDA8U)));
            std::fprintf(stderr, "JFC_HUD_RESOURCE_FAILURE %s\n", error);
            throw std::runtime_error(error);
        }
        std::array<uint8_t, 32> saved_record;
        std::array<uint8_t, 0x40> saved_object;
        for (size_t i = 0; i < saved_record.size(); ++i) saved_record[i] = MEM_BU(i, record);
        for (size_t i = 0; i < saved_object.size(); ++i) saved_object[i] = MEM_BU(i, object);
        const uint32_t opacity = MEM_W(0, 0x800A51CCU);
        const auto restore = [&] {
            MEM_W(0, 0x800A51CCU) = opacity;
            for (size_t i = 0; i < saved_record.size(); ++i) MEM_B(i, record) = saved_record[i];
            for (size_t i = 0; i < saved_object.size(); ++i) MEM_B(i, object) = saved_object[i];
        };
        const uint32_t viewport_index = MEM_W(0, 0x800FA634U);
        if (viewport_index >= 4) throw std::runtime_error("Invalid native HUD viewport index");

        command(rdram, 0x64000000U | G_EX_SETVIEWPORTALIGN_V1, G_EX_ORIGIN_RIGHT);
        command(rdram, uint32_t(uint16_t(-320 * 4)) << 16, 0);
        command(rdram, 0x03800010U, 0x000A3008U + viewport_index * 16);
        MEM_H(0, record) = -32768; MEM_H(2, record) = 0; MEM_H(4, record) = 0;
        MEM_H(6, record) = item;
        MEM_B(0x3A, object) = variant;
        const float scale = std::min(float(width)/(bounds.right-bounds.left),
            float(height)/(bounds.top-bounds.bottom));
        const float centre_x = (bounds.left+bounds.right)*0.5f;
        const float centre_y_model = (bounds.bottom+bounds.top)*0.5f;
        MEM_W(8, record) = std::bit_cast<uint32_t>(scale);

        MEM_W(12, record) = std::bit_cast<uint32_t>(right_x - 160.0f - width*0.5f + centre_x*scale);
        MEM_W(16, record) = std::bit_cast<uint32_t>(120.0f - centre_y - centre_y_model*scale);
        MEM_W(20, record) = 0;
        MEM_W(0, 0x800A51CCU) = 255;
        call.r4 = 50;
        const uint32_t first_command = MEM_W(0, display_list);
        try { frontDrawObj(rdram, &call); }
        catch (...) { restore(); throw; }
        restore();
    }
    void coin(int right_x, int centre_y) {

        model_icon(token_item, token_variant, jfc::native_data::get().token_bounds, 16,16,right_x,centre_y);
    }
    void pickup_counter(uint32_t mask, bool ship, int centre_y, bool valid) {
        const int icon_right = 310-numeric_width()-4;
        if (ship) {
            const unsigned part = mask ? std::countr_zero(mask) : 0;
            model_icon(111+part,0,jfc::native_data::get().part_bounds.at(part),24,16,icon_right,centre_y);
        }
        else model_icon(key_item,8,jfc::native_data::get().key_bounds.at(8),24,16,icon_right,centre_y);
        char value[8];
        if (valid) std::snprintf(value,sizeof(value),"%d",std::popcount(mask));
        else std::snprintf(value,sizeof(value),"--");
        text(309,centre_y,value,TextAlign::Right);
    }
    void key_counter(int variant, int centre_y) {
        const IconBounds bounds = jfc::native_data::get().key_bounds.at(variant);
        model_icon(key_item,variant,bounds,24,16,310-numeric_width()-4,centre_y);
        text(309,centre_y,"1",TextAlign::Right);
    }
    void floyd_counter(uint8_t parts, int centre_y) {

        for (int variant = 1; variant <= 4; ++variant)
            model_icon(token_item,variant,jfc::native_data::get().floyd_bounds,24,16,
                310-numeric_width()-4,centre_y);
        char value[4];
        std::snprintf(value,sizeof(value),"%d",std::popcount(parts));
        text(309,centre_y,value,TextAlign::Right);
    }
    void counter(const char* value, int centre_y, bool token, int frame = 0) {
        const int column_width = numeric_width();
        const int text_left = 310 - column_width;
        const int width = token ? 16 : 24;
        const int height = token ? 16 : 10;
        const int icon_right = text_left - 4;

        if (token) coin(icon_right - (24 - width) / 2, centre_y);
        else icon(jfc::hud::read_tribal_icon(rdram, frame),
            icon_right - width, centre_y - height / 2, width, height);

        int value_width = column_width;
        if (std::strlen(value) > 2) {
            if (std::strcmp(measured_count.data(), value) != 0) {
                measured_count_width = text_width(value);
                std::snprintf(measured_count.data(), measured_count.size(), "%s", value);
            }
            value_width = measured_count_width;
        }
        const int scale = value_width > column_width
            ? G_EX_SCALE_ONE * column_width / value_width : G_EX_SCALE_ONE;
        if (scale != G_EX_SCALE_ONE) {
            command(rdram, 0x64000000U | G_EX_SETDRAWSCALE_V1,
                scale | (G_EX_ORIGIN_RIGHT << 9) | (G_EX_SCALE_ORIGIN_BOTTOM << 21));
            text(320 - (11 * G_EX_SCALE_ONE + scale / 2) / scale,
                240 - ((240 - centre_y) * G_EX_SCALE_ONE + scale / 2) / scale,
                value, TextAlign::Right);
            command(rdram, 0x64000000U | G_EX_SETDRAWSCALE_V1,
                G_EX_SCALE_ONE | (G_EX_ORIGIN_RIGHT << 9) | (G_EX_SCALE_ORIGIN_BOTTOM << 21));
        }
        else text(309, centre_y, value, TextAlign::Right);
    }
};

}

void jfc::hud::invalidate_metadata() noexcept {
    jfc::boss::invalidate_target();
    area_metadata.ready = false;
    tribal_metadata.ready = false;
    counter_column_width = 0;
    measured_count[0] = 0;
}

extern "C" void jfc_invalidate_hud_metadata() {
    jfc::hud::invalidate_metadata();
}

void jfc::hud::set_modern_health(bool enabled) {
    modern_health.store(enabled, std::memory_order_relaxed);
}

void jfc::hud::set_room_counters(bool enabled) {
    room_counters.store(enabled, std::memory_order_relaxed);
}

void jfc::hud::set_area_pickups(bool enabled) {
    area_pickups.store(enabled, std::memory_order_relaxed);
}

void jfc::hud::set_weapon_shortcuts(bool enabled) {
    weapon_shortcuts_hud.store(enabled, std::memory_order_relaxed);
}

uint32_t jfc::hud::read_weapon_icon(uint8_t* rdram, unsigned weapon) {
    if (weapon >= 16) return 0;
    const unsigned item = 10 + weapon;
    if (!MEM_BU(item, frontend_loaded)) return 0;
    auto resource = uint32_t(MEM_W(item * 4, frontend_items));
    const auto table = uint32_t(MEM_W(0, 0x800A51D0U));
    if (!readable(table, (item + 1) * 2, 2)) return 0;
    const unsigned type = MEM_HU(item * 2, table) & 0xC000;
    if (type == 0xC000) return readable(resource, 32, 4) ? resource : 0;
    if (type != 0x8000 || !readable(resource, 0x14, 4) || MEM_H(2, resource) <= 0) return 0;
    const auto frames = uint32_t(MEM_W(0x10, resource));
    if (!readable(frames, 4, 4)) return 0;
    resource = uint32_t(MEM_W(0, frames));
    return readable(resource, 32, 4) ? resource : 0;
}

extern "C" uint16_t jfc_hud_weapon_mask(uint8_t* rdram) {
    const unsigned character = MEM_HU(0, 0x800A3240U) & 3U;
    const auto game = uint32_t(MEM_W(0, 0x800FD7D4U));
    if (character >= 3 || !readable(game, 0x15C + 3 * 0x76, 2)) return 0;
    const auto shortcuts = jfc::modernization::weapon_shortcuts(rdram, character);
    uint16_t mask = uint16_t(1U << shortcuts.throwable);
    if (weapon_shortcuts_hud.load(std::memory_order_relaxed))
        for (auto encoded : shortcuts.slots) if (encoded && encoded <= 16) mask |= 1U << (encoded - 1);
    return mask & MEM_HU(0x15C + character * 0x76 + 0xA, game);
}

namespace {
void acquire_weapon_icon(uint8_t* rdram, const recomp_context& ctx, unsigned weapon) {
    const auto item = 10 + weapon;
    if (MEM_BU(item, frontend_loaded)) return;
    recomp_context call;
    jfc::runtime::copy_context(call, ctx); call.r29 -= 0x100; call.r4 = item;
    loadFrontEndItem(rdram, &call);
}
void draw_weapon_icon(NativeHudDraw& draw, uint8_t* rdram, unsigned weapon, int x, int y, int extent) {
    const auto texture = jfc::hud::read_weapon_icon(rdram, weapon);
    if (!texture || !MEM_BU(0, texture) || !MEM_BU(1, texture))
        throw std::runtime_error("Invalid native weapon HUD icon");
    const int w = MEM_BU(0, texture), h = MEM_BU(1, texture), longest = std::max(w,h);
    const int width = std::max(1, extent * w / longest), height = std::max(1, extent * h / longest);
    draw.icon(texture, x - width/2, y - height/2, width, height);
}
}

extern "C" void jfc_draw_weapon_shortcuts(uint8_t* rdram, recomp_context* ctx) {
    if (!weapon_shortcuts_hud.load(std::memory_order_relaxed)) return;
    const auto player = uint32_t(ctx->r17);
    if (!readable(player,2) || MEM_B(0,player) != 0) return;
    const auto shortcuts = jfc::modernization::weapon_shortcuts(rdram, MEM_BU(1,player) & 3U);
    const auto mask = jfc_hud_weapon_mask(rdram);
    for (auto encoded : shortcuts.slots)
        if (encoded && (mask & (1U << (encoded-1)))) acquire_weapon_icon(rdram,*ctx,encoded-1);
    NativeHudDraw draw(rdram,*ctx,true,false,true);
    const bool controller = jfc::input_prompts::current_device() ==
        jfc::input_prompts::PromptProfileDevice::Controller;
    constexpr std::array<std::array<int,2>,4> controller_positions{{
        {274,17},{298,40},{274,63},{250,40}}};
    if (controller) {
        draw.box(272,32,4,17,0x709990);
        draw.box(265,38,18,4,0x709990);
    }
    for (unsigned slot=0;slot<4;++slot) {
        const auto [x,y] = controller ? controller_positions[slot] :
            std::array<int,2>{216 + int(slot)*27, 32};
        if (!controller) {
            draw.box(x-12,6,24,1,0x709990);
            draw.box(x-12,44,24,1,0x709990);
            draw.box(x-12,7,1,37,0x709990);
            draw.box(x+11,7,1,37,0x709990);
            const char label[]{char('1'+slot),0};
            draw.text(x,15,label,TextAlign::Center);
        }
        const auto encoded = shortcuts.slots[slot];
        if (encoded && (mask & (1U << (encoded-1)))) draw_weapon_icon(draw,rdram,encoded-1,x,y,18);
        else { draw.box(x-4,y,8,1,0x709990); }
    }
}

extern "C" void jfc_draw_boss_health(uint8_t* rdram, recomp_context* ctx) {
    const auto target = jfc::boss::current_target(rdram,uint32_t(ctx->r17));
    if (!target.name) return;
    NativeHudDraw draw(rdram,*ctx,false,false,true,true);
    const auto color = target.destroyed ? 0x606870U : target.vulnerable ? 0x65ED96U : 0xD5A34AU;
    if (target.maximum > 0) {
        draw.box(110,27,100,5,0x343C44);
        draw.box(110,27,(100*target.remaining+target.maximum-1)/target.maximum,5,color);
    }
    draw.text(160,17,target.name,TextAlign::Center);
    draw.text(160,41,target.destroyed ? "DESTROYED" : target.vulnerable ? "VULNERABLE" : "PROTECTED",TextAlign::Center);
}

extern "C" int jfc_draw_modern_health(uint8_t* rdram, recomp_context* ctx) {
    if (!modern_health.load(std::memory_order_relaxed) ||
        MEM_BU(0, 0x800A4FD0U) != 1) return 0;
    const auto health = jfc::hud::read_health(rdram,
        static_cast<uint32_t>(ctx->r4), static_cast<int>(ctx->r6));
    if (!health.valid) return 0;
    NativeHudDraw draw(rdram, *ctx, false);
    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", jfc::hud::active_health_percent(health));
    draw.text(43, 8, text, TextAlign::Right);
    for (int i = 0; i < health.reserve_containers; ++i) {
        draw.cell(48 + i * 8, 5, i < health.full_reserves ? 256 : 0, 0xFFD34E);
    }
    return 1;
}

uint32_t jfc::hud::read_tribal_icon(uint8_t* rdram, int frame) {
    const int item = tribal_item;
    if (!MEM_BU(item, frontend_loaded)) return 0;
    auto pointer = static_cast<uint32_t>(MEM_W(item * 4, frontend_items));
    if (!readable(pointer, 0x14, 4) || frame < 0 ||
        frame >= MEM_H(2, pointer)) return 0;
    pointer = static_cast<uint32_t>(MEM_W(0x10, pointer));
    if (!readable(pointer, (frame + 1) * 4, 4)) return 0;
    return static_cast<uint32_t>(MEM_W(frame * 4, pointer));
}

extern "C" void jfc_free_counter_icons(uint8_t* rdram, recomp_context* ctx) {
    jfc::hud::invalidate_metadata();

    recomp_context call;
    for (int item : {10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
                    111,112,113,114,115,116,117,118,119,120,121,122,key_item,token_item}) {
        if (!MEM_BU(item, frontend_loaded)) continue;
        jfc::runtime::copy_context(call, *ctx);
        call.r29 -= 0x100;
        call.r4 = item;
        freeFrontEndItem(rdram, &call);
    }
}

extern "C" void jfc_preserve_frontend_model_textures(uint8_t* rdram, recomp_context* ctx) {

    if (ctx->r4 != 0) return;
    const auto model = uint32_t(ctx->r18);
    constexpr unsigned item_count = 180;
    const auto table = uint32_t(MEM_W(0, 0x800A51D0U));
    const auto checked = [](uint32_t pointer, uint32_t size) {
        if (!readable(pointer, size, 4))
            throw std::runtime_error("Invalid frontend model ownership during texture suspension");
        return pointer;
    };
    checked(model, 0x50);
    if (!readable(table, item_count * 2, 2))
        throw std::runtime_error("Invalid frontend resource table during texture suspension");
    const auto owns = [&](uint32_t instance) {
        const auto definition = uint32_t(MEM_W(0, checked(instance, 4)));
        checked(definition, 0x50);
        return (definition & 0x1FFFFFFFU) == (model & 0x1FFFFFFFU);
    };

    for (unsigned item = 0; item < item_count; ++item) {
        if (!MEM_BU(item, frontend_loaded)) continue;
        const unsigned type = MEM_HU(item * 2, table) & 0xC000;
        if (type & 0x8000) continue;
        const auto resource = uint32_t(MEM_W(item * 4, frontend_items));
        if (type == 0) {
            if (owns(resource)) { ctx->r4 = 1; return; }
            continue;
        }
        const auto object = checked(resource, 0x70);
        const auto header = checked(uint32_t(MEM_W(0x40, object)), 0x20);
        if (MEM_B(0x1E, header) != 0) continue;
        const int count = MEM_B(0x1F, header);
        if (count <= 0) continue;
        const auto instances = checked(uint32_t(MEM_W(0x6C, object)), count * 4);

        for (int variant = 0; variant < count; ++variant) {
            if (owns(uint32_t(MEM_W(variant * 4, instances)))) {
                ctx->r4 = 1;
                return;
            }
        }
    }
}

extern "C" void jfc_draw_grenade_counter(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t player = uint32_t(ctx->r17);
    if (!readable(player, 2) || MEM_B(0, player) != 0) return;
    recomp_context call;
    jfc::runtime::copy_context(call, *ctx);
    call.r4 = MEM_B(0, player); call.r5 = MEM_B(1, player);
    mainGetGameCharacter(rdram, &call);
    const uint32_t record = uint32_t(call.r2);
    const auto weapon = jfc::modernization::weapon_shortcuts(rdram, MEM_BU(1,player) & 3U).throwable;
    if (!readable(record, 0x76, 2) || !(MEM_HU(0xA, record) & (1U << weapon))) return;
    const int count = std::max(0, int(MEM_H(0x14 + 2 * weapon, record)));
    acquire_weapon_icon(rdram,*ctx,weapon);
    NativeHudDraw draw(rdram, *ctx, false, true);
    draw_weapon_icon(draw,rdram,weapon,17,221,18);
    char label[8]; std::snprintf(label, sizeof(label), "%d", count);
    draw.text(27, 221, label);
}

extern "C" void jfc_draw_room_counters(uint8_t* rdram, recomp_context* ctx) {
    const bool rooms = room_counters.load(std::memory_order_relaxed);
    const bool pickups = area_pickups.load(std::memory_order_relaxed);
    if (!rooms && !pickups) return;
    const auto player = static_cast<uint32_t>(ctx->r17);
    if (!readable(player, 2)) return;
    recomp_context call;
    jfc::runtime::copy_context(call, *ctx);
    call.r4 = MEM_B(0, player); call.r5 = MEM_B(1, player);
    mainGetGameCharacter(rdram, &call);
    const auto character = static_cast<uint32_t>(call.r2);
    const auto counts = rooms ? jfc::hud::read_counters(rdram, character) : jfc::hud::Counters{};
    const auto area = pickups ? jfc::hud::read_area_pickups(rdram, character) : jfc::hud::AreaPickups{};
    const bool tokens_visible = rooms && (!counts.tokens_valid || counts.mizar_tokens != 0);
    const bool tribals_visible = rooms && (!counts.tribals_valid || counts.room_tribals.missing != 0);
    const bool lost_visible = rooms && counts.tribals_valid && counts.room_tribals.lost != 0;
    const bool keys_visible = pickups && (!area.valid || area.keys != 0);
    const bool parts_visible = pickups && (!area.valid || area.ship_parts != 0);
    const bool floyd_visible = pickups && area.valid && area.floyd_parts != 0;
    if (!tokens_visible && !tribals_visible && !lost_visible && !keys_visible && !parts_visible && !floyd_visible) return;

    const auto acquire = [&](int item) {
        if (MEM_BU(item, frontend_loaded)) return;
        jfc::runtime::copy_context(call, *ctx); call.r29 -= 0x100; call.r4 = item;
        loadFrontEndItem(rdram, &call);
    };
    if (tokens_visible || floyd_visible) acquire(token_item);
    if (parts_visible) {
        acquire(111+(area.ship_parts ? std::countr_zero(area.ship_parts) : 0));
    }
    if (keys_visible) acquire(key_item);
    NativeHudDraw draw(rdram, *ctx, true);

    int row_y = 221;
    const auto next_row = [&] { const int y = row_y; row_y -= 19; return y; };
    char text[48];
    if (tokens_visible) {
        if (counts.tokens_valid) std::snprintf(text, sizeof(text), "%d", counts.mizar_tokens);
        else std::snprintf(text, sizeof(text), "--");
        draw.counter(text, next_row(), true);
    }
    if (tribals_visible) {
        if (counts.tribals_valid) std::snprintf(text, sizeof(text), "%d", counts.room_tribals.missing);
        else std::snprintf(text, sizeof(text), "--");
        draw.counter(text, next_row(), false);
    }
    if (keys_visible) {
        uint32_t remaining = area.keys;
        if (area.valid) for (const auto& icon : jfc::native_data::get().key_icons) {
            const uint32_t bit = 1U << icon.flag;
            if ((remaining & bit) == 0) continue;
            draw.key_counter(icon.variant, next_row());
            remaining &= ~bit;
        }

        if (!area.valid || remaining) draw.pickup_counter(remaining, false, next_row(), area.valid);
    }
    if (parts_visible) draw.pickup_counter(area.ship_parts, true, next_row(), area.valid);
    if (floyd_visible) draw.floyd_counter(area.floyd_parts, next_row());
    if (lost_visible) {
        std::snprintf(text, sizeof(text), "%d", counts.room_tribals.lost);
        draw.counter(text, next_row(), false, 1);
    }
}

jfc::hud::Health jfc::hud::read_health(
    uint8_t* rdram, uint32_t player_object, int capacity
) {
    if (!readable(player_object, 0x50, 4)) return {};
    const auto record = static_cast<uint32_t>(MEM_W(0x4C, player_object));
    if (!readable(record, 8, 2)) return {};
    return health(MEM_H(6, record), capacity);
}

jfc::hud::Counters jfc::hud::read_counters(
    uint8_t* rdram, uint32_t character_save
) {
    Counters result;
    if (readable(character_save, 0x14, 2)) {
        const int tokens = MEM_H(0x12, character_save);
        if (tokens >= 0 && tokens <= 32000) {
            result.mizar_tokens = tokens;
            result.tokens_valid = true;
        }
    }

    const auto level = static_cast<uint32_t>(MEM_W(0, 0x800FB118U));
    const auto save = static_cast<uint32_t>(MEM_W(0, 0x800FD7D4U));
    if (!readable(level, 0x104) || !readable(save, 0x15C, 2)) return result;
    const uint32_t region = MEM_BU(0x103, level);
    if (region >= 64) return result;

    const int count = MEM_W(0, 0x800FE9F0U);
    const auto list = static_cast<uint32_t>(MEM_W(0, 0x800A38E4U));
    if (!tribal_metadata.ready || tribal_metadata.memory != rdram ||
        tribal_metadata.level != level || tribal_metadata.list != list ||
        tribal_metadata.count != count) {
        tribal_metadata = {rdram, level, list, count, 0, true, false};
        if (count < 0 || count > (8 * 1024 * 1024) / 4 ||
            (count != 0 && !readable(list, static_cast<uint32_t>(count) * 4, 4))) {
            return result;
        }
        for (int index = 0; index < count; ++index) {
            const auto object = static_cast<uint32_t>(MEM_W(index * 4, list));
            if (!readable(object, 0x6C, 4)) return result;
            const auto squad = static_cast<uint32_t>(MEM_W(0x68, object));
            if (!readable(squad, 0x65, 4)) return result;
            if (jfc::modernization::is_tribal_actor_type(MEM_HU(0x2C, squad))) {
                tribal_metadata.mask |= static_cast<uint16_t>(1U << (MEM_BU(0x64, squad) & 15));
            }
        }
        tribal_metadata.valid = true;
    }
    if (!tribal_metadata.valid) return result;
    result.room_tribals = tribals(tribal_metadata.mask,
        MEM_HU(0x5C + region * 2, save),
        MEM_HU(0xDC + region * 2, save));
    result.tribals_valid = true;
    return result;
}

namespace {
jfc::hud::AreaPickups scan_area_definitions(uint8_t* rdram) {
    jfc::hud::AreaPickups result;
    const auto& native=jfc::native_data::get();
    const auto merge=[&](jfc::native_data::PickupMask mask) {
        result.ship_parts |= mask.parts; result.keys |= mask.keys; result.floyd_parts |= mask.floyd;
    };
    for (unsigned slot=0;slot<2;++slot) {
        const auto list = uint32_t(MEM_W(slot*4,0x800F2CD8U));
        const auto bytes = uint32_t(MEM_W(slot*4,0x800F2CE0U));
        if (bytes==0) continue;
        if (bytes>0x3000-16 || !readable(list,bytes,2)) return {};

        for (uint32_t offset=0;offset<bytes;) {
            if (bytes-offset<10) return {};
            const uint32_t record=list+offset;
            const unsigned size=MEM_BU(2,record), actor=MEM_HU(0,record);
            if (size<10 || (size&1) || size>bytes-offset) return {};
            if (actor<native.actor_pickups.size()) {
                const auto& pickup=native.actor_pickups[actor];
                merge(pickup.direct);
                if (pickup.selector!=jfc::native_data::RewardSelector::None) {
                    if (size<20) return {};
                    if (pickup.selector==jfc::native_data::RewardSelector::Chest)
                        merge(native.chest_rewards[MEM_BU(0xA,record)]);
                    else merge(native.dialogue_rewards[MEM_BU(0xE,record)]);
                }
            }
            offset+=size;
        }
    }
    if (MEM_HU(0,0x800A323CU)==native.event_level) merge(native.event_reward);
    result.valid = true;
    return result;
}
}

jfc::hud::AreaPickups jfc::hud::read_area_pickups(uint8_t* rdram, uint32_t character_save) {
    const auto save = uint32_t(MEM_W(0,0x800FD7D4U));
    if (!readable(save,0x36) || !readable(character_save,0x69)) return {};
    const std::array<uint32_t,5> identity{
        uint32_t(MEM_W(0,0x800F2CD8U)), uint32_t(MEM_W(0,0x800F2CE0U)),
        uint32_t(MEM_W(4,0x800F2CD8U)), uint32_t(MEM_W(4,0x800F2CE0U)),
        uint32_t(MEM_HU(0,0x800A323CU))};
    if (!area_metadata.ready || area_metadata.memory != rdram || area_metadata.identity != identity) {
        area_metadata = {rdram, identity, scan_area_definitions(rdram), true};
    }
    AreaPickups result = area_metadata.definitions;
    if (!result.valid) return result;

    for (unsigned part=0;part<12;++part)
        if (MEM_BU(0x30+(32+part)/8,save)&(1U<<((32+part)&7)))
            result.ship_parts &= uint16_t(~(1U<<part));
    for (unsigned key=0;key<18;++key)
        if (MEM_BU(0x66+key/8,character_save)&(0x80U>>(key&7)))
            result.keys &= ~(1U<<key);
    result.floyd_parts &= uint8_t(~(MEM_BU(0x35,save)>>4));
    result.valid=true;
    return result;
}
