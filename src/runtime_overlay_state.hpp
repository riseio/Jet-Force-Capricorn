#pragma once

#include <cstdint>
#include <unordered_map>

namespace jfc::overlay_runtime {
struct OverlayLocation {
    uint32_t module;
    uint32_t rom;
    uint32_t ram;
    uint32_t text_size;
    uint32_t data_size;
    uint32_t rodata_size;
    uint32_t relocation_sizes;
    int32_t init_offset;
    uint32_t stored_size;
    int32_t resume_offset;
};

extern std::unordered_map<uint32_t, OverlayLocation> loaded_overlays;

}
