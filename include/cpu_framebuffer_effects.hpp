#pragma once

#include <cstdint>
#include <span>

namespace jfc::framebuffer_effects {
    void begin_frame(uint8_t* rdram);
    uint32_t frame_address();
    std::span<const uint32_t> pending(uint32_t cpu_address);
    void completed() noexcept;
    void invalidate_cursor() noexcept;
}
