#include "boss_health.hpp"
#include "jfc_recomp.h"
#include "native_rom_data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace {
std::atomic_bool half_health{false};
std::atomic_bool target_hud{false};
std::atomic_bool weak_point_effect{true};
std::atomic_bool mantis_final_vulnerability{true};
std::atomic_bool double_pistol_damage{true};
std::atomic_bool phase_ammo_refill{true};
constexpr uint32_t simulation_tick = 0x800FE9F8U;
constexpr unsigned part_stride = 0x34;
enum class Kind { None, FatBob, Slug, Mantis, Mizar };
struct PartBudget { unsigned part; unsigned definition; };
constexpr std::array<PartBudget,4> fat_bob{{{18,0},{20,1},{21,2},{22,3}}};
constexpr std::array<PartBudget,8> slug{{
    {0,4},{1,5},{2,6},{3,7},{4,8},{5,9},{7,10},{9,11}}};
int maximum(const PartBudget& part) { return jfc::native_data::get().boss_thresholds[part.definition]; }
struct AimResult {
    uint8_t* memory = nullptr;
    uint32_t player = 0, object = 0, hit = 0, part = 0, tick = 0;
} aim;

bool readable(uint32_t address, uint32_t size, unsigned alignment = 4) {
    const uint32_t segment = address & 0xE0000000U;
    const uint32_t offset = address & 0x1FFFFFFFU;
    return (segment == 0x80000000U || segment == 0xA0000000U) &&
        address % alignment == 0 && size <= 0x800000U && offset <= 0x800000U-size;
}

uint32_t overlay(unsigned section, uint32_t offset, uint32_t size) {
    const auto base = uint32_t(section_addresses[section]);
    if (offset > 0x800000U || size > 0x800000U-offset || !readable(base,offset+size)) return 0;
    return base+offset;
}

Kind kind(uint8_t* rdram, uint32_t object) {
    if (!readable(object,0x6C)) return Kind::None;
    switch (MEM_HU(0x48,object)) {
    case 0x82: return Kind::Mantis;
    case 0x6C: return Kind::Mizar;
    case 0x18: {
        const auto state = uint32_t(MEM_W(0x68,object));
        if (!readable(state,0xAC)) return Kind::None;
        const auto brain = uint32_t(MEM_W(0xA8,state));
        if (!brain) return Kind::None;
        if (brain == overlay(201,0x3278,0x58)) return Kind::FatBob;
        if (brain == overlay(209,0x2250,0x38)) return Kind::Slug;
        break;
    }
    }
    return Kind::None;
}

int part_budget(Kind owner, unsigned part) {
    if (owner == Kind::FatBob) for (const auto& p : fat_bob) if (p.part == part) return maximum(p);
    if (owner == Kind::Slug) for (const auto& p : slug) if (p.part == part) return maximum(p);
    return 0;
}

template<size_t N>
void initialize_counted_parts(uint8_t* rdram, uint32_t object, const std::array<PartBudget,N>& budgets) {
    if (!readable(object,0x6C) || MEM_HU(0x48,object) != 0x18)
        throw std::runtime_error("Invalid boss health initialization object");
    const auto state = uint32_t(MEM_W(0x68,object));
    if (!readable(state,0xAC)) throw std::runtime_error("Invalid boss state");
    if (MEM_W(0xA8,state) != 0) return;
    const auto hit = uint32_t(MEM_W(0x4C,object));
    if (!readable(hit,0x44)) throw std::runtime_error("Invalid boss hit header");
    const int count = MEM_H(0x32,hit);
    const auto parts = uint32_t(MEM_W(0x40,hit));
    if (count <= 0 || count > 256 || !readable(parts,unsigned(count)*part_stride))
        throw std::runtime_error("Invalid boss hit parts");
    for (const auto& p : budgets) if (p.part >= unsigned(count))
        throw std::runtime_error("Missing authored boss health part");
    for (const auto& p : budgets) {
        const auto address = parts+p.part*part_stride;

        MEM_H(0x2C,address) = std::max(int(MEM_H(0x2C,address)),maximum(p)/2);
    }
}
}

