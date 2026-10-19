#pragma once
#include "texture_reconstruction.hpp"
namespace jfc::textures {

Image reconstruct_font(const Image& source);
Image reconstruct_number_sheet(const Image& source, uint32_t cell_width);
std::vector<uint8_t> make_font_pack(std::span<const uint8_t> validated_rom);
std::filesystem::path prepare_font_pack(std::span<const uint8_t> validated_rom,
    const std::filesystem::path& application_folder);
}
