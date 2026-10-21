#pragma once

#include <cstdint>
#include <vector>
#include "ultramodern/graphics_task_metadata.hpp"

namespace jfc::enhancements {

const char* try_graphics_task_is_frame(uint8_t* rdram, uint32_t task, bool& is_frame) noexcept;
bool graphics_task_is_frame(uint8_t* rdram, uint32_t task);
const char* try_graphics_task_framebuffer(uint8_t* rdram, uint32_t task, uint32_t& address) noexcept;
uint32_t graphics_task_framebuffer(uint8_t* rdram, uint32_t task);
ultramodern::renderer::CameraSamples graphics_task_cameras(uint8_t* rdram, uint32_t task);
ultramodern::renderer::GraphicsTaskMetadata graphics_task_metadata(uint8_t* rdram, uint32_t task);
void record_effect_matrix(uint8_t* rdram, uint32_t matrix, uint32_t object);

const char* try_publish_native_frame_time(uint8_t* rdram) noexcept;

const char* try_tag_model_bone_dma(
    uint8_t* rdram, uint32_t command, uint32_t instance, uint32_t model) noexcept;

void set_modern_controls(bool enabled);
bool modern_controls_enabled();
void set_mouse_acceleration(bool enabled);
void clear_mouse_aim();
bool physical_aim_active(int controller);

bool grounded_traversal_active(uint8_t* rdram, uint32_t player);
void sample_direct_actions(int controller, bool jump, bool crouch);
void set_joystick_sensitivity(int overall, int horizontal, int vertical);
void set_output_aspect_ratio(float aspect_ratio);
void set_cutscene_skipping(bool enabled);
void set_draw_distance_percent(int percent);
void set_fog_strength_percent(int percent);
void sample_aim(
    int controller,
    float stick_x,
    float stick_y,
    float mouse_x,
    float mouse_y);

}
