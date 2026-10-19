/*
Copyright 2011 X-Scale. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY X-Scale ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
EVENT SHALL X-Scale OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are
those of the authors and should not be interpreted as representing official
policies, either expressed or implied, of X-Scale.

The CIC challenge implementation in src/libultra_extensions.cpp adapts this
algorithm. It implements a hardware protocol and contains no extracted game
challenge/response pairs.
Reference: https://github.com/mupen64plus/mupen64plus-core/blob/master/src/device/pif/n64_cic_nus_6105.c
*/
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include "jfc_recomp.h"


namespace {

constexpr int32_t pfs_no_pack = 1;
constexpr uint32_t os_read = 0;
constexpr uint32_t os_write = 1;
constexpr size_t pif_ram_size = 0x40;
constexpr size_t cic_data_offset = 0x30;
constexpr size_t cic_byte_count = 15;

std::array<uint8_t, pif_ram_size> pif_ram{};

void return_no_pack(recomp_context* context) {
    context->r2 = pfs_no_pack;
}

void process_cic_6105_challenge() {
    static constexpr std::array<uint8_t, 16> lut0{
        0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
        0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9,
    };
    static constexpr std::array<uint8_t, 16> lut1{
        0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
        0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9,
    };

    std::array<uint8_t, cic_byte_count * 2> response{};
    uint8_t key = 0xB;
    const auto* lut = &lut0;
    for (size_t i = 0; i < response.size(); i++) {
        const uint8_t challenge_byte = pif_ram[cic_data_offset + i / 2];
        const uint8_t challenge = (i & 1) == 0 ? challenge_byte >> 4 : challenge_byte & 0xF;
        response[i] = (key + 5 * challenge) & 0xF;
        key = (*lut)[response[i]];

        const uint8_t sign = response[i] >> 3;
        const uint8_t magnitude = (sign != 0 ? ~response[i] : response[i]) & 0x7;
        uint8_t modifier = magnitude % 3 == 1 ? sign : 1 - sign;
        if (lut == &lut1 && (response[i] == 0x1 || response[i] == 0x9)) {
            modifier = 1;
        }
        if (lut == &lut1 && (response[i] == 0xB || response[i] == 0xE)) {
            modifier = 0;
        }
        lut = modifier != 0 ? &lut1 : &lut0;
    }

    pif_ram[0x2E] = 0;
    pif_ram[0x2F] = 0;
    for (size_t i = 0; i < cic_byte_count; i++) {
        pif_ram[cic_data_offset + i] = (response[i * 2] << 4) | response[i * 2 + 1];
    }
    pif_ram[0x3F] &= ~0x02U;
}

}

namespace ultramodern {
void send_si_message();
}

extern "C" void __osSiGetAccess_recomp(uint8_t*, recomp_context*) {}
extern "C" void __osSiRelAccess_recomp(uint8_t*, recomp_context*) {}

extern "C" void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* context) {
    const uint32_t direction = static_cast<uint32_t>(context->r4);
    const gpr dram_address = context->r5;
    if (direction == os_write) {
        for (size_t i = 0; i < pif_ram.size(); i++) {
            pif_ram[i] = MEM_BU(i, dram_address);
        }
        if ((pif_ram[0x3F] & 0x02U) != 0) {
            process_cic_6105_challenge();
        }
    }
    else if (direction == os_read) {
        for (size_t i = 0; i < pif_ram.size(); i++) {
            MEM_BU(i, dram_address) = pif_ram[i];
        }
    }
    else {
        context->r2 = static_cast<gpr>(-1);
        return;
    }

    context->r2 = 0;
    ultramodern::send_si_message();
}

extern "C" void __osSpSetStatus_recomp(uint8_t*, recomp_context*) {}

extern "C" void jfc_initialize_boot_environment(
    uint8_t* rdram, recomp_context*
) {
    MEM_W(0, 0xFFFFFFFF80000310ULL) = 6105;
}

extern "C" void jfc_boot_ram_test_6105(uint8_t*, recomp_context* context) {
    context->r2 = 1;
}

extern "C" void jfc_configure_native_heap(uint8_t* rdram, recomp_context*) {

    if (MEM_W(0, 0x80000318U) != 8 * 1024 * 1024)
        throw std::runtime_error("Native game heap requires 8 MiB RDRAM");
    MEM_B(0, 0x800A33F0U) = 1;
    std::fputs("JFC_GAME_HEAP mode=extended end=0x80600000 rdram_bytes=8388608\n", stderr);
}

extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* context) {
    MEM_B(0, context->r5) = 0;
    context->r2 = 0;
}

extern "C" void osPfsInit_recomp(uint8_t*, recomp_context* context) {
    return_no_pack(context);
}

extern "C" void __osPfsGetStatus_recomp(uint8_t*, recomp_context* context) {
    return_no_pack(context);
}

extern "C" void __osGetId_recomp(uint8_t*, recomp_context* context) {
    return_no_pack(context);
}

extern "C" void __osPfsSelectBank_recomp(uint8_t*, recomp_context* context) {
    return_no_pack(context);
}

extern "C" void __osContRamWrite_recomp(uint8_t*, recomp_context* context) {
    return_no_pack(context);
}

extern "C" void __osContRamRead_recomp(uint8_t*, recomp_context* context) {
    return_no_pack(context);
}
