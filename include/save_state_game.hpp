#pragma once

#include "save_state_file.hpp"
#include <memory>

namespace jfc::state {
struct GameState {
    Bytes enhancements, modernization;
};

Bytes capture_enhancements();
Bytes capture_modernization();
class PreparedEnhancements {
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend Bytes capture_enhancements();
public:
    explicit PreparedEnhancements(std::span<const uint8_t> saved);
    ~PreparedEnhancements();
    void commit() noexcept;
};
class PreparedModernization {
    struct Impl;
    std::unique_ptr<Impl> impl;
    friend Bytes capture_modernization();
public:
    explicit PreparedModernization(std::span<const uint8_t> saved);
    ~PreparedModernization();
    void commit() noexcept;
};
}
