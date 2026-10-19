#include "tribal_classification.hpp"
#include "jfc_recomp.h"
#include <array>
#include <stdexcept>

namespace jfc::modernization {
namespace {
alignas(64) std::array<uint64_t, 1024> classifications{};
bool ready = false;
unsigned end = 0;
}
void initialize_tribal_classifier(void (*classify)(uint8_t*, recomp_context*)) {
    if (!classify) throw std::invalid_argument("Missing native Tribal classifier");
    std::array<uint64_t, 1024> candidate{};
    unsigned candidate_end = 0;

    for (unsigned actor = 0; actor <= UINT16_MAX; ++actor) {
        recomp_context context{};
        context.r4 = actor;
        classify(nullptr, &context);
        if (context.r2 > 1) throw std::runtime_error("Invalid native Tribal classification");
        candidate[actor >> 6] |= uint64_t(context.r2) << (actor & 63);
        if (context.r2) candidate_end = actor + 1;
    }
    classifications = candidate;
    end = candidate_end;
    ready = true;
}
bool is_tribal_actor_type(unsigned int actor_type) {
    if (actor_type >= end) {
        if (!ready) throw std::logic_error("Native Tribal classifier is not initialized");
        return false;
    }
    return ((classifications[actor_type >> 6] >> (actor_type & 63)) & 1U) != 0;
}
}
