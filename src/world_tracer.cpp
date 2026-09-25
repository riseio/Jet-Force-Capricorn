#include "jfc_recomp.h"
#include "native_context.hpp"
#include "include/rt64_f3ddkr_tracer.h"

#include <bit>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr uint32_t memory_size = 0x800000U;

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

extern "C" void jfc_draw_world_tracer(uint8_t *rdram, recomp_context *ctx,
    void (*get_projection)(uint8_t *, recomp_context *),
    void (*set_texture)(uint8_t *, recomp_context *)) {
    if (!rdram || !ctx || !get_projection || !set_texture)
        invalid_tracer("missing native producer context");
    const uint32_t stack = uint32_t(ctx->r29);
    physical(stack, 0x28, 8);
    if ((stack & 0x1FFFFFFFU) < 0x40)
        invalid_tracer("insufficient native call frame");
    const uint32_t dl_pointer = uint32_t(ctx->r4);
    const uint32_t vertex_pointer = uint32_t(ctx->r5);
    physical(dl_pointer, 4, 4);
    physical(vertex_pointer, 4, 4);
    const uint32_t vertex = uint32_t(MEM_W(0, vertex_pointer));
    const uint32_t vertex_physical = physical(vertex, RT64::F3DDKRTracerBytes, 2);
    const uint32_t triangles = physical(uint32_t(ctx->r7), 32, 8);
    RT64::F3DDKRTracer tracer;
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
    if (tracer.endpoints[0] == tracer.endpoints[3] &&
        tracer.endpoints[1] == tracer.endpoints[4] &&
        tracer.endpoints[2] == tracer.endpoints[5])
        return;

    const uint32_t prim = uint32_t(MEM_W(0x1C, stack));
    const uint32_t env = uint32_t(MEM_W(0x20, stack));
    const uint32_t flags = uint32_t(MEM_W(0x24, stack));
    const uint32_t texture = uint32_t(ctx->r6);
    recomp_context call;
    jfc::runtime::copy_context(call, *ctx);
    call.r29 = ADD32(call.r29, -0x40);
    const uint32_t local_cursor = uint32_t(call.r29) + 0x20;
    uint32_t cursor = uint32_t(MEM_W(0, dl_pointer));
    physical(cursor, 8, 8);
    get_projection(rdram, &call);
    const uint32_t projection = physical(uint32_t(call.r2), 64, 8);
    MEM_W(0, cursor) = 0x01010040U;
    MEM_W(4, cursor) = projection;
    cursor += 8;
    MEM_W(0, local_cursor) = cursor;
    call.r4 = int32_t(local_cursor);
    call.r5 = int32_t(texture);
    call.r6 = flags | 6U;
    call.r7 = 0;
    set_texture(rdram, &call);
    cursor = uint32_t(MEM_W(0, local_cursor));
    physical(cursor, 64, 8);
    const uint32_t commands[16] = {
        0xFA000000U, prim, 0xFB000000U, env,
        0x04200030U | ((vertex_physical & 6U) << 16) | RT64::F3DDKRTracerMarker, vertex_physical,
        0x05110020U, triangles, 0xE7000000U, 0,
        0xFA000000U, UINT32_MAX, 0xFB000000U, UINT32_MAX, 0xBC00000AU, 0
    };
    for (uint32_t i = 0; i < 16; ++i)
        MEM_W(i * 4, cursor) = commands[i];
    MEM_W(0, dl_pointer) = cursor + 64;
    MEM_W(0, vertex_pointer) = vertex + RT64::F3DDKRTracerBytes;
}
