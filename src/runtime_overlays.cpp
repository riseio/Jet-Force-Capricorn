#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include "runtime_overlay_state.hpp"

#include "jfc_recomp.h"

extern "C" void load_overlays(uint32_t rom, int32_t ram, uint32_t size);
extern "C" void unload_overlays(int32_t ram, uint32_t size);

using namespace jfc::overlay_runtime;
std::unordered_map<uint32_t, OverlayLocation> jfc::overlay_runtime::loaded_overlays;

namespace {

constexpr uint32_t overlay_table_ptr = 0x800FEAA0U;
constexpr uint32_t overlay_count_addr = 0x800FEAACU;
constexpr uint32_t overlay_header_size = 0x20U;
constexpr uint32_t max_overlays = 158U;
constexpr uint32_t rdram_size = 8U * 1024U * 1024U;
constexpr uint32_t rom_size = 32U * 1024U * 1024U;

bool valid_rdram(uint32_t address, uint32_t size) {
    const uint32_t segment = address & 0xE0000000U;
    const uint32_t physical = address & 0x1FFFFFFFU;
    return (segment == 0x80000000U || segment == 0xA0000000U) &&
           size <= rdram_size && physical <= rdram_size - size;
}

uint32_t read_rdram_word(const uint8_t* rdram, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, rdram + (address & 0x1FFFFFFFU), sizeof(value));
    return value;
}

void write_rdram_word(uint8_t* rdram, uint32_t address, uint32_t value) {
    std::memcpy(rdram + (address & 0x1FFFFFFFU), &value, sizeof(value));
}

bool find_overlay(uint8_t* rdram, uint32_t ram, OverlayLocation& output) {
    const uint32_t count = read_rdram_word(rdram, overlay_count_addr);
    const uint32_t table = read_rdram_word(rdram, overlay_table_ptr);
    if (count < 2 || count > max_overlays ||
        !valid_rdram(table, count * overlay_header_size)) {
        return false;
    }

    for (uint32_t module = 1; module < count; ++module) {
        const uint32_t header = table + module * overlay_header_size;
        if (read_rdram_word(rdram, header) != ram) {
            continue;
        }

        const uint32_t rom = read_rdram_word(rdram, header + 4U);
        const uint32_t text = read_rdram_word(rdram, header + 8U);
        const uint32_t data = read_rdram_word(rdram, header + 12U);
        const uint32_t rodata = read_rdram_word(rdram, header + 16U);
        const uint32_t relocation_sizes =
            read_rdram_word(rdram, header + 20U);
        const int32_t init = static_cast<int32_t>(
            read_rdram_word(rdram, header + 24U));
        const int32_t resume = static_cast<int32_t>(
            read_rdram_word(rdram, header + 28U));
        if (text == 0 || text > rom_size || data > rom_size - text ||
            rom >= rom_size || text + data > rom_size - rom ||
            !valid_rdram(ram, text + data)) {
            std::fprintf(stderr, "Invalid JFG overlay %u metadata\n", module);
            std::exit(EXIT_FAILURE);
        }
        output = {
            module, rom, ram, text, data, rodata, relocation_sizes,
            init, text + data, resume
        };
        return true;
    }
    return false;
}

void register_overlay(const OverlayLocation& overlay) {
    const uint64_t loaded_end =
        static_cast<uint64_t>(overlay.ram) + overlay.stored_size;
    for (auto previous = loaded_overlays.begin();
         previous != loaded_overlays.end();) {
        const uint64_t previous_end =
            static_cast<uint64_t>(previous->second.ram) +
            previous->second.stored_size;
        if (previous->second.module == overlay.module ||
            (previous->second.ram < loaded_end &&
             overlay.ram < previous_end)) {
            unload_overlays(static_cast<int32_t>(previous->second.ram),
                            previous->second.stored_size);
            previous = loaded_overlays.erase(previous);
        } else {
            ++previous;
        }
    }
    load_overlays(overlay.rom, static_cast<int32_t>(overlay.ram),
                  overlay.stored_size);
    loaded_overlays[overlay.ram] = overlay;
}

}