void jfc::boss::set_half_health(bool enabled) { half_health.store(enabled,std::memory_order_relaxed); }
void jfc::boss::set_target_hud(bool enabled) { target_hud.store(enabled,std::memory_order_relaxed); }
void jfc::boss::set_weak_point_effect(bool enabled) { weak_point_effect.store(enabled,std::memory_order_relaxed); }
void jfc::boss::set_mantis_final_vulnerability(bool enabled) { mantis_final_vulnerability.store(enabled,std::memory_order_relaxed); }
void jfc::boss::set_double_pistol_damage(bool enabled) { double_pistol_damage.store(enabled,std::memory_order_relaxed); }
void jfc::boss::set_phase_ammo_refill(bool enabled) { phase_ammo_refill.store(enabled,std::memory_order_relaxed); }
void jfc::boss::invalidate_target() noexcept { aim = {}; }

extern "C" int32_t jfc_boss_pistol_damage(uint8_t* rdram, uint32_t target, uint32_t source,
    uint32_t part, int32_t damage) {
    if (!double_pistol_damage.load(std::memory_order_relaxed) || damage <= 0 ||
        !readable(source,0x6C) || MEM_HU(0x48,source) != 0xA2) return damage;
    const auto shot = uint32_t(MEM_W(0x68,source));
    if (!readable(shot,0x7D) || MEM_BU(0x7C,shot) != 0) return damage;
    const auto owner = uint32_t(MEM_W(0x70,shot));
    if (!readable(owner,0x6C) || MEM_HU(0x48,owner) != 1 ||
        kind(rdram,target) == Kind::None || !readable(part,part_stride) ||
        MEM_BU(0x2F,part) == 255) return damage;

    const int room = INT16_MAX - std::max(0,int(MEM_H(0x2C,part)));
    return int32_t(std::min(int64_t(room),int64_t(damage)*2));
}

extern "C" void jfc_boss_part_completed(uint8_t* rdram, uint32_t object, unsigned part) {
    if (!phase_ammo_refill.load(std::memory_order_relaxed)) return;
    const auto owner = kind(rdram,object);
    if (!part_budget(owner,part) || (owner == Kind::Slug && part == 7)) return;
    const auto hit = uint32_t(MEM_W(0x4C,object));
    if (!readable(hit,0x44)) return;
    const int count = MEM_H(0x32,hit);
    const auto parts = uint32_t(MEM_W(0x40,hit));
    if (count <= 0 || count > 256 || part >= unsigned(count) ||
        !readable(parts,unsigned(count)*part_stride)) return;

    if (!(MEM_W(0x30,parts+part*part_stride) & 0x40000000U))
        jfc_refill_boss_phase_ammo(rdram);
}

extern "C" void jfc_boss_mantis_weapon_completed(uint8_t* rdram, uint32_t object, unsigned group) {
    if (!phase_ammo_refill.load(std::memory_order_relaxed) ||
        (group != 2 && group != 3 && group != 5) || kind(rdram,object) != Kind::Mantis) return;
    const auto state = uint32_t(MEM_W(0x68,object));
    if (readable(state,0x10) && !(MEM_BU(9+group,state)&1))
        jfc_refill_boss_phase_ammo(rdram);
}

extern "C" void jfc_boss_mizar_damage(uint8_t* rdram, uint32_t object, unsigned group, int32_t damage) {
    if (!phase_ammo_refill.load(std::memory_order_relaxed) || group < 2 || group > 6 ||
        kind(rdram,object) != Kind::Mizar) return;
    const auto groups = overlay(112,0x12F88,11*0x20);
    if (!groups) return;
    const int health = MEM_H(group*0x20+0x10,groups);
    if (health > 0 && damage >= health && MEM_B(group*0x20+0x13,groups))
        jfc_refill_boss_phase_ammo(rdram);
}

extern "C" uint32_t jfc_mantis_group_status(uint8_t* rdram, uint32_t object, uint32_t group, uint32_t status) {
    if (!mantis_final_vulnerability.load(std::memory_order_relaxed) || group != 1 ||
        (status & 1) || kind(rdram,object) != Kind::Mantis) return status;
    const auto state = uint32_t(MEM_W(0x68,object));
    const auto hit = uint32_t(MEM_W(0x4C,object));
    if (!readable(state,0x10) || !readable(hit,0x44)) return status;
    const unsigned phase = MEM_BU(1,state);

    if (!(MEM_BU(0xB,state) & MEM_BU(0xC,state) & MEM_BU(0xE,state) & 1) ||
        phase >= 0x16 || (phase >= 0x11 && phase <= 0x13) || MEM_H(6,hit) <= 0) return status;
    return status | 2U;
}

