#include "texture_installation.hpp"
#include "atomic_file.hpp"
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

namespace jfc::textures {
namespace {
std::filesystem::path receipt_path(const std::filesystem::path& folder) {
    return folder / "cache" / "textures" / "coordinate-v7.installed";
}

bool owned_paths(const std::filesystem::path& folder) {
    for (const auto& path : {folder / "cache", folder / "cache" / "textures",
                            pack_path(folder), receipt_path(folder)}) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory) return false;
        if (std::filesystem::is_symlink(status)) return false;
        if (std::filesystem::exists(status) &&
            !(path == pack_path(folder) || path == receipt_path(folder)
                ? std::filesystem::is_regular_file(status)
                : std::filesystem::is_directory(status))) return false;
    }
    return true;
}

std::string receipt(const std::filesystem::path& path) {
    return "JFC textures v7\n1970530095\n4cf50c0f03f077e8\n" +
        std::to_string(std::filesystem::last_write_time(path).time_since_epoch().count()) + "\n";
}
}

std::filesystem::path pack_path(const std::filesystem::path& folder) {
    return folder / "cache" / "textures" / "coordinate-v7.rtz";
}

void validate_pack_location(const std::filesystem::path& folder) {
    if (!owned_paths(folder))
        throw std::runtime_error("Enhanced texture cache must contain regular files and directories, not links");
}

std::filesystem::path installed_pack(const std::filesystem::path& folder) {
    const auto path = pack_path(folder);
    if (!owned_paths(folder)) return {};
    std::error_code error;
    if (std::filesystem::file_size(path, error) != pack_size || error) return {};

    try {
        std::ifstream input(receipt_path(folder), std::ios::binary);
        std::array<char, 256> bytes{};
        input.read(bytes.data(), bytes.size());
        if (!input.eof() || input.bad()) return {};
        if (std::string_view(bytes.data(), size_t(input.gcount())) == receipt(path)) return path;
    } catch (const std::filesystem::filesystem_error&) {
        return {};
    }
    return {};
}

void record_pack_installation(const std::filesystem::path& folder) {
    if (!owned_paths(folder) || std::filesystem::file_size(pack_path(folder)) != pack_size)
        throw std::runtime_error("Enhanced texture installation path is not a regular local pack");
    const auto text = receipt(pack_path(folder));
    const auto result = files::write_atomic(receipt_path(folder),
        {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    if (!result.durable) std::fprintf(stderr, "Enhanced textures: %s\n", result.warning.c_str());
}

void remove_pack(const std::filesystem::path& folder) {
    validate_pack_location(folder);

    std::filesystem::remove(receipt_path(folder));
    std::filesystem::remove(pack_path(folder));
}
}
