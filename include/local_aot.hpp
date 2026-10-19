#pragma once
#include <cstdint>
#include <span>
#include "local_aot_abi.h"

namespace jfc::local_aot {
void register_loaded_overlays();
bool prepare();
bool prepare(std::span<const uint8_t> validated_rom);
const JfcAotModule& module();
}
