#pragma once
#include <cstdint>
#include <filesystem>

namespace jfc::textures {
inline constexpr uint64_t pack_size = 1970530095ULL;
inline constexpr uint64_t pack_hash = 0x4cf50c0f03f077e8ULL;
std::filesystem::path pack_path(const std::filesystem::path& folder);
void validate_pack_location(const std::filesystem::path& folder);

std::filesystem::path installed_pack(const std::filesystem::path& folder);

void record_pack_installation(const std::filesystem::path& folder);
void remove_pack(const std::filesystem::path& folder);
}
