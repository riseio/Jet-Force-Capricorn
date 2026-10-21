#include "cpu_framebuffer_effects.hpp"
#include "jfc_recomp.h"
#include "include/rt64_cpu_framebuffer_effects.h"

#include <stdexcept>
#include <array>
#include <atomic>

namespace {

    RT64::CpuFramebufferEffects::Builder effects;
    uint32_t imageAddress = 0;
    uint32_t imagePixels = 0;
    uint32_t imageWidth = 0, imageHeight = 0;
    bool active = false;
    uint32_t frameAddress = 0, frameWidth = 0, frameHeight = 0;
    bool frameActive = false;
    std::vector<jfc_refraction_sample> refractionSamples;
    uint32_t refractionFramebuffer = 0;
    bool refractionCaptured = false;
    constexpr uint32_t lineBase = 0x80102DC0U, lineCapacity = 148, lineStride = 8;
    constexpr uint32_t textBase = 0x80103700U, textCapacity = 16, textStride = 36;
    struct CursorKey {
        uint32_t player = 0, level = 0, weapon = 0, character = 0, view = 0, scope = 0;
        uint32_t width = 0, height = 0;
        bool operator==(const CursorKey&) const = default;
    };
    struct CursorTarget {
        CursorKey key;
        int32_t x = 0, y = 0;
        uint32_t lineFirst = 0, lineEnd = 0, textFirst = 0, textEnd = 0;
        bool valid = false, ambiguous = false;
    };
    std::array<CursorTarget, 2> queuedTargets;
    CursorTarget constructingTarget, outputTarget, previousTarget;
    uint32_t constructingBuffer = 0, outputBuffer = 0;
    bool constructing = false, outputActive = false, cursorOutput = false, cursorDrawn = false;
    uint64_t frameSerial = 0, previousCursorFrame = 0;
    std::atomic_uint64_t cursorRevision{0};
    uint64_t observedCursorRevision = 0;

    void synchronizeCursor() {
        const auto revision = cursorRevision.load(std::memory_order_relaxed);
        if (revision == observedCursorRevision) return;
        queuedTargets = {}; constructing = outputActive = cursorOutput = cursorDrawn = false;
        previousTarget = {}; previousCursorFrame = 0;
        observedCursorRevision = revision;
    }

    uint32_t queueBuffer(uint8_t* rdram) {
        if (!rdram) throw std::invalid_argument("Missing native cursor memory");
        const auto buffer = uint32_t(MEM_W(0, 0x80103B90U));
        if (buffer > 1) throw std::invalid_argument("Invalid native cursor queue bank");
        return buffer;
    }

