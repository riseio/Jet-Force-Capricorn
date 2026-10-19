#include "jfc_recomp.h"
#include "include/rt64_f3ddkr_tracer.h"

#include <bit>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr uint32_t memory_size = 0x800000U;
thread_local uint32_t tracer_owner = 0, tracer_segment = 0;

[[noreturn]] void invalid_tracer(const char *reason) {
    std::fprintf(stderr, "JFC objDrawTracer: %s\n", reason);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

uint32_t physical(uint32_t address, uint32_t bytes, uint32_t alignment) {
    if ((address & 0xE0000000U) != 0x80000000U &&
        (address & 0xE0000000U) != 0xA0000000U)
        invalid_tracer("expected native KSEG0/KSEG1 pointer");
    const uint32_t result = address & 0x1FFFFFFFU;
    if ((result & (alignment - 1U)) || bytes > memory_size || result > memory_size - bytes)
        invalid_tracer("unaligned or out-of-range native buffer");
    return result;
}
}

extern "C" void jfc_validate_tracer_buffer(uint32_t address, uint32_t bytes, uint32_t alignment) {
    physical(address, bytes, alignment);
}

extern "C" void jfc_set_tracer_owner(uint32_t address, uint32_t segment) noexcept(false) {
    RT64::f3ddkrTracerIdentity(physical(address, 4, 4), segment);
    if (tracer_owner) throw std::runtime_error("Unconsumed native tracer owner");
    tracer_owner = address;
    tracer_segment = segment;
}

extern "C" int jfc_prepare_world_tracer(uint8_t *rdram, recomp_context *ctx) {
    if (!rdram || !ctx)
        invalid_tracer("missing native producer context");
    const uintptr_t ram_address = reinterpret_cast<uintptr_t>(rdram);
    const uintptr_t context_address = reinterpret_cast<uintptr_t>(ctx);
    if ((context_address >= ram_address && context_address - ram_address < memory_size) ||
        (ram_address >= context_address && ram_address - context_address < sizeof(*ctx)))
        invalid_tracer("native context overlaps guest RDRAM");
    const uint32_t stack = uint32_t(ctx->r29);
    physical(stack, 0x28, 8);
    if ((stack & 0x1FFFFFFFU) < 0xA8)
        invalid_tracer("insufficient native call frame");
    const uint32_t dl_pointer = uint32_t(ctx->r4);
    const uint32_t vertex_pointer = uint32_t(ctx->r5);
    physical(dl_pointer, 4, 4);
    physical(vertex_pointer, 4, 4);
    const uint32_t vertex = uint32_t(MEM_W(0, vertex_pointer));
    const uint32_t vertex_physical = physical(vertex, RT64::F3DDKRTracerBytes, 2);
    physical(uint32_t(ctx->r7), 32, 8);
    physical(uint32_t(MEM_W(0, dl_pointer)), 8, 8);
    RT64::F3DDKRTracer tracer;
    if (tracer_owner) {
        tracer.identity = RT64::f3ddkrTracerIdentity(tracer_owner & 0x1FFFFFFFU, tracer_segment);
        tracer.generation = jfc_effect_generation(tracer_owner);
        tracer_owner = 0;
    }
    for (uint32_t point = 0; point < 2; ++point) {
        const uint32_t source = uint32_t(MEM_W(0x10 + point * 4, stack));
        physical(source, 12, 4);
        for (uint32_t axis = 0; axis < 3; ++axis)
            tracer.endpoints[point * 3 + axis] = std::bit_cast<float>(uint32_t(MEM_W(axis * 4, source)));
    }
    tracer.width = std::bit_cast<float>(uint32_t(MEM_W(0x18, stack)));
    try {
        RT64::writeF3DDKRTracer(rdram, memory_size, vertex_physical, tracer);
    }
    catch (const std::exception &error) { invalid_tracer(error.what()); }
    return !(tracer.endpoints[0] == tracer.endpoints[3] &&
        tracer.endpoints[1] == tracer.endpoints[4] &&
        tracer.endpoints[2] == tracer.endpoints[5]);
}
