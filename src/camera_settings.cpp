#include "camera_settings.hpp"
#include "camera_adjustments.hpp"
#include "elements/ui_button.h"

namespace jfc::camera_settings {
namespace {
constexpr const char* fov_key = "camera_fov";
constexpr const char* distance_key = "camera_distance";
void reset(const char* key, double value) {
    auto& config = recompui::config::get_graphics_config();

    config.update_option_value(key, value);
}
}
void add_options(recomp::config::Config& config) {
    config.add_number_option(fov_key, "Field of View", "Vertical field of view for ordinary gameplay. Aiming retains its zoom; wide and ultrawide screens expand the horizontal view.",
        30, 89, 1, 0, false, camera::default_fov);
    config.add_number_option(distance_key, "Camera Distance", "Moves the gameplay camera closer or farther away. 100% is the original distance; walls and scoped aiming retain native camera control.",
        50, 150, 5, 0, true, camera::default_distance);
    config.add_option_change_callback(fov_key, [](auto value, auto, auto context) {
        if (context != recomp::config::OptionChangeContext::Temporary) camera::set_fov(std::get<double>(value));
    });
    config.add_option_change_callback(distance_key, [](auto value, auto, auto context) {
        if (context != recomp::config::OptionChangeContext::Temporary) camera::set_distance(std::get<double>(value));
    });
}
void create_resets(recompui::ContextId context, recompui::Element* parent) {
    auto* group = context.create_element<recompui::Element>(parent);
    group->set_display(recompui::Display::Flex);
    group->set_flex_direction(recompui::FlexDirection::Column);
    group->set_gap(8);
    group->set_as_navigation_container(recompui::NavigationType::Vertical);
    auto* fov = context.create_element<recompui::Button>(group, "Reset Field of View", recompui::ButtonStyle::Secondary);
    fov->add_pressed_callback([] { reset(fov_key, camera::default_fov); });
    auto* distance = context.create_element<recompui::Button>(group, "Reset Camera Distance", recompui::ButtonStyle::Secondary);
    distance->add_pressed_callback([] { reset(distance_key, camera::default_distance); });
}
}