extern "C" void osInvalICache_recomp(uint8_t* rdram, recomp_context* ctx) {
    OverlayLocation overlay{};
    const uint32_t ram = static_cast<uint32_t>(ctx->r4);
    const uint32_t requested_text_size = static_cast<uint32_t>(ctx->r5);
    if (find_overlay(rdram, ram, overlay) &&
        requested_text_size == overlay.text_size) {
        register_overlay(overlay);
    }
}

extern "C" void jfc_register_resumed_overlay(uint8_t* rdram,
                                                recomp_context* ctx) {
    const uint32_t count = read_rdram_word(rdram, overlay_count_addr);
    const uint32_t table = read_rdram_word(rdram, overlay_table_ptr);
    const uint32_t module = static_cast<uint32_t>(ctx->r23);
    if (module == 0 || module >= count || count > max_overlays ||
        !valid_rdram(table, count * overlay_header_size)) {
        return;
    }

    OverlayLocation overlay{};
    const uint32_t ram =
        read_rdram_word(rdram, table + module * overlay_header_size);
    if (ram != 0 && find_overlay(rdram, ram, overlay) &&
        overlay.module == module) {
        register_overlay(overlay);
    }
}

extern "C" uint32_t jfc_restore_registered_runlink_header(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t count = read_rdram_word(rdram, overlay_count_addr);
    const uint32_t table = read_rdram_word(rdram, overlay_table_ptr);
    const uint32_t module = static_cast<uint32_t>(ctx->r4);
    if (count < 2U || count > max_overlays || module == 0U ||
        module >= count ||
        !valid_rdram(table, count * overlay_header_size)) {

        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }

    const OverlayLocation* registered = nullptr;
    for (const auto& entry : loaded_overlays) {
        if (entry.second.module == module) {
            registered = &entry.second;
            break;
        }
    }

    if (registered == nullptr) {
        return 0U;
    }

    const uint32_t header = table + module * overlay_header_size;
    const uint32_t old_base = read_rdram_word(rdram, header);
    const uint32_t old_rom = read_rdram_word(rdram, header + 4U);
    const uint32_t old_text = read_rdram_word(rdram, header + 8U);
    const uint32_t old_data = read_rdram_word(rdram, header + 12U);
    const uint32_t old_rodata = read_rdram_word(rdram, header + 16U);
    const uint32_t old_relocations = read_rdram_word(rdram, header + 20U);
    const uint32_t old_init = read_rdram_word(rdram, header + 24U);
    const uint32_t old_resume = read_rdram_word(rdram, header + 28U);

    uint32_t changed = 0U;
    const auto restore = [&](uint32_t bit, uint32_t offset,
                             uint32_t observed, uint32_t expected) {
        if (observed != expected) {
            changed |= bit;
            write_rdram_word(rdram, header + offset, expected);
        }
    };

    if (old_base != 0U) {
        restore(1U << 0, 0U, old_base, registered->ram);
    }
    restore(1U << 1, 4U, old_rom, registered->rom);
    restore(1U << 2, 8U, old_text, registered->text_size);
    restore(1U << 3, 12U, old_data, registered->data_size);
    restore(1U << 4, 16U, old_rodata, registered->rodata_size);
    restore(1U << 5, 20U, old_relocations,
            registered->relocation_sizes);
    restore(1U << 6, 24U, old_init,
            static_cast<uint32_t>(registered->init_offset));
    restore(1U << 7, 28U, old_resume,
            static_cast<uint32_t>(registered->resume_offset));

    if (changed != 0U) {

        std::fflush(stderr);
    }

    return 1U;
}

