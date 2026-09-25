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
}
