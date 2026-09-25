#pragma once

#include <cstdint>
#include <filesystem>

namespace jfc::crash_reporter {

void install(const std::filesystem::path& app_folder);
void set_vi(uint64_t vi) noexcept;
void set_function_lookup_failure(int32_t target, const char* caller) noexcept;

}
