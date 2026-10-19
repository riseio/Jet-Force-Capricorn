#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace jfc::local_aot {

std::vector<uint8_t> reconstruct_image(std::span<const uint8_t> recipe,
                                     std::span<const uint8_t> validated_rom);
}