    uint32_t queueCount(uint8_t* rdram, uint32_t buffer, bool text) {
        const auto count = uint32_t(MEM_W(buffer * 4, text ? 0x80103B88U : 0x80103B80U));
        if (count > (text ? textCapacity : lineCapacity))
            throw std::invalid_argument("Invalid native cursor queue extent");
        return count;
    }

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

extern "C" void jfc_cpu_cursor_begin(uint8_t* rdram, uint32_t player, int32_t x, int32_t y,
        uint32_t weapon, uint32_t enabled) {
    synchronizeCursor();
    if (constructing) throw std::logic_error("Native cursor producer overlaps its owner");
    constructingTarget = {};
    constructingBuffer = queueBuffer(rdram);
    constructing = true;
    if (!enabled || !frameActive) return;
    physical(player, 0x569);
    if (MEM_B(0, player) != 0 || MEM_BU(1, player) > 2) return;
    if (weapon > 12 || x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX)
        throw std::invalid_argument("Invalid native target cursor");
    auto &target = constructingTarget;
    target.key = {player, uint32_t(MEM_HU(0, 0x800A323CU)), weapon, MEM_BU(1, player),
        MEM_BU(0x568, player), MEM_BU(0, 0x800A3174U), frameWidth, frameHeight};
    target.x = x; target.y = y;
    target.lineFirst = queueCount(rdram, constructingBuffer, false);
    target.textFirst = queueCount(rdram, constructingBuffer, true);
    target.valid = true;
}

extern "C" void jfc_cpu_cursor_end(uint8_t* rdram) {
    if (!constructing) throw std::logic_error("Native cursor producer has no owner");
    if (queueBuffer(rdram) != constructingBuffer)
        throw std::logic_error("Native cursor producer changed queue banks");
    if (constructingTarget.valid) {
        auto &target = queuedTargets[constructingBuffer];
        if (target.valid) target.ambiguous = true;
        else {
            target = constructingTarget;
            target.lineEnd = queueCount(rdram, constructingBuffer, false);
            target.textEnd = queueCount(rdram, constructingBuffer, true);
            if (target.lineEnd < target.lineFirst || target.textEnd < target.textFirst)
                throw std::logic_error("Native cursor producer rewound its queue");
        }
    }
    constructing = false;
}

extern "C" void jfc_cpu_cursor_output_begin(uint8_t* rdram) {
    synchronizeCursor();
    if (outputActive) throw std::logic_error("Native cursor replay overlaps its owner");
    outputActive = true;
    outputBuffer = queueBuffer(rdram) ^ 1U;
    outputTarget = queuedTargets[outputBuffer];
    cursorOutput = active && frameActive && outputTarget.valid && !outputTarget.ambiguous &&
        outputTarget.key.width == imageWidth && outputTarget.key.height == imageHeight;
    if (!cursorOutput) return;
    if (outputTarget.lineEnd > queueCount(rdram, outputBuffer, false) ||
        outputTarget.textEnd > queueCount(rdram, outputBuffer, true))
        throw std::logic_error("Native cursor replay lost queued records");
    const bool continuous = previousTarget.valid && previousCursorFrame + 1 == frameSerial &&
        previousTarget.key == outputTarget.key;
    effects.setCursorMotion(continuous ? previousTarget.x - outputTarget.x : 0,
        continuous ? previousTarget.y - outputTarget.y : 0);
}

extern "C" void jfc_cpu_cursor_line(uint32_t address) {
    if (!cursorOutput) return;
    const uint32_t base = lineBase + outputBuffer * lineCapacity * lineStride;
    if (address < base || address - base >= lineCapacity * lineStride || (address - base) % lineStride)
        throw std::invalid_argument("Native cursor line has no queue owner");
    const uint32_t index = (address - base) / lineStride;
    effects.setAdditionLayer(index < outputTarget.lineFirst ? 0U : index < outputTarget.lineEnd ? 1U : 2U);
}

extern "C" void jfc_cpu_cursor_text(uint32_t address) {
    if (!cursorOutput) return;
    const uint32_t base = textBase + outputBuffer * textCapacity * textStride;
    if (address < base || address - base >= textCapacity * textStride || (address - base) % textStride)
        throw std::invalid_argument("Native cursor text has no queue owner");
    const uint32_t index = (address - base) / textStride;
    effects.setAdditionLayer(index < outputTarget.textFirst ? 2U : index < outputTarget.textEnd ? 3U : 4U);
}

extern "C" void jfc_cpu_cursor_output_end(void) {
    if (!outputActive) throw std::logic_error("Native cursor replay has no owner");
    queuedTargets[outputBuffer] = {};
    if (cursorOutput) {
        previousTarget = outputTarget; previousCursorFrame = frameSerial;
        cursorDrawn = true;
        effects.setAdditionLayer(0);
    }
    outputActive = cursorOutput = false;
}

extern "C" void jfc_cpu_cursor_clear(void) {
    queuedTargets = {}; previousTarget = {}; constructingTarget = {};
    constructing = outputActive = cursorOutput = cursorDrawn = false; previousCursorFrame = 0;
}

extern "C" void jfc_cpu_effect_begin(uint8_t*, uint32_t address, uint32_t width, uint32_t height) {
    if (active) throw std::logic_error("CPU framebuffer effect image was not completed");
    const uint64_t pixels = uint64_t(width) * height;
    if (pixels == 0 || pixels > 0x400000U)
        throw std::invalid_argument("CPU framebuffer effect image dimensions exceed RDRAM");
    imageAddress = physical(address, uint32_t(pixels) * 2);
    imagePixels = uint32_t(pixels);
    imageWidth = width;
    imageHeight = height;
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

extern "C" jfc_refraction_capture jfc_cpu_effect_refraction_begin(
        uint32_t descriptors, uint32_t framebuffer, uint32_t bytes) {
    if (!active) return {nullptr, 0};
    if (bytes == 0 || (bytes & 3U))
        throw std::invalid_argument("CPU refraction sample span must contain complete words");
    physical(descriptors, bytes);
    refractionFramebuffer = physical(framebuffer, bytes);
    refractionCaptured = true;

    refractionSamples.assign(bytes / 2, {});
    return {refractionSamples.data(), uint32_t(refractionSamples.size())};
}

extern "C" void jfc_cpu_effect_refraction_bounds_error(void) {
    throw std::out_of_range("Native refraction sample exceeds its captured span");
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
            uint32_t source;
            const bool constant = sample.address == 0 || !imagePixel(sample.address, source);
            effects.refract(output, constant ? sample.value : source, sample.tint, constant);
        }
    }
}

void jfc::framebuffer_effects::begin_frame(uint8_t* rdram) {
    if (!rdram || frameActive || active)
        throw std::logic_error("Rendered framebuffer ownership is not available");
    const uint32_t mode = MEM_BU(0, 0x800FECA8U);
    if (mode >= 15U) throw std::invalid_argument("Invalid native framebuffer video mode");
    const uint32_t row = 0x800A392CU + mode * 40U;
    const uint32_t width = MEM_W(0, row), height = MEM_W(4, row);
    const uint64_t pixels = uint64_t(width) * height;
    if (!width || !height || pixels > 0x400000U)
        throw std::invalid_argument("Invalid native framebuffer dimensions");
    const uint32_t address = physical(MEM_W(0, 0x800FECB0U), uint32_t(pixels) * 2U);
    frameAddress = address;
    frameWidth = width;
    frameHeight = height;
    frameActive = true;
    synchronizeCursor();
    cursorDrawn = false;
    if (++frameSerial == 0) { frameSerial = 1; previousTarget = {}; }
}

uint32_t jfc::framebuffer_effects::frame_address() {
    if (!frameActive) throw std::logic_error("Rendered framebuffer has no frame owner");
    return frameAddress;
}

std::span<const uint32_t> jfc::framebuffer_effects::pending(uint32_t cpu_address) {
    if (!frameActive) throw std::logic_error("CPU completion has no rendered frame owner");
    if (!active) return {};
    if (imageAddress != physical(cpu_address, imagePixels * 2U))
        throw std::logic_error("CPU completion changed its effect image owner");

    if (imageAddress != frameAddress || imageWidth != frameWidth || imageHeight != frameHeight) {
        previousTarget = {};
        return {};
    }
    effects.sealCursor();
    return {effects.data(), effects.size()};
}

void jfc::framebuffer_effects::completed() noexcept {
    if (!cursorDrawn) previousTarget = {};
    active = false;
    frameActive = false;
}

void jfc::framebuffer_effects::invalidate_cursor() noexcept {
    cursorRevision.fetch_add(1, std::memory_order_relaxed);
}
