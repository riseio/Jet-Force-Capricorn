#pragma once
#include <stddef.h>
#include <stdint.h>
#include "librecomp/sections.h"

typedef struct {
    uint32_t version;
    uint32_t context_size;
    uint32_t context_status_offset;
    uint32_t section_entry_size;
    const char* identity;
    void (*const* functions)(void);
    size_t function_count;
    uint8_t* rsp_dmem;
    uint16_t* rsp_reciprocals;
    uint16_t* rsp_inverse_roots;
    int32_t** section_addresses_owner;
    uint32_t (*read_cop1)(void);
    void (*write_cop1)(uint32_t);
    int32_t (*cvt_w_s)(float);
    int32_t (*cvt_w_d)(double);
    int64_t (*cvt_l_s)(float);
    int64_t (*cvt_l_d)(double);
} JfcAotHost;

typedef struct {
    SectionTableEntry* sections;
    size_t section_count;
    size_t total_sections;
    int* overlays;
    size_t overlay_count;
    recomp_func_t* const* functions;
    size_t function_count;
    int (*audio)(uint8_t*, uint32_t);
    const uint8_t* effect_spirv;
    size_t effect_spirv_size;
    const uint8_t* effect_dxil;
    size_t effect_dxil_size;
} JfcAotModule;

typedef int (*JfcAotInitialize)(const JfcAotHost*, JfcAotModule*);
