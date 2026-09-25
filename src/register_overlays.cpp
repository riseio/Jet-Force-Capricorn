#include "recomp_overlays.inl"

#include <stdexcept>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <csignal>
#include <cstdlib>

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include "texture_reconstruction.hpp"
#include "texture_bank.hpp"
#include "recompui/renderer.h"
#include "recompui/recompui.h"
#include "util/file.h"

void jfc_register_overlays() {
    recomp::overlays::register_overlays(
        {
            .code_sections = section_table,
            .num_code_sections = ARRLEN(section_table),
            .total_num_sections = num_sections,
        },
        {
            .table = overlay_sections_by_index,
            .len = ARRLEN(overlay_sections_by_index),
        });
}

void jfc_load_bootstrap_overlay_data(uint8_t* rdram, recomp_context* context) {
    jfc_initialize_boot_environment(rdram, context);
    recompui::renderer::set_base_texture_pack_path("assets/branding");
    try {
        recompui::open_notification("Enhanced textures", "Checking the local texture cache...");
        struct CloseNotice {~CloseNotice(){recompui::close_prompt();}} notice;
        recompui::renderer::set_builtin_texture_pack_path(jfc::textures::prepare_pack(
            recomp::get_rom(), recompui::file::get_app_folder_path(), [](size_t count) {
                recompui::open_notification("Preparing enhanced textures",
                    "Cleaning the original textures: " + std::to_string(count) +
                    " / 9358 variants processed. This one-time step can take several minutes. "
                    "Please keep the game open; later launches reuse this cache.");
            }, ultramodern::shutdown_token()));
    } catch (const jfc::textures::PreparationCancelled&) {

        return;
    }
    for (const SectionTableEntry& section : section_table) {
        for (size_t index = 0; index < section.num_funcs; ++index) {
            if (section.funcs[index].func != mainInitRlo) {
                continue;
            }

            const auto rom = recomp::get_rom();
            if (section.rom_addr > rom.size() ||
                section.size > rom.size() - section.rom_addr) {
                throw std::runtime_error("Bootstrap overlay is outside the selected ROM");
            }
            recomp::do_rom_read(
                rdram,
                static_cast<uint32_t>(section.ram_addr),
                recomp::rom_base + section.rom_addr,
                section.size);
            return;
        }
    }
    throw std::runtime_error("Bootstrap overlay metadata is missing");
}
