#include "jfc_recomp.h"
#include "title_logo_data.hpp"
#include <array>
#include <stdexcept>

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("Title logo native asset layout is invalid");
}
bool readable(uint32_t address, uint32_t bytes, uint32_t alignment = 4) {
    return (address & 0xE0000000U) == 0x80000000U && address % alignment == 0 &&
        bytes <= 0x800000U && (address & 0x1FFFFFFFU) <= 0x800000U - bytes;
}
uint32_t physical_pointer(uint32_t value) {
    require(value < 0x800000U);
    return value | 0x80000000U;
}
}

extern "C" void jfc_install_title_logo(uint8_t* rdram) {

    const uint32_t sprite = MEM_W(148 * 4, 0x800FF3F8U);
    require(readable(sprite, 28));
    require(MEM_BU(0, sprite) == 1 && MEM_H(2, sprite) == 17 &&
        !(MEM_H(6, sprite) & 0x40));
    const uint32_t table = MEM_W(16, sprite), frame = MEM_W(24, sprite);
    require(readable(table, 17 * 4) && readable(frame, 40, 8));
    const uint32_t texture = MEM_W(0, table);
    require(readable(texture, 32 + jfc::branding::pixels.size()));
    require(MEM_BU(0, texture) == 32 && MEM_BU(1, texture) == 13 &&
        (MEM_BU(2, texture) & 15) == 0 && MEM_BU(18, texture) == 1);

    require((uint32_t(MEM_W(0, frame)) >> 24) == 7 &&
        (uint32_t(MEM_W(8, frame)) >> 24) == 4 &&
        uint32_t(MEM_W(16, frame)) == 0x05110020U);
    const uint32_t vertices = physical_pointer(MEM_W(12, frame));
    const uint32_t triangles = physical_pointer(MEM_W(20, frame));
    require(readable(vertices, 40, 2) && readable(triangles, 32, 8));
    require(MEM_BU(0, triangles) == 0x40 && MEM_BU(1, triangles) == 3 &&
        MEM_BU(2, triangles) == 2 && MEM_BU(3, triangles) == 1 &&
        MEM_BU(16, triangles) == 0x40 && MEM_BU(17, triangles) == 4 &&
        MEM_BU(18, triangles) == 3 && MEM_BU(19, triangles) == 1);
    for (size_t i = 0; i < jfc::branding::pixels.size(); ++i)
        MEM_B(32 + i, texture) = jfc::branding::pixels[i];

    constexpr std::array<std::array<int16_t, 2>, 4> corners{{
        {-126,42}, {126,42}, {126,-42}, {-126,-42}}};
    for (size_t i = 0; i < corners.size(); ++i) {
        MEM_H(i * 10, vertices) = corners[i][0];
        MEM_H(i * 10 + 2, vertices) = corners[i][1];
    }

    MEM_W(8, frame) = 0x04000000U | ((32U | (vertices & 6U)) << 16) | 0x230U;
    constexpr std::array<uint16_t, 12> uv{{1024,416, 1024,0, 0,0, 0,416, 1024,416, 0,0}};
    for (size_t i = 0; i < 6; ++i) MEM_H(4 + i * 2, triangles) = uv[i];
    for (size_t i = 0; i < 6; ++i) MEM_H(20 + i * 2, triangles) = uv[6 + i];
    MEM_W(24, frame) = 0xE7000000U; MEM_W(28, frame) = 0;
    MEM_W(32, frame) = 0xB8000000U; MEM_W(36, frame) = 0;
}
