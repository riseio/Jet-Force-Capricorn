#pragma once
#include "texture_reconstruction.hpp"
#include <functional>
#include <array>

namespace jfc::textures {
struct BankTile {
    uint32_t bank{}, index{}, frame{}, level{};
    uint64_t hash{};

    bool self_contained{};
    Image image;
    std::array<uint8_t,32> original_header{};

    std::span<const uint8_t> original_frame;
};

void visit_bank_tiles(std::span<const uint8_t> rom,
    const std::function<void(BankTile&&)>& consume);
struct FontTile {
    uint32_t font{}, glyph{};
    uint64_t hash{};
    Image image;
    std::array<uint8_t,16> definition{};
    std::span<const uint8_t> pixels;
};

void visit_font_tiles(std::span<const uint8_t> rom,
    const std::function<void(FontTile&&)>& consume);

void visit_number_tiles(std::span<const uint8_t> rom,
    const std::function<void(BankTile&&, uint32_t cell_width)>& consume);
}
