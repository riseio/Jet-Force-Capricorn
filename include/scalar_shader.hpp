#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace jfc::shader {
struct CodeSpan { size_t offset, size; };

std::string scalar_block(std::span<const uint8_t> bytes, std::span<const CodeSpan> spans,
    const std::array<std::string,32>& inputs, unsigned output);
std::string cpu_effect_source(std::span<const uint8_t> validated_rom, std::string_view source_template);
}
