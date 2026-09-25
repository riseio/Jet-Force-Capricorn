#include "save_state_overlays.hpp"
#include "save_state_codec.hpp"
#include "runtime_overlay_state.hpp"
#include "librecomp/overlays.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

using namespace jfc::overlay_runtime;
namespace {
constexpr uint32_t max_overlays = 158U;
constexpr uint32_t rdram_size = 8U * 1024U * 1024U;
constexpr uint32_t rom_size = 32U * 1024U * 1024U;
bool valid_rdram(uint32_t address, uint32_t size) {
    return address >= 0x80000000U && uint64_t(address) + size <= 0x80800000ULL;
}
}

struct jfc::state::PreparedOverlays::Impl {
    std::unordered_map<uint32_t, OverlayLocation> registrations;
    recomp::overlays::PreparedState dispatch;
    bool committed = false;
    explicit Impl(recomp::overlays::PreparedState&& prepared)
        : dispatch(std::move(prepared)) {}
};

jfc::state::Bytes jfc::state::capture_overlays() {
    const auto sections = recomp::overlays::capture_state(rdram_size);
    Writer out;
    out.u32(1);
    out.u32(uint32_t(sections.size()));
    for (const auto& section : sections) {
        out.u32(section.rom); out.u32(section.ram); out.u32(section.size);
    }
    std::vector<OverlayLocation> registrations;
    registrations.reserve(loaded_overlays.size());
    for (const auto& [address, overlay] : loaded_overlays) {
        if (address != overlay.ram) throw std::runtime_error("Live runlink key mismatch");
        registrations.push_back(overlay);
    }
    std::sort(registrations.begin(), registrations.end(),
        [](const auto& a, const auto& b) { return a.module < b.module; });
    out.u32(uint32_t(registrations.size()));
    for (const auto& overlay : registrations) {
        out.u32(overlay.module); out.u32(overlay.rom); out.u32(overlay.ram);
        out.u32(overlay.text_size); out.u32(overlay.data_size); out.u32(overlay.rodata_size);
        out.u32(overlay.relocation_sizes); out.u32(uint32_t(overlay.init_offset));
        out.u32(overlay.stored_size); out.u32(uint32_t(overlay.resume_offset));
    }

    PreparedOverlays checked(out.bytes);
    return std::move(out.bytes);
}

jfc::state::PreparedOverlays::PreparedOverlays(std::span<const uint8_t> saved) {

    Reader in(saved);
    if (in.u32() != 1) throw std::runtime_error("Unsupported overlay state schema");
    const uint32_t count = in.u32();

    if (count > max_overlays || count > saved.size() / 12U) {
        throw std::runtime_error("Invalid save-state compiled overlay count");
    }
    std::vector<recomp::overlays::SectionBinding> sections;
    sections.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto rom = in.u32(), ram = in.u32(), size = in.u32();
        sections.push_back({rom, ram, size});
    }
    auto prepared = std::make_unique<Impl>(recomp::overlays::prepare_state(sections, rdram_size));
    const uint32_t registrations = in.u32();
    if (registrations >= max_overlays || registrations > saved.size() / 40U) {
        throw std::runtime_error("Invalid save-state runlink count");
    }
    uint32_t previous_module = 0;
    for (uint32_t i = 0; i < registrations; ++i) {
        OverlayLocation overlay{};
        overlay.module = in.u32(); overlay.rom = in.u32(); overlay.ram = in.u32();
        overlay.text_size = in.u32(); overlay.data_size = in.u32(); overlay.rodata_size = in.u32();
        overlay.relocation_sizes = in.u32(); overlay.init_offset = std::bit_cast<int32_t>(in.u32());
        overlay.stored_size = in.u32(); overlay.resume_offset = std::bit_cast<int32_t>(in.u32());
        const auto valid_function_offset = [&](int32_t offset) {
            return offset == -1 || (offset >= 0 && uint32_t(offset) < overlay.text_size && (offset & 3) == 0);
        };
        if (overlay.module <= previous_module || overlay.module >= max_overlays ||
            overlay.text_size == 0 || overlay.text_size > rom_size ||
            overlay.data_size > rom_size - overlay.text_size ||
            overlay.rodata_size > rom_size - overlay.text_size - overlay.data_size ||
            overlay.stored_size != overlay.text_size + overlay.data_size ||
            overlay.rom >= rom_size || overlay.stored_size > rom_size - overlay.rom ||
            !valid_rdram(overlay.ram, overlay.stored_size) || (overlay.ram & 3U) != 0 ||
            !valid_function_offset(overlay.init_offset) || !valid_function_offset(overlay.resume_offset)) {
            throw std::runtime_error("Invalid save-state runlink metadata");
        }
        previous_module = overlay.module;
        const auto section = std::find_if(sections.begin(), sections.end(), [&](const auto& entry) {

            return entry.rom == overlay.rom && entry.ram == overlay.ram && entry.size == overlay.stored_size;
        });
        if (section == sections.end()) {
            throw std::runtime_error("Save-state runlink has no matching compiled dispatch section");
        }
        for (const auto& [address, prior] : prepared->registrations) {
            if (uint64_t(overlay.ram) < uint64_t(address) + prior.stored_size &&
                uint64_t(address) < uint64_t(overlay.ram) + overlay.stored_size) {
                throw std::runtime_error("Save-state runlink allocations overlap");
            }
        }
        if (!prepared->registrations.emplace(overlay.ram, overlay).second) {
            throw std::runtime_error("Duplicate save-state runlink address");
        }
    }
    in.end();
    impl = std::move(prepared);
}
jfc::state::PreparedOverlays::~PreparedOverlays() = default;
jfc::state::PreparedOverlays::PreparedOverlays(PreparedOverlays&&) noexcept = default;
jfc::state::PreparedOverlays& jfc::state::PreparedOverlays::operator=(PreparedOverlays&&) noexcept = default;
void jfc::state::PreparedOverlays::commit() noexcept {
    if (!impl || impl->committed) std::terminate();
    impl->dispatch.commit();
    loaded_overlays.swap(impl->registrations);
    impl->committed = true;
}
