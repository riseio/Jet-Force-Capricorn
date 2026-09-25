#include <array>
#include <cstdint>

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

uint32_t controller_address_crc(uint16_t address) {
    uint32_t crc = 0;
    for (uint32_t mask = 1U << 10; mask != 0; mask >>= 1) {
        crc <<= 1;
        if ((address & mask) != 0) {
            crc = (crc & 0x20U) != 0 ? crc ^ 0x14U : crc + 1U;
        }
        else if ((crc & 0x20U) != 0) {
            crc ^= 0x15U;
        }
    }
    for (int bit = 0; bit < 5; bit++) {
        crc <<= 1;
        if ((crc & 0x20U) != 0) {
            crc ^= 0x15U;
        }
    }
    return crc & 0x1FU;
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

extern "C" void __osContAddressCrc_recomp(
    uint8_t*, recomp_context* context
) {
    context->r2 = controller_address_crc(static_cast<uint16_t>(context->r4));
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
