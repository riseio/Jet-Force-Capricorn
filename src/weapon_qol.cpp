#include "weapon_qol.hpp"
#include "jfc_recomp.h"
#include "native_context.hpp"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cmath>
#include <stdexcept>
#include <bit>
#include <numbers>

namespace {
std::atomic_bool shotgun_upgrade{true};
std::atomic_bool flamethrower_upgrade{true};
std::atomic_bool shocker_upgrade{true};
jfc::weapons::State weapon_state{};
struct Ignition { uint32_t object; bool accepted = false; };
thread_local Ignition* ignition = nullptr;
bool hit_update_active = false;
bool chain_processing = false;
struct Contacts {
    std::array<uint32_t, 256> roots{};
    uint32_t count = 0;
    std::array<bool, 3> arcs{};
};
std::array<Contacts, 4> contacts{};

[[noreturn]] void invalid_weapon(const char* reason) {
    std::fprintf(stderr, "JFC weapon enhancement: %s\n", reason);
    std::exit(EXIT_FAILURE);
}

void require_span(uint32_t address, uint32_t size, uint32_t alignment = 4) {
    const auto segment = address & 0xE0000000U;
    const auto physical = address & 0x1FFFFFFFU;
    if ((segment != 0x80000000U && segment != 0xA0000000U) ||
        (address & (alignment - 1U)) || size > 0x800000U || physical > 0x800000U - size)
        invalid_weapon("invalid native operand span");
}
uint32_t object_index(uint32_t object) {
    require_span(object, 0x6C);
    return (object & 0x1FFFFFFFU) / 4;
}
bool player_burn(uint32_t object) {
    const auto index = object_index(object);
    return (weapon_state.flame_burns[index / 32] & (1U << (index % 32))) != 0;
}
}

