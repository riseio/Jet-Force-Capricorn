#include "jfc_recomp.h"
#include "funcs.h"
#include "local_aot.hpp"
#include "local_aot_metadata.hpp"
#include "tribal_classification.hpp"

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
#include "texture_installation.hpp"
#include "texture_bank.hpp"
#include "font_textures.hpp"
#include "recompui/renderer.h"
#include "recompui/recompui.h"
#include "util/file.h"

void jfc::local_aot::register_loaded_overlays() {
    const auto& native = module();
    jfc::modernization::initialize_tribal_classifier(squadsIsTribal);
    recomp::overlays::register_overlays(
        {
            .code_sections = native.sections,
            .num_code_sections = native.section_count,
            .total_num_sections = native.total_sections,
        },
        {
            .table = native.overlays,
            .len = native.overlay_count,
        });
}

void jfc_load_bootstrap_overlay_data(uint8_t* rdram, recomp_context* context) {
    jfc_initialize_boot_environment(rdram, context);
    recompui::renderer::set_base_texture_pack_paths({"assets/branding",
        jfc::textures::prepare_font_pack(recomp::get_rom(),recompui::file::get_app_folder_path())});
    const auto& native = jfc::local_aot::module();
    for (const SectionTableEntry& section : std::span(native.sections,native.section_count)) {
        for (size_t index = 0; index < section.num_funcs; ++index) {
            if (section.funcs[index].func != native.functions[jfc::local_aot::kBootstrapFunctionIndex]) {
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
