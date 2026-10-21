#include "boss_health.hpp"
#include "jfc_recomp.h"
#include "jfc_enhancements.hpp"
#include "gbi/rt64_gbi_f3ddkr.h"
#include "include/rt64_f3ddkr_tracer.h"
#include "../include/rt64_extended_gbi.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace {
constexpr uint32_t cursor_pointer = 0x800F2CC8U;
constexpr unsigned segments = 8;

constexpr std::array<std::array<std::array<float,2>,2>,segments> brackets{{
    {{{-1,-0.6f},{-1,-1}}}, {{{-1,-1},{-0.6f,-1}}},
    {{{0.6f,-1},{1,-1}}}, {{{1,-1},{1,-0.6f}}},
    {{{1,0.6f},{1,1}}}, {{{1,1},{0.6f,1}}},
    {{{-0.6f,1},{-1,1}}}, {{{-1,1},{-1,0.6f}}}}};
constexpr std::array pushes{G_EX_PUSHPROJMATRIX_V1,G_EX_PUSHGEOMETRYMODE_V1,
    G_EX_PUSHOTHERMODE_V1,G_EX_PUSHCOMBINE_V1,G_EX_PUSHPRIMCOLOR_V1};
constexpr std::array pops{G_EX_POPPRIMCOLOR_V1,G_EX_POPCOMBINE_V1,
    G_EX_POPOTHERMODE_V1,G_EX_POPGEOMETRYMODE_V1,G_EX_POPPROJMATRIX_V1};

float floating(uint8_t* rdram,uint32_t address) {
    const float result = std::bit_cast<float>(uint32_t(MEM_W(0,address)));
    if (!std::isfinite(result)) throw std::runtime_error("Non-finite weak-point camera");
    return result;
}

uint32_t reserve(uint8_t* rdram,uint32_t bytes) {
    const int bank = MEM_W(0,0x800FD7C0U);
    const int capacity = MEM_W(0,0x800FD7DCU);
    if (bank < 0 || bank > 1 || capacity <= 0 || capacity > 0x100000)
        throw std::runtime_error("Invalid weak-point command arena");
    const auto base = uint32_t(MEM_W(bank*4,0x800FB400U));
    const auto cursor = uint32_t(MEM_W(0,cursor_pointer));
    const uint64_t end = uint64_t(base)+uint32_t(capacity)*8U;
    if ((base & 0xE0000007U) != 0x80000000U || (cursor & 7U) ||
        (base & 0x1FFFFFFFU)+uint64_t(capacity)*8 > 0x800000U || cursor < base ||
        uint64_t(cursor)+bytes > end)
        throw std::runtime_error("Boss weak-point effect exceeds native command arena");
    return cursor;
}
}