void jfc::weapons::set_shotgun_upgrade(bool enabled) {
    shotgun_upgrade.store(enabled, std::memory_order_relaxed);
}
void jfc::weapons::set_flamethrower_upgrade(bool enabled) {
    flamethrower_upgrade.store(enabled, std::memory_order_relaxed);
}
void jfc::weapons::set_shocker_upgrade(bool enabled) {
    shocker_upgrade.store(enabled, std::memory_order_relaxed);
}
extern "C" uint32_t jfc_weapon_capacity(uint32_t capacity, uint32_t weapon) {
    return weapon == 4 && shocker_upgrade.load(std::memory_order_relaxed)
        ? std::max(capacity, 100U) : capacity;
}
extern "C" uint32_t jfc_weapon_pickup_capacity(uint32_t capacity, uint32_t weapon, uint32_t held) {
    const auto effective = jfc_weapon_capacity(capacity, weapon);

    return weapon == 4 ? std::max(effective, held) : effective;
}
extern "C" void jfc_shocker_range(uint8_t* rdram, recomp_context* ctx) {
    const auto object = uint32_t(ctx->r4);
    require_span(object, 0x6C);
    const auto state = uint32_t(MEM_W(0x68, object));
    require_span(state, 0xA84);
    const auto owner = uint32_t(MEM_W(0xA38, state));
    if (!owner) return;
    require_span(owner, 0x6C);
    if (MEM_H(0x48, owner) != 1) return;
    const auto player = uint32_t(MEM_W(0x68, owner));
    require_span(player, 0x564);
    if (uint32_t(MEM_W(0x560, player)) != object) return;
    const auto port = MEM_BU(0, player);
    if (port >= 4) invalid_weapon("invalid shocker player port");
    auto& chain = weapon_state.chains[port];

    if (!chain.root) {
        chain = {}; chain.root = object; chain.owner = owner;
        chain.spawn[1] = 0xB6; chain.spawn[2] = uint8_t(chain.spawn.size());
        for (unsigned i = 0; i < 4; ++i) chain.spawn[0x10 + i] = uint8_t(owner >> (24 - i * 8));
        for (unsigned i = 0; i < 18; ++i) chain.spawn[0x14 + i] = MEM_BU(0xA64 + i, state);
        chain.spawn[0x26] = uint8_t(MEM_HU(0xA7A, state) >> 8);
        chain.spawn[0x27] = uint8_t(MEM_HU(0xA7A, state));
        chain.spawn[0x28] = MEM_BU(0xA7C, state);
        chain.spawn[0x29] = MEM_BU(0xA82, state);
        chain.spawn[0x2A] = MEM_BU(0xA83, state);
    } else if (chain.root != object || chain.owner != owner) invalid_weapon("conflicting native shocker owner");
    const int length = shocker_upgrade.load(std::memory_order_relaxed) ? 1600 : 400;
    MEM_H(0xA7A, state) = length;
    if (MEM_H(0xA78, state) > length) MEM_H(0xA78, state) = length;
}
const jfc::weapons::State& jfc::weapons::state() {
    if (ignition || hit_update_active) throw std::runtime_error("Save requested inside native weapon contact");
    return weapon_state;
}
void jfc::weapons::restore_state(const State& saved) noexcept {
    weapon_state = saved; contacts = {}; hit_update_active = chain_processing = false;
}
void jfc::weapons::validate_state(const State& saved) {
    for (auto half : saved.flame_half_round)
        if (half > 1) throw std::runtime_error("Invalid saved flamethrower debit carry");
    std::array<uint32_t, 16> objects{}; size_t count = 0;
    auto pointer = [](uint32_t address) {
        const auto segment = address & 0xE0000000U;
        return address == 0 || ((segment == 0x80000000U || segment == 0xA0000000U) && !(address & 3) &&
            (address & 0x1FFFFFFFU) <= 0x800000U - 0xA1U);
    };
    for (const auto& chain : saved.chains) {
        if (!pointer(chain.root) || !pointer(chain.owner) || bool(chain.root) != bool(chain.owner))
            throw std::runtime_error("Invalid saved shocker owner");
        if (chain.root) {
            uint32_t owner = 0;
            for (unsigned i = 0; i < 4; ++i) owner = (owner << 8) | chain.spawn[0x10 + i];
            if (chain.spawn[0] || chain.spawn[1] != 0xB6 || chain.spawn[2] != 44 ||
                owner != chain.owner || chain.spawn[0x29] != 0 || chain.spawn[0x2A] != 0)
                throw std::runtime_error("Invalid saved native shocker spawn parameters");
        }
        if (chain.root) objects[count++] = chain.root & 0x1FFFFFFFU;
        std::array<uint32_t, 4> victims{}; size_t victim_count = 0; bool ended = false;
        for (const auto& arc : chain.arcs) {
            if (!pointer(arc.beam) || !pointer(arc.source) || !pointer(arc.target) ||
                (arc.beam && !chain.root) || bool(arc.source) != bool(arc.target) ||
                (arc.target && !arc.beam) || (arc.source && arc.source == arc.target))
                throw std::runtime_error("Invalid saved shocker arc");
            if (arc.beam) objects[count++] = arc.beam & 0x1FFFFFFFU;
            if (arc.target) {
                const auto source = arc.source & 0x1FFFFFFFU, target = arc.target & 0x1FFFFFFFU;
                if (!victim_count) victims[victim_count++] = source;
                if (ended || victims[victim_count - 1] != source ||
                    std::find(victims.begin(), victims.begin() + victim_count, target) != victims.begin() + victim_count)
                    throw std::runtime_error("Invalid saved shocker target path");
                victims[victim_count++] = target;
            } else ended = true;
        }
    }
    std::sort(objects.begin(), objects.begin() + count);
    if (std::adjacent_find(objects.begin(), objects.begin() + count) != objects.begin() + count)
        throw std::runtime_error("Aliased saved shocker beam");
}

