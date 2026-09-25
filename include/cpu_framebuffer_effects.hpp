#pragma once

#include <cstdint>
#include <span>

namespace jfc::framebuffer_effects {
    std::span<const uint32_t> pending();
    void completed() noexcept;
}