extern "C" uint32_t jfc_validate_runlink_resume_target(
    uint8_t* rdram,
    recomp_context* ctx
) {
    const uint32_t count = read_rdram_word(rdram, overlay_count_addr);
    const uint32_t table = read_rdram_word(rdram, overlay_table_ptr);
    const uint32_t module = static_cast<uint32_t>(ctx->r5);
    const uint32_t observed_header = static_cast<uint32_t>(ctx->r7);
    const uint32_t observed_base = static_cast<uint32_t>(ctx->r6);
    const int32_t observed_resume = static_cast<int32_t>(ctx->r8);
    const uint32_t observed_target = static_cast<uint32_t>(ctx->r25);
    const uint32_t caller = static_cast<uint32_t>(ctx->r31);

    bool table_valid = count >= 2U && count <= max_overlays &&
        valid_rdram(table, count * overlay_header_size);
    uint32_t expected_header = 0U;
    bool header_valid = false;
    uint32_t rom = 0U;
    uint32_t text = 0U;
    uint32_t data = 0U;
    uint32_t rodata = 0U;
    int32_t init = -1;
    int32_t resume = -1;
    if (table_valid && module > 0U && module < count) {
        expected_header = table + module * overlay_header_size;
        header_valid = expected_header == observed_header;
        rom = read_rdram_word(rdram, expected_header + 4U);
        text = read_rdram_word(rdram, expected_header + 8U);
        data = read_rdram_word(rdram, expected_header + 12U);
        rodata = read_rdram_word(rdram, expected_header + 16U);
        init = static_cast<int32_t>(
            read_rdram_word(rdram, expected_header + 24U));
        resume = static_cast<int32_t>(
            read_rdram_word(rdram, expected_header + 28U));
    }

    const bool sizes_valid = text > 0U && text <= rom_size &&
        data <= rom_size - text && rodata <= rom_size - text - data &&
        rom < rom_size && text + data <= rom_size - rom;
    const bool base_valid = sizes_valid &&
        valid_rdram(observed_base, text + data);
    const bool resume_valid = resume >= 0 &&
        static_cast<uint32_t>(resume) < text &&
        (static_cast<uint32_t>(resume) & 3U) == 0U;
    const bool operands_match = observed_resume == resume &&
        observed_target == observed_base + static_cast<uint32_t>(resume);

    const OverlayLocation* registered = nullptr;
    for (const auto& entry : loaded_overlays) {
        if (entry.second.module == module) {
            registered = &entry.second;
            break;
        }
    }
    const bool registration_valid = registered != nullptr &&
        registered->ram == observed_base &&
        registered->text_size == text &&
        registered->resume_offset == resume;
    const bool valid = table_valid && header_valid && base_valid &&
        resume_valid && operands_match && registration_valid;

    if (!valid) {
        std::fprintf(
            stderr,
            "JFC_RUNLINK_RESUME_INVALID module=%u count=%u table=0x%08X "
            "header=0x%08X expected_header=0x%08X base=0x%08X "
            "resume=0x%08X target=0x%08X caller=0x%08X rom=0x%08X "
            "text=0x%08X data=0x%08X rodata=0x%08X init=0x%08X "
            "registered=%u registered_base=0x%08X "
            "registered_text=0x%08X registered_resume=0x%08X\n",
            module, count, table, observed_header, expected_header,
            observed_base, static_cast<uint32_t>(observed_resume),
            observed_target, caller, rom, text, data, rodata,
            static_cast<uint32_t>(init), registered != nullptr ? 1U : 0U,
            registered != nullptr ? registered->ram : 0U,
            registered != nullptr ? registered->text_size : 0U,
            registered != nullptr
                ? static_cast<uint32_t>(registered->resume_offset)
                : 0xFFFFFFFFU
        );
        std::fflush(stderr);
    }
    return valid ? 1U : 0U;
}

extern "C" void jfc_unregister_overlay(uint8_t* rdram, recomp_context*) {
    const uint32_t count = read_rdram_word(rdram, overlay_count_addr);
    const uint32_t table = read_rdram_word(rdram, overlay_table_ptr);
    if (count < 2 || count > max_overlays ||
        !valid_rdram(table, count * overlay_header_size)) {
        return;
    }

    for (auto overlay = loaded_overlays.begin();
         overlay != loaded_overlays.end();) {
        const uint32_t module = overlay->second.module;
        if (module >= count ||
            read_rdram_word(rdram, table + module * overlay_header_size) != 0) {
            ++overlay;
            continue;
        }
        unload_overlays(static_cast<int32_t>(overlay->second.ram),
                        overlay->second.stored_size);
        overlay = loaded_overlays.erase(overlay);
    }
}