namespace {
using jfc::weapons::Arc;
using jfc::weapons::Chain;
struct Point { float x, y, z; };
float real(uint8_t* rdram, uint32_t address) {
    return std::bit_cast<float>(uint32_t(MEM_W(0, address)));
}
void real(uint8_t* rdram, uint32_t address, float value) {
    MEM_W(0, address) = std::bit_cast<uint32_t>(value);
}
uint32_t beam_state(uint8_t* rdram, uint32_t object) {
    require_span(object, 0xA1);
    if (MEM_H(0x48, object) != 0x98) invalid_weapon("chain object is not a native beam");
    const auto state = uint32_t(MEM_W(0x68, object));
    require_span(state, 0xA84);
    return state;
}
bool enemy(uint8_t* rdram, uint32_t object) {
    if (!object) return false;
    require_span(object, 0xA1);
    if (MEM_H(0x48, object) != 0x18 || MEM_H(0x4A, object) == 0x112 || MEM_BU(0xA0, object)) return false;
    const auto state = uint32_t(MEM_W(0x68, object)), hit = uint32_t(MEM_W(0x4C, object));
    if (!state || !hit) return false;
    require_span(state, 0xA8); require_span(hit, 0x40);
    return MEM_H(0xA6, state) > 0 && MEM_BU(0x55, state) == 0 &&
        !(uint32_t(MEM_W(0, state)) & 8U) && MEM_H(6, hit) > 0;
}
void prepare_call(recomp_context& call, const recomp_context& parent, uint32_t reserve = 0x80) {
    const auto stack = uint32_t(parent.r29);
    require_span(stack, 0x20, 8);
    if ((stack & 0x1FFFFFFFU) < reserve + 0x400) invalid_weapon("insufficient chain call stack");
    jfc::runtime::copy_context(call, parent);
    call.r29 = ADD32(parent.r29, -int32_t(reserve));
}
Point center(uint8_t* rdram, const recomp_context& parent, uint32_t object, recomp_func_t* rotate) {
    require_span(object, 0x50);
    const auto hit = uint32_t(MEM_W(0x4C, object)); require_span(hit, 6, 2);
    recomp_context call; prepare_call(call, parent);
    const uint32_t vector = uint32_t(call.r29) + 0x20;
    for (unsigned i = 0; i < 3; ++i) real(rdram, vector + i * 4, float(MEM_H(i * 2, hit)));
    call.r4 = 1; call.r5 = int32_t(object); call.r6 = call.r7 = int32_t(vector);
    rotate(rdram, &call);
    const float scale = real(rdram, object + 8);
    Point p{real(rdram, object + 0xC) + real(rdram, vector) * scale,
            real(rdram, object + 0x10) + real(rdram, vector + 4) * scale,
            real(rdram, object + 0x14) + real(rdram, vector + 8) * scale};
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) invalid_weapon("nonfinite chain position");
    return p;
}
Point hit_center(uint8_t* rdram, uint32_t object) {
    const auto hit = uint32_t(MEM_W(0x4C, object)); require_span(hit, 0x18);
    Point p{real(rdram, hit + 0xC), real(rdram, hit + 0x10), real(rdram, hit + 0x14)};
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) invalid_weapon("nonfinite hit position");
    return p;
}
float distance2(Point a, Point b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x*x + y*y + z*z;
}
bool clear_line(uint8_t* rdram, const recomp_context& parent, uint32_t source, uint32_t target,
                Point from, Point to, recomp_func_t* visibility) {
    recomp_context call; prepare_call(call, parent);
    call.r4 = int32_t(source); call.r5 = int32_t(target);
    call.r6 = std::bit_cast<uint32_t>(from.x); call.r7 = std::bit_cast<uint32_t>(from.y);
    real(rdram, uint32_t(call.r29) + 0x10, from.z);
    real(rdram, uint32_t(call.r29) + 0x14, to.x);
    real(rdram, uint32_t(call.r29) + 0x18, to.y);
    real(rdram, uint32_t(call.r29) + 0x1C, to.z);
    visibility(rdram, &call);
    return call.r2 == 0;
}
bool active(uint8_t* rdram, const Chain& chain) {
    if (!chain.root || !shocker_upgrade.load(std::memory_order_relaxed)) return false;
    const auto state = beam_state(rdram, chain.root);
    require_span(chain.owner, 0xA1);
    if (MEM_BU(0xA0, chain.root) || MEM_BU(0xA0, chain.owner)) return false;
    const auto player = uint32_t(MEM_W(0x68, chain.owner)); require_span(player, 0x564);
    return MEM_H(0x48, chain.owner) == 1 && uint32_t(MEM_W(0x560, player)) == chain.root &&
        MEM_BU(0xA7D, state) && MEM_H(0xA78, state) > 0;
}
void silence(uint8_t* rdram, const Arc& arc) {
    if (!arc.beam) return;
    const auto state = beam_state(rdram, arc.beam);
    MEM_B(0xA7D, state) = 0; MEM_H(0xA78, state) = 0; MEM_H(0xA7A, state) = 0;
    const auto hit = uint32_t(MEM_W(0x4C, arc.beam)); require_span(hit, 8);
    MEM_H(6, hit) = 0;
}
Arc* find_arc(uint32_t object, size_t* group = nullptr, size_t* index = nullptr) {
    if (!object) return nullptr;
    for (size_t g = 0; g < weapon_state.chains.size(); ++g)
        for (size_t i = 0; i < 3; ++i) if (weapon_state.chains[g].arcs[i].beam == object) {
            if (group) *group = g;
            if (index) *index = i;
            return &weapon_state.chains[g].arcs[i];
        }
    return nullptr;
}
bool touched(const Contacts& hits, uint32_t target) {
    return std::find(hits.roots.begin(), hits.roots.begin() + hits.count, target) != hits.roots.begin() + hits.count;
}
}
extern "C" void jfc_shocker_created(uint8_t* rdram, recomp_context* ctx) {
    if (!ctx->r2) return;
    const auto stack = uint32_t(ctx->r29); require_span(stack, 0x50, 8);
    const auto owner = uint32_t(MEM_W(0x34, stack)); require_span(owner, 0x6C);
    const auto player = uint32_t(MEM_W(0x68, owner)); require_span(player, 4);
    const auto port = MEM_BU(0, player);
    if (port >= 4) invalid_weapon("invalid shocker player port");
    auto& chain = weapon_state.chains[port];
    if (chain.root) invalid_weapon("native shocker replaced without releasing its previous beam");
    chain = {}; chain.root = uint32_t(ctx->r2); chain.owner = owner;
    for (size_t i = 0; i < chain.spawn.size(); ++i) chain.spawn[i] = MEM_BU(0x24 + i, stack);
}
extern "C" void jfc_weapon_world_reset(void) {

    weapon_state.chains = {};
    weapon_state.flame_burns.fill(0);
    contacts = {};
}
extern "C" void jfc_shocker_free(uint8_t* rdram, recomp_context* ctx, recomp_func_t* release) {
    const auto object = uint32_t(ctx->r4);
    for (auto& chain : weapon_state.chains) {
        if (chain.root == object || chain.owner == object) {
            const auto arcs = chain.arcs; chain = {};
            for (const auto& arc : arcs) if (arc.beam && arc.beam != object) {
                recomp_context call; prepare_call(call, *ctx);
                call.r4 = int32_t(arc.beam); release(rdram, &call);
            }
        } else {
            bool disconnected = false;
            for (auto& arc : chain.arcs) {
                disconnected |= arc.beam == object || arc.source == object || arc.target == object;
                if (arc.beam == object) arc = {};
                else if (disconnected) {
                    arc.source = arc.target = 0; silence(rdram, arc);
                }
            }
        }
    }
}
extern "C" void jfc_shocker_pose(uint8_t* rdram, recomp_context* ctx, recomp_func_t* rotate) {
    size_t group = 0;
    auto* arc = find_arc(uint32_t(ctx->r4), &group);
    if (!arc) return;
    const auto& chain = weapon_state.chains[group];
    if (!active(rdram, chain) || !enemy(rdram, arc->source) || !enemy(rdram, arc->target)) {
        silence(rdram, *arc); return;
    }
    const auto from = center(rdram, *ctx, arc->source, rotate), to = center(rdram, *ctx, arc->target, rotate);
    const float squared = distance2(from, to);
    if (squared < 1.0f || squared > 200.0f * 200.0f) { silence(rdram, *arc); return; }
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    constexpr float angles = 32768.0f / std::numbers::pi_v<float>;
    const auto state = beam_state(rdram, arc->beam);
    MEM_H(0, arc->beam) = uint16_t(int32_t(std::lround(std::atan2(-dx, -dz) * angles)));
    MEM_H(2, arc->beam) = uint16_t(int32_t(std::lround(std::atan2(dy, std::hypot(dx, dz)) * angles)));
    MEM_H(4, arc->beam) = 0;
    real(rdram, arc->beam + 8, 1.0f);
    real(rdram, arc->beam + 0xC, from.x); real(rdram, arc->beam + 0x10, from.y); real(rdram, arc->beam + 0x14, from.z);

    MEM_H(0xA78, state) = MEM_H(0xA7A, state) = int16_t(std::ceil(std::sqrt(squared)));
    MEM_B(0xA7D, state) = 1;
}
extern "C" void jfc_shocker_hits_begin(uint8_t* rdram, recomp_context* ctx,
                                       recomp_func_t* visibility, recomp_func_t* rotate) {
    if (hit_update_active) invalid_weapon("recursive native hit update");
    hit_update_active = true;
    chain_processing = shocker_upgrade.load(std::memory_order_relaxed) &&
        std::any_of(weapon_state.chains.begin(), weapon_state.chains.end(), [](const auto& c) { return c.root != 0; });
    if (!chain_processing) return;
    contacts = {};

    for (auto& chain : weapon_state.chains) for (auto& arc : chain.arcs) if (arc.beam) {
        if (!active(rdram, chain) || !enemy(rdram, arc.source) || !enemy(rdram, arc.target)) {
            silence(rdram, arc); continue;
        }
        const auto from = center(rdram, *ctx, arc.source, rotate), to = center(rdram, *ctx, arc.target, rotate);
        if (distance2(from, to) > 40000.0f || !clear_line(rdram, *ctx, arc.source, arc.target, from, to, visibility))
            silence(rdram, arc);
    }
}
extern "C" void jfc_shocker_hits_order(uint8_t* rdram, recomp_context* ctx) {
    if (!chain_processing) return;
    const auto stack = uint32_t(ctx->r29); require_span(stack, 0x128, 8);
    const auto count = uint32_t(MEM_W(0x120, stack));
    if (count > 256) invalid_weapon("native attacker count out of range");
    const auto list = uint32_t(MEM_W(0, 0x801047ECU)); require_span(list, count * 4);
    std::array<uint32_t, 256> ordered{}; size_t next = 0;
    std::array<bool, 12> present{};
    for (size_t i = 0; i < count; ++i) {
        const auto object = uint32_t(MEM_W(i * 4, list)); size_t g, a;
        if (find_arc(object, &g, &a)) present[g * 3 + a] = true;
        else ordered[next++] = object;
    }
    for (size_t g = 0; g < 4; ++g) for (size_t a = 0; a < 3; ++a)
        if (present[g * 3 + a]) ordered[next++] = weapon_state.chains[g].arcs[a].beam;
    if (next != count) invalid_weapon("duplicate native chain attacker");
    for (size_t i = 0; i < count; ++i) MEM_W(i * 4, list) = ordered[i];
}
extern "C" int jfc_shocker_query(uint8_t* rdram, recomp_context* ctx, recomp_func_t* query) {
    size_t g, a; auto* arc = find_arc(uint32_t(ctx->r20), &g, &a);
    if (!arc) return 0;
    auto& hits = contacts[g]; const auto& chain = weapon_state.chains[g];
    const bool predecessor = a == 0 ? touched(hits, arc->source) : hits.arcs[a - 1];
    ctx->r2 = 0;
    if (!chain_processing || !active(rdram, chain) || !predecessor || touched(hits, arc->target) || !enemy(rdram, arc->target)) return 1;
    const auto count = uint32_t(ctx->r6), candidates = uint32_t(ctx->r7);
    if (count > 256) invalid_weapon("native query target count out of range");
    require_span(candidates, count * 4);
    bool eligible = false;
    for (uint32_t i = 0; i < count; ++i)
        if (uint32_t(MEM_W(i * 4, candidates)) == arc->target) { eligible = true; break; }
    if (!eligible) return 1;
    recomp_context call; prepare_call(call, *ctx);
    const auto stack = uint32_t(call.r29);
    MEM_W(0x20, stack) = arc->target;
    MEM_W(0x10, stack) = MEM_W(0x10, ctx->r29);
    MEM_W(0x14, stack) = ctx->r21;
    call.r4 = ctx->r20; call.r5 = ctx->r23; call.r6 = 1; call.r7 = int32_t(stack + 0x20);
    query(rdram, &call); ctx->r2 = call.r2;
    return 1;
}
extern "C" void jfc_shocker_hit(uint8_t* rdram, recomp_context* ctx) {
    if (!chain_processing) return;
    const auto projectile = uint32_t(ctx->r20), victim = uint32_t(ctx->r18);

    if (int32_t(ctx->r3) <= 0) return;
    size_t g, a;
    if (auto* arc = find_arc(projectile, &g, &a)) {
        if (arc->target == victim) contacts[g].arcs[a] = true;
        return;
    }
    for (g = 0; g < 4; ++g) if (weapon_state.chains[g].root == projectile) {
        if (!enemy(rdram, victim)) return;
        auto& hits = contacts[g];
        if (!touched(hits, victim)) {
            if (hits.count == hits.roots.size()) invalid_weapon("too many native shocker contacts");
            hits.roots[hits.count++] = victim;
        }
        return;
    }
}
extern "C" void jfc_shocker_hits_end(uint8_t* rdram, recomp_context* ctx,
                                     recomp_func_t* visibility, recomp_func_t* create) {
    if (!chain_processing) { hit_update_active = false; return; }
    if (std::none_of(contacts.begin(), contacts.end(), [](const auto& h) { return h.count != 0; })) {
        for (auto& chain : weapon_state.chains) for (auto& arc : chain.arcs) {
            arc.source = arc.target = 0; silence(rdram, arc);
        }
        hit_update_active = false; return;
    }
    const auto stack = uint32_t(ctx->r29); require_span(stack, 0x128, 8);
    const auto count = uint32_t(MEM_W(0x124, stack));
    if (count > 256) invalid_weapon("native target count out of range");
    std::array<uint32_t, 256> candidates{};
    const auto list = uint32_t(MEM_W(0, 0x801047E8U)); require_span(list, count * 4);
    for (size_t i = 0; i < count; ++i) candidates[i] = uint32_t(MEM_W(i * 4, list));

    for (size_t g = 0; g < 4; ++g) {
        auto& chain = weapon_state.chains[g]; const auto& hits = contacts[g];
        uint32_t source = active(rdram, chain) && hits.count ? hits.roots[0] : 0;
        std::array<uint32_t, 3> selected{};
        for (size_t a = 0; a < 3; ++a) {
            auto& arc = chain.arcs[a]; uint32_t target = 0;
            if (source && enemy(rdram, source)) {
                const auto from = hit_center(rdram, source);

                struct Candidate { float distance; uint32_t object; };
                std::array<Candidate, 256> nearby{}; size_t n = 0;
                for (size_t i = 0; i < count; ++i) {
                    const auto object = candidates[i];
                    if (object == source || touched(hits, object) ||
                        std::find(selected.begin(), selected.end(), object) != selected.end() || !enemy(rdram, object)) continue;
                    const float squared = distance2(from, hit_center(rdram, object));
                    if (squared >= 1.0f && squared <= 40000.0f) nearby[n++] = {squared, object};
                }
                std::sort(nearby.begin(), nearby.begin() + n, [](const auto& l, const auto& r) {
                    return l.distance != r.distance ? l.distance < r.distance : l.object < r.object;
                });
                for (size_t i = 0; i < n; ++i) if (clear_line(rdram, *ctx, source, nearby[i].object,
                        from, hit_center(rdram, nearby[i].object), visibility)) { target = nearby[i].object; break; }
            }
            if (target && !arc.beam) {
                recomp_context call; prepare_call(call, *ctx);
                const uint32_t payload = uint32_t(call.r29) + 0x20;
                for (size_t i = 0; i < chain.spawn.size(); ++i) MEM_B(i, payload) = chain.spawn[i];
                call.r4 = int32_t(payload); call.r5 = 1; create(rdram, &call);
                if (call.r2) {
                    arc.beam = uint32_t(call.r2); beam_state(rdram, arc.beam);
                    MEM_W(0x3C, arc.beam) = 0;
                } else target = 0;
            }
            arc.source = target ? source : 0; arc.target = target;
            if (!target) silence(rdram, arc);
            selected[a] = target; source = target;
        }
    }
    hit_update_active = false;
}

