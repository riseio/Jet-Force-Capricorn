#include "camera_adjustments.hpp"
#include "jfc_recomp.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <numbers>
#include <limits>
#include <stdexcept>

namespace {
std::atomic<float> lens_scale{1.0f}, distance_scale{1.0f}, height_offset{0.0f};
constexpr float radians = std::numbers::pi_v<float> / 180.0f;
bool valid(uint32_t address, uint32_t bytes) {
    return (address & 0xE0000000U) == 0x80000000U && !(address & 3U) &&
        bytes <= 0x800000U && (address & 0x1FFFFFFFU) <= 0x800000U - bytes;
}
float player_distance_scale(uint8_t* rdram, uint32_t player) {
    const float scale = distance_scale.load(std::memory_order_relaxed);
    if (scale == 1.0f) return scale;
    if (!rdram || !valid(player, 4)) throw std::runtime_error("Invalid gameplay camera owner");
    return MEM_BU(MEM_BU(0, player) & 3U, 0x800A3174U) ? 1.0f : scale;
}
}

void jfc::camera::set_fov(double degrees) {
    degrees = std::clamp(std::isfinite(degrees) ? degrees : default_fov, 30.0, 89.0);
    lens_scale.store(degrees == default_fov ? 1.0f :
        std::tan(float(degrees) * radians / 2) / std::tan(float(default_fov) * radians / 2),
        std::memory_order_relaxed);
}

void jfc::camera::set_distance(double percent) {
    distance_scale.store(float(std::clamp(std::isfinite(percent) ? percent : default_distance,
        min_distance, max_distance) / 100.0), std::memory_order_relaxed);
}

void jfc::camera::set_height(double units) {
    height_offset.store(float(std::clamp(std::isfinite(units) ? units : default_height,
        min_height, max_height)), std::memory_order_relaxed);
}

extern "C" float jfc_adjust_camera_height(uint8_t* rdram, uint32_t player, float authored) {
    const float offset = height_offset.load(std::memory_order_relaxed);
    if (offset == 0.0f) return authored;
    if (!rdram || !valid(player, 4)) throw std::runtime_error("Invalid gameplay camera height owner");
    if (MEM_BU(MEM_BU(0, player) & 3U, 0x800A3174U)) return authored;
    const float adjusted = authored + offset;
    if (!std::isfinite(adjusted)) throw std::runtime_error("Non-finite gameplay camera height");
    return adjusted;
}

extern "C" float jfc_adjust_camera_fov(float authored) {
    const float scale = lens_scale.load(std::memory_order_relaxed);
    if (scale == 1.0f) return authored;
    if (!std::isfinite(authored) || authored <= 0 || authored >= 90)
        throw std::runtime_error("Invalid authored gameplay camera lens");
    return std::clamp(2 * std::atan(std::tan(authored * radians / 2) * scale) / radians, 1.0f, 89.0f);
}

extern "C" void jfc_adjust_camera_distance(uint8_t* rdram, uint32_t player,
                                            uint32_t target, uint32_t components) {
    const float scale = player_distance_scale(rdram, player);
    if (scale == 1.0f) return;
    if (!rdram || !valid(player, 4) || (components != 1 && components != 3) || !valid(target, components * 4))
        throw std::runtime_error("Invalid gameplay camera distance owner");

    for (uint32_t i = 0; i < components; ++i) {
        const float value = std::bit_cast<float>(uint32_t(MEM_W(i * 4, target))) * scale;
        if (!std::isfinite(value)) throw std::runtime_error("Non-finite gameplay camera distance");
        MEM_W(i * 4, target) = std::bit_cast<uint32_t>(value);
    }
}

extern "C" float jfc_camera_distance_units(uint8_t* rdram, uint32_t player, float radius) {
    return radius / player_distance_scale(rdram, player);
}

extern "C" float jfc_camera_distance_gain(uint8_t* rdram, uint32_t player, float gain) {

    return player_distance_scale(rdram, player) == 1.0f ? gain : std::max(gain, 0.1f);
}

extern "C" uint32_t jfc_culling_vertex_x(uint32_t x, uint32_t z,
                                        uint32_t apex_z, float tangent) {
    const float authored = std::bit_cast<float>(x);
    const float depth = std::bit_cast<float>(apex_z) - std::bit_cast<float>(z);
    if (!std::isfinite(authored) || !std::isfinite(depth) || depth < 0 ||
        !std::isfinite(tangent) || tangent <= 0) {
        throw std::runtime_error("Invalid camera visibility-plane input");
    }

    const float required = depth * tangent;
    if (!std::isfinite(required)) throw std::runtime_error("Camera visibility-plane overflow");
    if (std::abs(authored) >= required || authored == 0) return x;
    const float expanded = std::nextafter(required, std::numeric_limits<float>::infinity());
    if (!std::isfinite(expanded)) throw std::runtime_error("Camera visibility-plane overflow");
    return std::bit_cast<uint32_t>(std::copysign(expanded, authored));
}
