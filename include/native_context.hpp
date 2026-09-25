#pragma once
#include "recomp.h"

namespace jfc::runtime {

inline void copy_context(recomp_context& destination, const recomp_context& source) {
    destination = source;
    destination.f_odd = destination.mips3_float_mode
        ? &destination.f1.u32l : &destination.f0.u32h;
}
}