extern "C" int jfc_flamethrower_upgrade_enabled(void) {
    return flamethrower_upgrade.load(std::memory_order_relaxed);
}
extern "C" void jfc_flame_debit(uint8_t* rdram, recomp_context* ctx) {
    if (!jfc_flamethrower_upgrade_enabled() || ctx->r25 != 6) return;
    const uint32_t player = uint32_t(ctx->r16);
    require_span(player, 4);
    const unsigned port = MEM_BU(0, player), character = MEM_BU(1, player);
    if (port > 3 || character > 7) invalid_weapon("invalid flame debit owner");
    auto& half = weapon_state.flame_half_round[port * 4 + (character & 3)];
    half ^= 1;
    if (half) ctx->r10 = ctx->r3;
}
extern "C" void jfc_forget_weapon_object(uint32_t object) {
    if (!object) return;
    const auto index = object_index(object);
    weapon_state.flame_burns[index / 32] &= ~(1U << (index % 32));
}
extern "C" void jfc_accepted_ignition(uint32_t object) {
    if (ignition && ignition->object == object) ignition->accepted = true;
}
extern "C" void jfc_flame_contact(uint8_t* rdram, recomp_context* ctx,
                                  recomp_func_t* ignite, recomp_func_t* hurt) {
    const auto object = uint32_t(ctx->r16);
    const auto index = object_index(object);
    const auto stack = uint32_t(ctx->r29);
    require_span(stack, 0xB8, 8);
    if ((stack & 0x1FFFFFFFU) < 0x100) invalid_weapon("insufficient flame call stack");
    Ignition current{object};
    struct Scope {
        Ignition* previous;
        ~Scope() { ignition = previous; }
    } scope{ignition};
    ignition = &current;
    recomp_context call;
    jfc::runtime::copy_context(call, *ctx);
    call.r29 = ADD32(call.r29, -0x20);
    call.r4 = int32_t(object);
    ignite(rdram, &call);
    if (!current.accepted) return;
    weapon_state.flame_burns[index / 32] |= 1U << (index % 32);
    const auto state = uint32_t(MEM_W(0x68, object));
    const auto hit = uint32_t(MEM_W(0x4C, object));
    require_span(state, 0x4C); require_span(hit, 8);
    if (MEM_H(0x48, object) != 0x18 || MEM_H(0x4A, object) == 0x112 ||
        MEM_H(6, hit) <= 0 || (uint32_t(MEM_W(0, state)) & 8U)) return;

    const auto animation = MEM_BU(0xA, state);
    const bool fire_animation = MEM_BU(0x4B, state) == 8 && (animation == 0xF || animation == 0xD);
    jfc::runtime::copy_context(call, *ctx);
    call.r29 = ADD32(call.r29, -0x20);
    call.r4 = fire_animation ? 11 : 10; call.r5 = 0x16; call.r6 = 0; call.r7 = 0;
    hurt(rdram, &call);
}
extern "C" void jfc_flame_damage(uint8_t* rdram, recomp_context* ctx) {
    if (!jfc_flamethrower_upgrade_enabled()) return;
    const uint32_t owner = uint32_t(ctx->r18);
    require_span(owner, 4);
    const uint32_t object = uint32_t(MEM_W(0, owner));
    if (!player_burn(object)) return;
    if (!std::isfinite(ctx->f18.fl) || ctx->f18.fl < 0.0f)
        invalid_weapon("invalid native burning damage");
    ctx->f18.fl *= 1.5f;
}

