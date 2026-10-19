#include "camera_ownership.hpp"
#include "jfc_recomp.h"
#include <stdexcept>

extern "C" void func_8002B378(uint8_t*, recomp_context*);

namespace {
thread_local const recomp_context* query_context = nullptr;
}

extern "C" uint32_t jfc_camera_query_active(const recomp_context* ctx) {
    return ctx == query_context;
}

bool jfc::camera::attached_eligible(uint8_t* rdram, uint32_t player) {
    constexpr uint32_t player_bytes = 0x5C4;
    const uint32_t segment = player & 0xE0000000U;
    if (!rdram || (segment != 0x80000000U && segment != 0xA0000000U) ||
        (player & 0x1FFFFFFFU) > 0x800000U - player_bytes || (player & 3U))
        return false;
    if (query_context) throw std::logic_error("Recursive native camera ownership query");

    recomp_context ctx{};
    ctx.r5 = int32_t(player);
    ctx.r29 = int32_t(player);
    struct Scope {
        explicit Scope(const recomp_context* ctx) { query_context = ctx; }
        ~Scope() { query_context = nullptr; }
    } scope(&ctx);
    ctx.r2 = -1;
    func_8002B378(rdram, &ctx);
    if (ctx.r2 != 0 && ctx.r2 != 1)
        throw std::runtime_error("Native camera query did not reach an ownership boundary");
    return ctx.r2 != 0;
}
