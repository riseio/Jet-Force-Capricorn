#ifndef JFC_PATCH_HELPERS_H
#define JFC_PATCH_HELPERS_H

#ifdef MIPS
#include "ultra64.h"
#else
#include "recomp.h"
#endif

#ifdef __cplusplus
#define JFC_EXTERN_C extern "C"
#else
#define JFC_EXTERN_C
#endif

#ifdef MIPS
#define DECLARE_FUNC(type, name, ...) JFC_EXTERN_C type name(__VA_ARGS__)
#else
#define DECLARE_FUNC(type, name, ...) \
    JFC_EXTERN_C void name(uint8_t* rdram, recomp_context* ctx)
#endif

#endif
