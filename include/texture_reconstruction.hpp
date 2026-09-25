#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <span>
#include <stop_token>
#include <exception>
#include <vector>
namespace jfc::textures {
struct Image { uint32_t width{}, height{}; std::vector<uint8_t> rgba; };
struct PreparationCancelled final : std::exception {
    const char* what() const noexcept override {return "Texture preparation cancelled by application shutdown";}
};

Image reconstruct(const Image& source, uint32_t scale = 9);

Image restore_detail(const Image& source);

Image clean_grain(const Image& source);

Image filter_surface(const Image& reconstructed, uint32_t texel_scale = 9);

Image reconstruct_material(const Image& source);
Image decode_tile(std::span<const uint8_t> tile);
std::vector<uint8_t> make_dds(const Image& image);

std::vector<uint8_t> make_pack(std::span<const uint8_t> validated_rom);

void write_pack(std::span<const uint8_t> validated_rom, FILE* output,
    const std::function<void(size_t)>& progress = {}, std::stop_token shutdown = {});
std::filesystem::path prepare_pack(std::span<const uint8_t> validated_rom,
    const std::filesystem::path& application_folder,
    const std::function<void(size_t)>& progress = {}, std::stop_token shutdown = {});
}