extern "C" void jfc_draw_boss_weak_points(uint8_t* rdram,recomp_context*,uint32_t object) {
    const auto points = jfc::boss::weak_points(rdram,object);
    if (!points.count) return;
    const uint64_t generation = jfc_effect_generation(object);
    const unsigned projection_bank = MEM_BU(0,0x800A316CU);

    if (projection_bank >= 16) throw std::runtime_error("Invalid weak-point projection bank");
    const uint32_t projection = 0x800FAB38U+projection_bank*64;
    std::array<std::array<float,3>,3> basis{};
    for (unsigned column=0;column<3;++column) {
        float length=0;
        for (unsigned axis=0;axis<3;++axis) {
            basis[column][axis]=floating(rdram,0x800FAF78U+(axis*4+column)*4);
            length+=basis[column][axis]*basis[column][axis];
        }
        if (!std::isfinite(length) || length < 0.5f || length > 2.0f)
            throw std::runtime_error("Invalid weak-point camera basis");
        const float inverse_length=1.0f/std::sqrt(length);
        for (auto& value : basis[column]) value *= inverse_length;
    }

    constexpr uint32_t payload_per_region=segments*RT64::F3DDKRTracerBytes;
    constexpr uint32_t commands_per_region=(1+segments*2)*8;
    const uint32_t bytes=280+points.count*(payload_per_region+commands_per_region);
    const uint32_t start=reserve(rdram,bytes);
    const uint32_t triangles=start+8;
    const uint32_t owned_projection=triangles+32;
    const uint32_t payload=owned_projection+64;
    for(unsigned i=0;i<16;++i) MEM_W(i*4,owned_projection)=MEM_W(i*4,projection);
    jfc::enhancements::record_effect_matrix(rdram,owned_projection,object);
    uint32_t cursor=payload+points.count*payload_per_region;
    MEM_W(0,start)=0x06010000U;
    MEM_W(4,start)=cursor & 0x1FFFFFFFU;
    for(unsigned i=0;i<32;++i) MEM_B(i,triangles)=0;
    MEM_B(0,triangles)=0x40; MEM_B(1,triangles)=0; MEM_B(2,triangles)=1; MEM_B(3,triangles)=2;
    MEM_B(16,triangles)=0x40; MEM_B(17,triangles)=2; MEM_B(18,triangles)=1; MEM_B(19,triangles)=3;
    const auto emit=[&](uint32_t w0,uint32_t w1) {
        MEM_W(0,cursor)=w0; MEM_W(4,cursor)=w1; cursor+=8;
    };
    emit(RT64_HOOK_MAGIC_NUMBER,0x50000064U);
    for(auto op:pushes) emit(0x64000000U|op,0);
    emit(0xE7000000U,0);
    emit(0xBA000020U,0x00000CF0U);
    emit(0xB9000020U,0x005049D8U);
    emit(0xFCFFFFFFU,0xFFFDF6FBU);
    emit(0xB6000000U,0xFFFFFFFFU);
    emit(0xB7000000U,1);
    emit(0xBC000002U,0);
    emit(0xFA000000U,0x99DDE8D8U);
    for(unsigned index=0;index<points.count;++index) {
        const auto& point=points.regions[index];
        std::array<float,3> center{},extent{};
        for(unsigned axis=0;axis<3;++axis) {
            center[axis]=(point.minimum[axis]+point.maximum[axis])*0.5f;
            extent[axis]=(point.maximum[axis]-point.minimum[axis])*0.5f;
        }
        std::array<float,3> radius{};
        for(unsigned column=0;column<3;++column)
            for(unsigned axis=0;axis<3;++axis) radius[column]+=std::abs(basis[column][axis])*extent[axis];
        const float width=std::max(0.35f,std::min(radius[0],radius[1])*0.015f);
        for(unsigned axis=0;axis<3;++axis) center[axis]+=basis[2][axis]*(radius[2]+width);
        const uint32_t id=(point.identity & 0x1FFFFFFFU)>>2;
        emit(RT64::GBI_F3DDKR::withMatrixTagW0(0x01010040U,id),
            RT64::GBI_F3DDKR::withMatrixTagW1(owned_projection & 0x1FFFFFFFU,id));
        for(unsigned segment=0;segment<segments;++segment) {
            RT64::F3DDKRTracer line;
            line.identity = RT64::f3ddkrTracerIdentity(point.identity & 0x1FFFFFFFU, segment);
            line.generation = generation;
            line.width=width;
            for(unsigned endpoint=0;endpoint<2;++endpoint) {
                const auto& angle=brackets[segment][endpoint];
                for(unsigned axis=0;axis<3;++axis)
                    line.endpoints[endpoint*3+axis]=center[axis]+
                        basis[0][axis]*radius[0]*angle[0]+basis[1][axis]*radius[1]*angle[1];
            }
            const uint32_t address=(payload+index*payload_per_region+segment*RT64::F3DDKRTracerBytes)&0x1FFFFFFFU;
            RT64::writeF3DDKRTracer(rdram,0x800000U,address,line);
            emit(0x04200030U|RT64::F3DDKRTracerMarker,address);
            emit(0x05100020U,triangles & 0x1FFFFFFFU);
        }
    }
    emit(0xE7000000U,0);
    emit(0xBC00000AU,0);
    for(auto op:pops) emit(0x64000000U|op,0);
    emit(RT64_HOOK_MAGIC_NUMBER,0x60000000U);
    if(cursor!=start+bytes) throw std::runtime_error("Weak-point command sizing mismatch");
    MEM_W(0,cursor_pointer)=cursor;
}