extern "C" int32_t jfc_boss_initial_health(int32_t health) {
    return health > 0 && half_health.load(std::memory_order_relaxed) ? health/2 + health%2 : health;
}

extern "C" void jfc_initialize_boss_parts(uint8_t* rdram, uint32_t object, int slug_boss) {
    if (!half_health.load(std::memory_order_relaxed)) return;
    if (slug_boss) initialize_counted_parts(rdram,object,slug);
    else initialize_counted_parts(rdram,object,fat_bob);
}

extern "C" void jfc_initialize_mizar_health(uint8_t* rdram) {
    if (!half_health.load(std::memory_order_relaxed)) return;
    const auto groups = overlay(112,0x12F88,11*0x20);
    if (!groups) throw std::runtime_error("Invalid Mizar health overlay");
    for (unsigned i=0;i<11;++i) {
        const auto health = groups+i*0x20+0x10;
        MEM_H(0,health) = jfc_boss_initial_health(MEM_H(0,health));
    }
}

extern "C" void jfc_begin_boss_target(uint8_t* rdram, uint32_t object) {
    if (!readable(object,0x6C)) return;
    const auto player = uint32_t(MEM_W(0x68,object));
    if (MEM_HU(0x48,object) == 1 && readable(player,2) && MEM_B(0,player) == 0)
        jfc::boss::invalidate_target();
}

extern "C" void jfc_observe_boss_target(uint8_t* rdram, recomp_context* ctx) {
    if (!target_hud.load(std::memory_order_relaxed) || int32_t(ctx->r2) <= 0) return;
    const auto player = uint32_t(ctx->r17);
    const auto collision = uint32_t(MEM_W(0x60,ctx->r29));
    if (!readable(player,2) || MEM_B(0,player) != 0 || !readable(collision,12)) return;
    const auto object = uint32_t(MEM_W(0,collision));
    if (kind(rdram,object) == Kind::None) return;
    aim = {rdram,player,object,uint32_t(MEM_W(4,collision)),uint32_t(MEM_W(8,collision)),
        uint32_t(MEM_W(0,simulation_tick))};
}

namespace {
jfc::boss::Target part_target(uint8_t* rdram, Kind owner, uint32_t object,
    uint32_t hit, uint32_t address, unsigned part, unsigned& region) {
    jfc::boss::Target result;
    region = 0;
    const uint32_t flags = MEM_W(0x30,address);
    result.vulnerable = MEM_BU(0x2F,address) != 0 && !(flags&0x60000000U);
    result.destroyed = (flags&0x40000000U) != 0;
    if (owner == Kind::FatBob || owner == Kind::Slug) {
        result.name = owner == Kind::FatBob ? "FET-BUBB" : "LURG";
        result.maximum = part_budget(owner,part);
        result.remaining = result.maximum-std::max(0,int(MEM_H(0x2C,address)));
        if (owner == Kind::FatBob) {
            for (unsigned i=0;i<fat_bob.size();++i) if (fat_bob[i].part == part) region = i;
        } else {
            for (unsigned i=0;i<slug.size();++i) if (slug[i].part == part) region = i;
        }
    } else if (owner == Kind::Mizar) {
        result.name = "MIZAR";
        const auto definitions = overlay(112,0x1254C,11*12);
        const auto groups = overlay(112,0x12F88,11*0x20);
        if (!definitions || !groups) return {};
        unsigned selected = 11;
        for (unsigned i=0;i<11;++i) {
            const auto definition = definitions+i*12;
            const int first = MEM_H(4,definition), last = MEM_H(6,definition);
            if (first >= 0 && int(part) >= first && int(part) <= last) {
                if (selected == 11) selected = i;

                if (MEM_BU(0x13,groups+i*0x20)) { selected = i; break; }
            }
        }
        if (selected != 11) {
            region = selected;
            result.maximum = std::max(0,int(MEM_H(8,definitions+selected*12)));
            result.remaining = MEM_H(0x10,groups+selected*0x20);
            result.vulnerable &= MEM_BU(0x13,groups+selected*0x20) != 0;
        }
    } else {
        result.name = "MECHANTIS";
        const auto state = uint32_t(MEM_W(0x68,object));
        const auto mapping = overlay(111,0x44F0,0x80);
        if (!readable(state,0x10) || !mapping || part >= 64) return {};
        const unsigned group = MEM_BU(part*2+1,mapping);
        if (group < 6) {
            region = group;
            const unsigned status = MEM_BU(9+group,state);
            result.destroyed |= (status&1) != 0;
            const auto& budgets = jfc::native_data::get().mantis_phase_budgets;
            result.maximum = budgets[(MEM_BU(0xE,state)&1) ? 2 : (MEM_BU(0xC,state)&1) ? 1 : 0];
            result.remaining = MEM_H(6,hit);
        }
    }
    if (result.maximum <= 0) result.vulnerable = false;
    result.remaining = result.destroyed ? 0 : std::clamp(result.remaining,0,result.maximum);
    result.destroyed |= result.maximum > 0 && result.remaining == 0;
    result.vulnerable &= !result.destroyed;
    return result;
}
}

