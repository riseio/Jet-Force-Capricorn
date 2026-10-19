#pragma once
#include <stdint.h>
#include <emmintrin.h>
#include "local_aot_abi.h"
#if !defined(__x86_64__) && !defined(_M_X64)
#error Local native compilation supports x86-64 hosts only
#endif

static inline int32_t jfc_aot_cvt_w_s(const JfcAotHost* host,float value) {
#ifdef _WIN32
    if(value>=-0x1p31f && value<0x1p31f) return _mm_cvtss_si32(_mm_set_ss(value));
#else
    if(value>=-0x1p63f && value<0x1p63f) return (int32_t)_mm_cvtss_si64(_mm_set_ss(value));
#endif
    return host->cvt_w_s(value);
}
static inline int32_t jfc_aot_cvt_w_d(const JfcAotHost* host,double value) {
#ifdef _WIN32
    if(value>=-0x1p31 && value<=2147483647.0) return _mm_cvtsd_si32(_mm_set_sd(value));
#else
    if(value>=-0x1p63 && value<0x1p63) return (int32_t)_mm_cvtsd_si64(_mm_set_sd(value));
#endif
    return host->cvt_w_d(value);
}
static inline int64_t jfc_aot_cvt_l_s(const JfcAotHost* host,float value) {
    if(value>=-0x1p63f && value<0x1p63f) return _mm_cvtss_si64(_mm_set_ss(value));
    return host->cvt_l_s(value);
}
static inline int64_t jfc_aot_cvt_l_d(const JfcAotHost* host,double value) {
    if(value>=-0x1p63 && value<0x1p63) return _mm_cvtsd_si64(_mm_set_sd(value));
    return host->cvt_l_d(value);
}
