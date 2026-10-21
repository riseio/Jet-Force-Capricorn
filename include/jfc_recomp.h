#ifndef JFC_RECOMP_H
#define JFC_RECOMP_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#define JFC_CAMERA_THROWS noexcept(false)
#else
#define JFC_CAMERA_THROWS
#endif

void osInvalICache_recomp(uint8_t* rdram, recomp_context* ctx);
void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx);
uint32_t jfc_camera_query_active(const recomp_context* ctx);
uint32_t jfc_state_requested(void);
void jfc_state_root(uint8_t* rdram, recomp_context* ctx, uint32_t root, uint64_t* locals) JFC_CAMERA_THROWS;
typedef struct jfc_actor_profile {
    uint64_t start_ns;
    uint32_t object;
    uint32_t type;
    uint32_t level;
} jfc_actor_profile;
jfc_actor_profile jfc_profile_actor_begin(uint8_t* rdram, uint32_t object);
void jfc_profile_actor_end(jfc_actor_profile scope, const char* name);
void jfc_unregister_overlay(uint8_t* rdram, recomp_context* ctx);
void jfc_register_resumed_overlay(uint8_t* rdram, recomp_context* ctx);
uint32_t jfc_restore_registered_runlink_header(
    uint8_t* rdram, recomp_context* ctx);
uint32_t jfc_validate_runlink_resume_target(
    uint8_t* rdram, recomp_context* ctx);
void jfc_prepare_matrix_tag(uint32_t id);
void jfc_use_model_matrix(uint8_t* rdram, recomp_context* ctx);
void jfc_tag_last_matrix(uint8_t* rdram, recomp_context* ctx);
void jfc_record_allocation(uint8_t* rdram, uint32_t slot, uint32_t size, uint32_t taken) JFC_CAMERA_THROWS;
void jfc_record_release(uint8_t* rdram, uint32_t slot) JFC_CAMERA_THROWS;
void jfc_record_effect_birth(uint32_t address, uint32_t bytes) JFC_CAMERA_THROWS;
void jfc_record_texture_scroll(uint32_t address, uint32_t count, uint32_t wrap_s, uint32_t wrap_t,
    int32_t delta_s, int32_t delta_t);
void jfc_record_object_texture_scroll(uint8_t* rdram, recomp_context* ctx);
uint64_t jfc_effect_generation(uint32_t address) JFC_CAMERA_THROWS;
void jfc_set_tracer_owner(uint32_t address, uint32_t segment) JFC_CAMERA_THROWS;
void jfc_tag_procedural_sky(uint8_t* rdram, recomp_context* ctx);

void jfc_tag_camera_matrix(uint8_t* rdram, uint32_t command) JFC_CAMERA_THROWS;
void jfc_begin_camera_pass(uint32_t producer) JFC_CAMERA_THROWS;
void jfc_capture_camera_sample(uint8_t* rdram, uint32_t role) JFC_CAMERA_THROWS;
void jfc_copy_camera_sample(uint8_t* rdram, uint32_t destination) JFC_CAMERA_THROWS;
void jfc_tag_saved_camera_matrix(uint8_t* rdram, uint32_t command) JFC_CAMERA_THROWS;
void jfc_camera_lifecycle_changed(void) JFC_CAMERA_THROWS;
void jfc_configure_native_heap(uint8_t* rdram, recomp_context* ctx) JFC_CAMERA_THROWS;
void jfc_bound_font_tiles(uint8_t* rdram, uint32_t definition) JFC_CAMERA_THROWS;
void jfc_ui_alpha_scope(uint8_t* rdram, uint32_t enter) JFC_CAMERA_THROWS;
void jfc_ui_alpha_scope_at(uint8_t* rdram, uint32_t cursor_pointer, uint32_t enter) JFC_CAMERA_THROWS;
float jfc_adjust_camera_fov(float authored) JFC_CAMERA_THROWS;
float jfc_adjust_camera_height(uint8_t* rdram, uint32_t player, float authored) JFC_CAMERA_THROWS;
float jfc_modern_aim_response(uint8_t* rdram, uint32_t player, float authored) JFC_CAMERA_THROWS;
void jfc_cpu_cursor_begin(uint8_t* rdram, uint32_t player, int32_t x, int32_t y,
    uint32_t weapon, uint32_t enabled) JFC_CAMERA_THROWS;
