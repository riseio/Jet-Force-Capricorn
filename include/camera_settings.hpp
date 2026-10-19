#pragma once
#include "recompui/config.h"

namespace jfc::camera_settings {
void add_options(recomp::config::Config& config);
void create_resets(recompui::ContextId context, recompui::Element* parent);
}
