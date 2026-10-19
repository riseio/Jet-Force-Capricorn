#pragma once
#include "recomp.h"

namespace jfc::modernization {

void initialize_tribal_classifier(void (*classify)(uint8_t*, recomp_context*));
bool is_tribal_actor_type(unsigned int actor_type);
}