jfc::boss::Target jfc::boss::current_target(uint8_t* rdram, uint32_t player) {
    if (!target_hud.load(std::memory_order_relaxed) || aim.memory != rdram ||
        aim.player != player || aim.tick != uint32_t(MEM_W(0,simulation_tick))) return {};
    const auto owner = kind(rdram,aim.object);
    if (owner == Kind::None || !readable(aim.hit,0x44) ||
        uint32_t(MEM_W(0x4C,aim.object)) != aim.hit) return {};
    const auto parts = uint32_t(MEM_W(0x40,aim.hit));
    const int count = MEM_H(0x32,aim.hit);
    if (count <= 0 || count > 256 || !readable(parts,unsigned(count)*part_stride) ||
        aim.part < parts || (aim.part-parts)%part_stride != 0) return {};
    const unsigned part = (aim.part-parts)/part_stride;
    if (part >= unsigned(count)) return {};
    unsigned region;
    return part_target(rdram,owner,aim.object,aim.hit,aim.part,part,region);
}

jfc::boss::WeakPoints jfc::boss::weak_points(uint8_t* rdram, uint32_t object) {
    WeakPoints result;
    if (!weak_point_effect.load(std::memory_order_relaxed)) return result;
    const auto owner = kind(rdram,object);
    if (owner == Kind::None || !readable(object,0x70) || (MEM_HU(6,object)&0xC00)) return result;
    const auto hit = uint32_t(MEM_W(0x4C,object));
    if (!readable(hit,0x44)) return result;
    const auto parts = uint32_t(MEM_W(0x40,hit));
    const int count = MEM_H(0x32,hit);
    if (count <= 0 || count > 256 || !readable(parts,unsigned(count)*part_stride)) return result;
    const unsigned model_index = MEM_BU(0x3A,object);
    const auto models = uint32_t(MEM_W(0x6C,object));
    if (!readable(models,(model_index+1)*4)) return result;
    const auto model = uint32_t(MEM_W(model_index*4,models));
    if (!readable(model,0x74)) return result;
    const auto positions = uint32_t(MEM_W(0x70,model));
    if (!readable(positions,unsigned(count)*12)) return result;
    std::array<WeakPoint,11> groups{};

    for (unsigned part=0;part<unsigned(count);++part) {
        const auto address = parts+part*part_stride;
        unsigned group;
        if (!part_target(rdram,owner,object,hit,address,part,group).vulnerable) continue;
        auto& bounds = groups[group];
        for (unsigned axis=0;axis<3;++axis) {
            const float center = std::bit_cast<float>(uint32_t(MEM_W(part*12+axis*4,positions)));
            const float radius = std::bit_cast<float>(uint32_t(MEM_W(axis==1?0x28:0x24,address)));
            if (!std::isfinite(center) || !std::isfinite(radius) || radius < 0 || radius > 65536)
                throw std::runtime_error("Invalid boss weak-point bounds");
            bounds.minimum[axis] = bounds.identity ? std::min(bounds.minimum[axis],center-radius) : center-radius;
            bounds.maximum[axis] = bounds.identity ? std::max(bounds.maximum[axis],center+radius) : center+radius;
        }
        if (!bounds.identity) bounds.identity = address;
    }
    for (const auto& group : groups) if (group.identity) result.regions[result.count++] = group;
    return result;
}