void jfc_cpu_cursor_end(uint8_t* rdram) JFC_CAMERA_THROWS;
void jfc_cpu_cursor_output_begin(uint8_t* rdram) JFC_CAMERA_THROWS;
void jfc_cpu_cursor_line(uint32_t address) JFC_CAMERA_THROWS;
void jfc_cpu_cursor_text(uint32_t address) JFC_CAMERA_THROWS;
void jfc_cpu_cursor_output_end(void) JFC_CAMERA_THROWS;
void jfc_cpu_cursor_clear(void) JFC_CAMERA_THROWS;
int jfc_should_interpolate_target_reticle(void);
void jfc_adjust_camera_distance(uint8_t* rdram, uint32_t player, uint32_t target, uint32_t components) JFC_CAMERA_THROWS;
float jfc_camera_distance_units(uint8_t* rdram, uint32_t player, float radius) JFC_CAMERA_THROWS;
float jfc_camera_distance_gain(uint8_t* rdram, uint32_t player, float gain) JFC_CAMERA_THROWS;
float jfc_culling_horizontal_tangent(uint8_t* rdram) JFC_CAMERA_THROWS;
uint32_t jfc_culling_vertex_x(uint32_t x, uint32_t z, uint32_t apex_z, float tangent) JFC_CAMERA_THROWS;
#undef JFC_CAMERA_THROWS
void jfc_publish_native_frame_time(uint8_t* rdram);
void jfc_complete_cpu_framebuffer(uint8_t* rdram, recomp_context* ctx, uint32_t address);
void jfc_cpu_effect_begin(uint8_t* rdram, uint32_t address, uint32_t width, uint32_t height);
void jfc_cpu_effect_rain(uint32_t address, uint32_t color, uint32_t shift, uint32_t opaque);
void jfc_cpu_effect_add(uint32_t address, int32_t r, int32_t g, int32_t b, uint32_t alpha);
typedef struct jfc_refraction_sample {
    uint32_t address;
    uint16_t value;
    uint16_t tint;
} jfc_refraction_sample;
typedef struct jfc_refraction_capture {
    jfc_refraction_sample* samples;
    uint32_t count;
} jfc_refraction_capture;
jfc_refraction_capture jfc_cpu_effect_refraction_begin(uint32_t descriptors, uint32_t framebuffer, uint32_t bytes);
void jfc_cpu_effect_refraction_bounds_error(void);
void jfc_cpu_effect_refraction_copy(uint32_t destination, uint32_t bytes);
void jfc_wait_render_admission(uint8_t* rdram, recomp_context* ctx);
int jfc_prepare_world_tracer(uint8_t* rdram, recomp_context* ctx);
void jfc_validate_tracer_buffer(uint32_t address, uint32_t bytes, uint32_t alignment);
void jfc_set_sprite_owner(uint32_t object);
void jfc_tag_owned_sprite_matrix(uint8_t* rdram, recomp_context* ctx);
void jfc_tag_model_bone_dma(uint8_t* rdram, uint32_t command, uint32_t instance, uint32_t model);
void jfc_request_cutscene_skip(uint8_t* rdram, recomp_context* ctx);
void jfc_request_travel_skip(uint8_t* rdram, recomp_context* ctx);
void jfc_apply_cutscene_skip(uint8_t* rdram, recomp_context* ctx);
void jfc_apply_object_cutscene_skip(recomp_context* ctx);
void jfc_begin_single_player_hud(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_single_player_health_hud(uint8_t* rdram, recomp_context* ctx);
void jfc_observe_weapon_selection_overlay(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t character,
    uint32_t animation_x_bits);
void jfc_finish_weapon_selection_overlay(
    uint8_t* rdram, recomp_context* ctx);
void jfc_begin_single_player_weapon_panel(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_single_player_weapon_panel_content(
    uint8_t* rdram, recomp_context* ctx);
void jfc_end_single_player_weapon_panel(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_queued_pickup_scissor(uint8_t* rdram, recomp_context* ctx);
void jfc_align_queued_pickup_text(uint8_t* rdram, recomp_context* ctx);
void jfc_end_queued_pickup_text(uint8_t* rdram, recomp_context* ctx);
void jfc_end_single_player_hud(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_hud_number_filter(uint8_t* rdram, recomp_context* ctx);
void jfc_end_hud_number_filter(uint8_t* rdram, recomp_context* ctx);
void jfc_adapt_native_prompt_text(uint8_t* rdram, recomp_context* ctx);
void jfc_adapt_native_prompt_width_text(uint8_t* rdram, recomp_context* ctx);
void jfc_hide_legacy_save_prompt_icon(uint8_t* rdram, recomp_context* ctx);
void jfc_install_title_logo(uint8_t* rdram);
void jfc_begin_transition_mask(uint8_t* rdram, uint32_t cursor_pointer);
void jfc_begin_transition_mask_kind(
    uint8_t* rdram, uint32_t cursor_pointer, uint32_t kind);
void jfc_end_transition_mask(uint8_t* rdram, uint32_t cursor_pointer);
void jfc_request_instant_pause_menu_tick(uint8_t* rdram, recomp_context* ctx);
void jfc_route_pause_settings(recomp_context* ctx);
int jfc_draw_modern_health(uint8_t* rdram, recomp_context* ctx);
void jfc_free_counter_icons(uint8_t* rdram, recomp_context* ctx);
void jfc_preserve_frontend_model_textures(uint8_t* rdram, recomp_context* ctx);
void jfc_draw_room_counters(uint8_t* rdram, recomp_context* ctx);
void jfc_invalidate_hud_metadata(void);
void jfc_apply_instant_pause_menu_tick(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_fullscreen_fade(uint8_t* rdram, recomp_context* ctx);
void jfc_end_fullscreen_fade(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_starfield_batch(uint8_t* rdram, recomp_context* ctx);
void jfc_end_starfield_batch(uint8_t* rdram, recomp_context* ctx);
void jfc_route_modern_input(uint8_t* rdram, recomp_context* ctx);
uint32_t jfc_allow_player_weapon_cycle(uint8_t* rdram, uint32_t player);
void jfc_route_interaction_buttons(recomp_context* ctx);
void jfc_select_modern_control_table(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_modern_player_control(uint8_t* rdram, recomp_context* ctx);
void jfc_apply_modern_l2_movement_and_vertical_look(
    uint8_t* rdram,
    recomp_context* ctx,
    recomp_func_t* fine_aim_handler,
    uint32_t object,
    uint32_t player,
    int32_t movement_delta,
    int32_t heading,
    uint32_t timestep);
void jfc_prepare_modern_native_fine_aim(uint8_t* rdram, uint32_t player);
int32_t jfc_modern_aim_entry_pitch(uint8_t* rdram, uint32_t player, int32_t native_pitch);
uint32_t jfc_should_skip_borrowed_state_zero(uint8_t* rdram, uint32_t player);
void jfc_restore_modern_action_momentum(uint8_t* rdram, uint32_t player);
void jfc_prepare_modern_airborne_travel(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_modern_flight_steering(uint8_t* rdram, recomp_context* ctx, uint32_t player, uint32_t timestep_bits);
void jfc_end_modern_flight_steering(uint8_t* rdram, recomp_context* ctx, uint32_t player);
void jfc_prepare_modern_grounded_action_travel(
    uint8_t* rdram,
    recomp_context* ctx,
    uint32_t player,
    uint32_t timestep_bits);
void jfc_select_modern_action_travel_heading(
    uint8_t* rdram, recomp_context* ctx, uint32_t player);
void jfc_select_modern_action_projection_basis(
    uint8_t* rdram, recomp_context* ctx, uint32_t player);
void jfc_validate_modern_action_travel(
    uint8_t* rdram, recomp_context* ctx, uint32_t player);
void jfc_finalize_modern_character_dispatch(
    uint8_t* rdram, uint32_t player);
int32_t jfc_modern_locomotion_pose_heading(
    uint8_t* rdram, uint32_t player, int32_t native_heading);
void jfc_end_modern_player_control(uint8_t* rdram, recomp_context* ctx);
void jfc_adjust_modern_manual_aim_rate(uint8_t* rdram, recomp_context* ctx);
void jfc_select_modern_scope(uint8_t* rdram, recomp_context* ctx);
int jfc_scope_input_active(uint8_t* rdram, uint32_t player, int native_eligible);
void jfc_finalize_modern_manual_aim(uint8_t* rdram, recomp_context* ctx);
void jfc_suppress_modern_level_camera(uint8_t* rdram, recomp_context* ctx);
void jfc_select_modern_attached_camera(uint8_t* rdram, recomp_context* ctx);
void jfc_prepare_target_reticle(uint8_t* rdram, recomp_context* ctx);
int jfc_should_draw_target_reticle(void);
void jfc_adjust_projection(uint8_t* rdram, recomp_context* ctx);
void jfc_adjust_fog(uint8_t* rdram, recomp_context* ctx);
void jfc_finish_game_load(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_game_save(uint8_t* rdram, recomp_context* ctx);
void jfc_finish_game_save(uint8_t* rdram, recomp_context* ctx);
void jfc_end_game_save(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_default_game(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_game_clear(uint8_t* rdram, recomp_context* ctx);
void jfc_finish_game_clear(uint8_t* rdram, recomp_context* ctx);
void jfc_finish_all_game_clear(uint8_t* rdram, recomp_context* ctx);
void jfc_restore_persistent_tribals(uint8_t* rdram, recomp_context* ctx);
void jfc_restore_tribal_region(uint8_t* rdram);
void jfc_adapt_tribal_dialogue(uint8_t* rdram, uint32_t block, uint32_t size,
                             uint32_t message, uint32_t asset);
void jfc_record_tribal_rescue(uint8_t* rdram, recomp_context* ctx);
void jfc_protect_tribal_death(uint8_t* rdram, recomp_context* ctx);
int jfc_should_protect_tribal_object(uint8_t* rdram, uint32_t object);
void jfc_override_final_tribal_gate(uint8_t* rdram, recomp_context* ctx);
int jfc_prepare_quick_character_switch(uint8_t* rdram, recomp_context* ctx);
void jfc_commit_quick_character_switch(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_sprint_control(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_weapon_shortcuts(uint8_t* rdram, recomp_context* ctx);
void jfc_end_weapon_shortcuts(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_quick_grenade_input(uint8_t* rdram, recomp_context* ctx);
void jfc_end_quick_grenade_input(uint8_t* rdram, recomp_context* ctx);
int jfc_weapon_shortcut_busy(uint32_t player);
void jfc_draw_grenade_counter(uint8_t* rdram, recomp_context* ctx);
void jfc_draw_weapon_shortcuts(uint8_t* rdram, recomp_context* ctx);
uint16_t jfc_hud_weapon_mask(uint8_t* rdram);
float jfc_sprint_speed_operand(uint8_t* rdram, uint32_t player, float native_operand);
void jfc_scale_pistol_recharge(recomp_context* ctx);
int32_t jfc_boss_pistol_damage(uint8_t* rdram, uint32_t target, uint32_t source, uint32_t part, int32_t damage);
void jfc_boss_part_completed(uint8_t* rdram, uint32_t object, unsigned part);
void jfc_boss_mantis_weapon_completed(uint8_t* rdram, uint32_t object, unsigned group);
void jfc_boss_mizar_damage(uint8_t* rdram, uint32_t object, unsigned group, int32_t damage);
void jfc_refill_boss_phase_ammo(uint8_t* rdram);
int jfc_unlimited_lives_enabled(void);
void jfc_refill_after_mission(uint8_t* rdram);
int32_t jfc_robot_mission_allowance(int32_t tenths);
int jfc_hide_companion(uint8_t* rdram, uint32_t object);
int32_t jfc_boss_initial_health(int32_t health);
void jfc_initialize_boss_parts(uint8_t* rdram, uint32_t object, int slug_boss);
void jfc_initialize_mizar_health(uint8_t* rdram);
void jfc_begin_boss_target(uint8_t* rdram, uint32_t object);
void jfc_observe_boss_target(uint8_t* rdram, recomp_context* ctx);
void jfc_draw_boss_health(uint8_t* rdram, recomp_context* ctx);
uint32_t jfc_mantis_group_status(uint8_t* rdram, uint32_t object, uint32_t group, uint32_t status);
void jfc_draw_boss_weak_points(uint8_t* rdram, recomp_context* ctx, uint32_t object);
void jfc_route_robot_pitch(uint8_t* rdram, recomp_context* ctx);
void jfc_route_robot_target_pitch(uint8_t* rdram, recomp_context* ctx);
void jfc_draw_robot_binding(uint8_t* rdram, recomp_context* ctx);
void jfc_reset_health_recharge(void);
void jfc_observe_health(uint8_t* rdram);
void jfc_advance_health_recharge(uint8_t* rdram, int32_t ticks);
void jfc_scale_enemy_drop_roll(recomp_context* ctx);
void jfc_begin_companion_hud(uint8_t* rdram, recomp_context* ctx);
void jfc_validate_companion_controller(uint8_t* rdram, recomp_context* ctx);
int32_t jfc_throw_timer_step(int32_t weapon, int32_t before, int32_t after);
float jfc_throw_animation_rate(int32_t character, int32_t animation, float rate);
void jfc_scale_small_ammo_pickup(recomp_context* ctx);
void jfc_scale_large_ammo_pickup(recomp_context* ctx);
void jfc_scale_dropped_ammo_pickup(recomp_context* ctx);
void jfc_scale_duplicate_weapon_ammo(uint8_t* rdram, recomp_context* ctx);
void jfc_preserve_dropped_collectible(uint8_t* rdram, recomp_context* ctx);
void jfc_preserve_dropped_health(recomp_context* ctx);
void jfc_preserve_corpse_fade(uint8_t* rdram, recomp_context* ctx, recomp_func_t* remove_squaddie);
void jfc_corpse_opacity(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_corpse_retirement(uint8_t* rdram, recomp_context* ctx);
void jfc_end_corpse_retirement(void);
int jfc_corpse_reward_completed(uint32_t object);
int jfc_retain_completed_corpse(uint8_t* rdram, recomp_context* ctx, recomp_func_t* tidy_squad,
                              recomp_func_t* clear_targets, recomp_func_t* clear_events);
void jfc_preserve_detached_squad_resources(uint8_t* rdram, recomp_context* ctx);
void jfc_forget_corpse(uint32_t object);
void jfc_begin_animseq_group(uint8_t* rdram, recomp_context* ctx);
int jfc_prepare_seen_animseq_skip(uint8_t* rdram, recomp_context* ctx);
void jfc_classify_animseq_transition(uint8_t* rdram, recomp_context* ctx);
void jfc_finish_animseq_update(uint8_t* rdram, recomp_context* ctx);
void jfc_log_level_request(uint8_t* rdram, recomp_context* ctx);
void jfc_observe_completed_level_load(uint8_t* rdram, recomp_context* ctx);
void jfc_copy_game_binding(uint8_t* rdram, uint32_t source, uint32_t destination);
void jfc_autosave_completed_level_load(uint8_t* rdram, recomp_context* ctx, recomp_func_t* request_save);
void jfc_track_ship_part_collectable(uint8_t* rdram, recomp_context* ctx);
int jfc_update_ship_part_scanner(uint8_t* rdram, recomp_context* ctx);
void jfc_initialize_boot_environment(uint8_t* rdram, recomp_context* ctx);
void jfc_boot_ram_test_6105(uint8_t* rdram, recomp_context* ctx);
int jfc_shotgun_upgrade_enabled(void);
void jfc_shotgun_press(uint8_t* rdram, recomp_context* ctx);
void jfc_shotgun_edge(uint8_t* rdram, recomp_context* ctx);
void jfc_shotgun_charge(uint8_t* rdram, recomp_context* ctx);
void jfc_shotgun_spread(uint8_t* rdram, recomp_context* ctx, recomp_func_t* create);
int jfc_flamethrower_upgrade_enabled(void);
void jfc_flame_debit(uint8_t* rdram, recomp_context* ctx);
void jfc_forget_weapon_object(uint32_t object);
void jfc_accepted_ignition(uint32_t object);
void jfc_flame_contact(uint8_t* rdram, recomp_context* ctx, recomp_func_t* ignite, recomp_func_t* hurt);
void jfc_flame_damage(uint8_t* rdram, recomp_context* ctx);
uint32_t jfc_weapon_capacity(uint32_t native_capacity, uint32_t weapon);
uint32_t jfc_weapon_pickup_capacity(uint32_t native_capacity, uint32_t weapon, uint32_t held);
void jfc_shocker_range(uint8_t* rdram, recomp_context* ctx);
void jfc_shocker_created(uint8_t* rdram, recomp_context* ctx);
void jfc_weapon_world_reset(void);
void jfc_shocker_free(uint8_t* rdram, recomp_context* ctx, recomp_func_t* release);
void jfc_shocker_pose(uint8_t* rdram, recomp_context* ctx, recomp_func_t* rotate);
void jfc_shocker_hits_begin(uint8_t* rdram, recomp_context* ctx, recomp_func_t* visibility, recomp_func_t* rotate);
void jfc_shocker_hits_order(uint8_t* rdram, recomp_context* ctx);
int jfc_shocker_query(uint8_t* rdram, recomp_context* ctx, recomp_func_t* query);
void jfc_shocker_hit(uint8_t* rdram, recomp_context* ctx);
void jfc_shocker_hits_end(uint8_t* rdram, recomp_context* ctx, recomp_func_t* visibility, recomp_func_t* create);

void osDpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx);
void __f_to_ll_recomp(uint8_t* rdram, recomp_context* ctx);
void __ll_to_f_recomp(uint8_t* rdram, recomp_context* ctx);
void __osPiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx);
void __osPiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx);
void jfc_begin_guidance_draw(uint8_t* rdram, recomp_context* ctx);
void jfc_end_guidance_draw(uint8_t* rdram, recomp_context* ctx);
void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx);
void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx);
void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx);
void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx);
void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx);
void __osGetId_recomp(uint8_t* rdram, recomp_context* ctx);
void __osPfsSelectBank_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