extern "C" int jfc_shotgun_upgrade_enabled(void) {
    return shotgun_upgrade.load(std::memory_order_relaxed);
}

extern "C" void jfc_shotgun_press(uint8_t* rdram, recomp_context* ctx) {

    const uint32_t player = uint32_t(ctx->r16);
    require_span(player, 0x544);
    MEM_B(0x540, player) = MEM_BU(0x540, player) & ~0x80U;
}

extern "C" void jfc_shotgun_edge(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t binding = uint32_t(MEM_W(0, 0x800A18B0U));
    require_span(binding, 4);
    if (!(uint32_t(MEM_W(0, binding)) & uint32_t(MEM_W(0, 0x800F6DA4U))))
        ctx->r13 = 0;
}

extern "C" void jfc_shotgun_charge(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t stack = uint32_t(ctx->r29), player = uint32_t(ctx->r16);
    require_span(stack, 0x60, 8);
    require_span(player, 0x540);
    const uint32_t record = uint32_t(MEM_W(0x5C, stack));
    require_span(record, 0x76, 2);
    if (MEM_BU(0x70, record) != 3) invalid_weapon("shotgun charge has another weapon owner");

    MEM_H(0x53E, player) = MEM_BU(0x57, record) * 32;
}

extern "C" void jfc_shotgun_spread(uint8_t* rdram, recomp_context* ctx, recomp_func_t* create) {

    if (!ctx->r2) return;
    const uint32_t stack = uint32_t(ctx->r29);
    require_span(stack, 0x38, 8);
    if ((stack & 0x1FFFFFFFU) < 0x80) invalid_weapon("insufficient projectile call stack");
    const uint16_t yaw = MEM_HU(0x2C, stack);
    for (const int offset : {-1024, 1024}) {
        MEM_H(0x2C, stack) = uint16_t(yaw + offset);
        recomp_context call;
        jfc::runtime::copy_context(call, *ctx);
        call.r29 = ADD32(call.r29, -0x20);
        call.r4 = int32_t(stack + 0x20);
        call.r5 = 1;
        create(rdram, &call);
        if (call.r2) {
            const uint32_t projectile = uint32_t(call.r2);
            require_span(projectile, 0x40);
            MEM_W(0x3C, projectile) = 0;
        }
    }
    MEM_H(0x2C, stack) = yaw;
}
