#include "cpu_framebuffer_effects.hpp"
#include "jfc_recomp.h"
#include "include/rt64_cpu_framebuffer_effects.h"

#include <stdexcept>

namespace {

    RT64::CpuFramebufferEffects::Builder effects;
    uint32_t imageAddress = 0;
    uint32_t imagePixels = 0;
    bool active = false;
    struct RefractionSample { uint32_t source; uint16_t tint; bool constant; };
    std::vector<RefractionSample> refractionSamples;
    uint32_t refractionFramebuffer = 0;
    bool refractionCaptured = false;

    uint32_t physical(uint32_t address, uint32_t bytes) {
        const uint32_t segment = address & 0xE0000000U;
        const uint32_t offset = address & 0x1FFFFFFFU;
        if ((segment != 0x80000000U && segment != 0xA0000000U) ||
            bytes > 0x800000U || offset > 0x800000U - bytes || (offset & 1U))
            throw std::invalid_argument("CPU framebuffer effect has invalid native memory range");
        return offset;
    }

    bool imagePixel(uint32_t address, uint32_t &index) {
        if (!active) return false;
        const uint32_t offset = physical(address, 2);
        if (offset < imageAddress || (offset - imageAddress) / 2 >= imagePixels)
            return false;
        index = (offset - imageAddress) / 2;
        return true;
    }
}

extern "C" void jfc_cpu_effect_begin(uint8_t*, uint32_t address, uint32_t width, uint32_t height) {
    if (active) throw std::logic_error("CPU framebuffer effect image was not completed");
    const uint64_t pixels = uint64_t(width) * height;
    if (pixels == 0 || pixels > 0x400000U)
        throw std::invalid_argument("CPU framebuffer effect image dimensions exceed RDRAM");
    imageAddress = physical(address, uint32_t(pixels) * 2);
    imagePixels = uint32_t(pixels);
    effects.begin(width, height);
    refractionFramebuffer = 0;
    refractionCaptured = false;
    active = true;
}

extern "C" void jfc_cpu_effect_rain(uint32_t address, uint32_t color, uint32_t shift, uint32_t opaque) {
    uint32_t index;
    if (imagePixel(address, index))
        effects.rain(index, uint16_t(color), uint8_t(shift), opaque != 0);
}

extern "C" void jfc_cpu_effect_add(uint32_t address, int32_t r, int32_t g, int32_t b, uint32_t alpha) {
    uint32_t index;
    if (imagePixel(address, index)) effects.add(index, r, g, b, alpha != 0);
}

extern "C" void jfc_cpu_effect_refraction(uint8_t* rdram, uint32_t descriptors, uint32_t framebuffer) {
    if (!active) return;

    constexpr uint32_t Pixels = 320U * 240U;
    physical(descriptors, Pixels * 2);
    physical(framebuffer, Pixels * 2);
    refractionFramebuffer = physical(framebuffer, Pixels * 2);
    refractionCaptured = true;
    refractionSamples.resize(Pixels);
    for (uint32_t i = 0; i < Pixels; ++i) {
        const uint32_t d = uint16_t(MEM_H(i * 2, descriptors));
        if (d == 0) {
            refractionSamples[i] = {0, 0, true};
            continue;
        }
        const int32_t delta = (int32_t(d >> 11) - 16) +
            (int32_t((d >> 1) & 31U) - 16) * 320;
        const uint32_t source = framebuffer + i * 2 + uint32_t(delta * 2);
        const uint16_t tint = uint16_t((d & 0x7C0U) + ((d & 0x7C0U) >> 5));
        uint32_t input;
        if (imagePixel(source, input)) refractionSamples[i] = {input, tint, false};
        else {
            physical(source, 2);
            refractionSamples[i] = {uint16_t(MEM_H(0, source)), tint, true};
        }
    }
}

extern "C" void jfc_cpu_effect_refraction_copy(uint32_t destination, uint32_t bytes) {
    if (!active) return;
    const uint32_t offset = physical(destination, bytes);
    if (!refractionCaptured || offset < refractionFramebuffer || (bytes & 3U) ||
        (offset - refractionFramebuffer) / 2 > refractionSamples.size() ||
        bytes / 2 > refractionSamples.size() - (offset - refractionFramebuffer) / 2)
        throw std::invalid_argument("CPU refraction copy does not own its sampled image");
    const uint32_t first = (offset - refractionFramebuffer) / 2;

    for (uint32_t i = 0; i < bytes / 2; ++i) {
        uint32_t output;
        if (imagePixel(destination + i * 2, output)) {
            const auto &sample = refractionSamples[first + i];
            effects.refract(output, sample.source, sample.tint, sample.constant);
        }
    }
}

std::span<const uint32_t> jfc::framebuffer_effects::pending() {
    if (!active) return {};
    return {effects.data(), effects.size()};
}

void jfc::framebuffer_effects::completed() noexcept {
    active = false;
}
