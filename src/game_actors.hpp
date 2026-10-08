#pragma once

#include "scene_math.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <memory>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace pusu {
class AssetStore;
class CollisionWorld;
struct GameInput;
struct RenderCamera;
using EntityHandle = std::uint32_t;
enum class WeaponKind : std::uint8_t { none, cz75, magnum, uzi, ak101, m4, baba };
enum class ActorKind { player, bot };
struct WeaponState {
    WeaponKind kind{};
    int magazine{};
    float cooldown{}, reload_remaining{};
    bool owned{};
};
struct PlayerMouseState {
    std::array<Vec2, 10> samples{};
    unsigned cursor{}, idle_samples{10};
};
struct PlayerCameraState {
    std::uint8_t special{};
    bool reverse{}, near{};
    float desired_distance{100}, special_rotation{};
    float pitch_anchor{}, pitch{}, distance_anchor{100}, distance{100};
    Vec3 ray_origin{};
    float half_width{10}, half_height{8}, recovery_acceleration{2500};
    double recovery_time{}, death_roll_time{};
    bool recovering{}, death_captured{};
    bool scope_visible{}, scope_entry_completed{};
    float scope_fov{90}, scope_rotation_anchor{};
    bool pitch_moved{}, hit_effect_active{};
    double hit_effect_time{};
    float hit_effect_duration{};
    Matrix death_orientation{identity_matrix()}, world{identity_matrix()};
    float fov{90};
};
struct ActorWaypoint {
    Vec3 position{}, direction{};
    float dwell_seconds{};
    bool crouch{}, run{};
};
struct AiState {
    float reaction_min{.2f}, reaction_max{1}, radius{7000};
    float movement_min{2}, movement_max{4}, run_probability{.5f};
    float last_attack_time{5}, last_attack_probability{.7f}, last_move_probability{.8f};
    float dexterity{1}, attack_ability{1};
    std::array<float, 3> attack_weights{.2f, .6f, .2f};
    bool registered{}, enabled{}, movement_pending{}, turned{};
    bool sound_played{}, autostart{}, last_seen_valid{}, movement_decision{}, acquisition{};
    std::string see_player_sound;
    Vec3 initial_position{}, initial_forward{}, last_seen_position{}, target{};
    Matrix saved_orientation{}, working_orientation{};
    Vec3 obstacle_contact{};
    bool obstacle_following{}, obstacle_side{}, obstacle_side_valid{};
    bool obstacle_previous_side{};
    int combat_state{}, navigation_state{}, route_cursor{}, move_mode{};
    double last_seen_time{}, move_segment_time{}, move_decision_time{}, reaction_time{};
    double dwell_time{}, search_time{};
    double movement_auxiliary_time{};
    float move_duration{}, reaction_delay{};
    bool run{};
    std::vector<ActorWaypoint> waypoints;
    std::vector<int> route;
    std::deque<Vec3> path;
};
struct ActorState : CharacterMotionState {
    EntityHandle entity{};
    ActorKind kind{};
    Vec3 angles{}, velocity{};
    Matrix orientation{};
    Vec3 previous_position{}; // Original actor +220, separate from current +22c.
    std::uint32_t position_time_tick{}; // Original actor +21c.
    float health{};
    float jump_velocity{352}; // 00417ed0, original float at 00475ce4.
    std::array<WeaponState, 7> weapons{};
    std::array<WeaponKind, 2> weapon_slots{};
    std::array<int, 5> ammunition{};
    std::vector<std::uint8_t> ammunition_order;
    std::uint8_t selected_slot{};
    std::vector<std::string> inventory;
    WeaponKind weapon{};
    AiState ai;
    int combat_state{}, previous_combat_state{}, shoot_flag{1};
    double combat_time{}, shot_time{}, movement_action_time{};
    double shot_visual_time{};
    std::uint32_t footstep_variant{};
    bool footstep_first{}, footstep_second{}, death_surface_sound{};
    int weapon_phase{};
    bool shot_light{};
    bool no_ammo_latch{}, reload_blocked{}, movement_reload_blocked{};
    int drop_ammunition{};
    bool drop_health{};
    float hit_pitch{}, hit_yaw{};
    Vec3 hit_direction{};
    double hit_time{};
    int hit_region{10}; // Original constructor's inactive HUD hit category.
    WeaponKind hit_weapon{};
    bool blood_enabled{true};
    double shake_time{};
    float shake_duration{}, shake_yaw{}, shake_pitch{};
    PlayerCameraState player_camera;
    bool shake_active{};
    std::string name, animation, ai_name, on_death;
    bool enabled{true}, alive{true}, grounded{}, crouched{}, attacking{}, moving{};
    bool no_weapon{};
    bool retired{};
    bool trigger_held{};
};
// Borrowed live controller state, valid until the next synchronous animation event.
struct ActorAnimationController {
    std::string_view clip;
    std::uint32_t mode{}, status{3}, start_tick{}, duration_ms{}, sample_tick{};
};
struct ActorEvent {
    enum class Kind { script, animation, shot, use, drop_weapon, sound, weapon_visibility, particle, drop_health, player_death, damage_visual, weapon_phase, weapon_light, player_effect };
    enum class AnimationOperation { pair, direct, reset_aim_unless_master, reset_aim, reset_all, pause_controller };
    Kind kind{};
    EntityHandle entity{}, other{};
    std::string name;
    std::string_view animation_clip; // Synchronous animation-only borrowed asset/script key.
    std::string category, shader, mesh;
    Vec3 position{}, direction{};
    Vec3 normal{}, basis_right{}, basis_up{};
    int material{-1}, part{-1}, hit_region{};
    int phase{};
    float value{}, radius{};
    WeaponKind weapon{};
    bool environment_sound{};
    std::array<int, 5> pickup{};
    int animation_channel{}; // Original pair first0/2, direct aim4/5, pause one actual slot.
    std::uint32_t animation_mode{};
    float animation_blend{1}; // Direct aim coefficient; effective weight comes from live master.
    bool restart_animation{};
    AnimationOperation animation_operation{AnimationOperation::pair};
    std::uint32_t animation_previous_mode{2}; // 004128f0 reverse-match mode.
    bool animation_reverse_match{}, animation_immediate{}; // Same-clip reversal / nonNULL param8.
    int animation_master_channel{-1};
    std::string_view animation_master_clip; // Synchronous borrowed expected PA identity.
};
struct ActorHit {
    bool hit{};
    EntityHandle entity{};
    EntityHandle owner{};
    int actor_type{-1};
    Vec3 position{}, normal{};
    float distance{}, plane_distance{};
    int part{-1}, material{-1}, region{};
    std::string_view mesh, shader;
};
struct CombatHooks {
    // Missing named attachments are errors; Game must return the actual posed
    // attachment matrix, not an actor-origin substitute.
    std::function<Matrix(EntityHandle, std::string_view)> attachment;
    std::function<ActorHit(Vec3, Vec3, EntityHandle, std::uint32_t, std::uint32_t, bool)> trace;
    std::function<bool(EntityHandle, std::string_view)> has_attachment;
    std::function<bool(EntityHandle, std::string_view)> animation_exists; // 00411a40.
    std::function<ActorAnimationController(EntityHandle, int)> animation_controller; // Actual slot0..5.
    // 00414240 / 00414fa0 material sounds; bots pass no positional origin.
    std::function<void(EntityHandle, bool, bool, Vec3, const ActorHit&, std::uint32_t*)> animation_surface_sound;
    std::function<void(EntityHandle, bool)> frame_changed; // Mode0 dirty, mode1 also relinks.
    std::function<void(EntityHandle)> corpse; // 004164d0 root flags/bounds/part culling/BSP.
    std::function<ActorHit(Vec3, Vec3, float, Bounds, EntityHandle, std::uint32_t, std::uint32_t, bool, EntityHandle, bool)> trace_motion;
    std::function<Matrix()> camera;
    std::function<float()> camera_fov;
    std::function<void(float)> set_camera_fov;
    std::function<float()> camera_view_distance;
    std::function<unsigned()> viewport_width;
    std::function<bool()> bot_damage_blocked;
    std::function<bool()> camera_placement_bypass;
    std::function<bool()> camera_update_blocked;
    std::function<void(const ActorHit&, float, EntityHandle)> damage;
    std::function<void(const ActorHit&, Vec3, int)> impact;
    std::function<void(EntityHandle, bool, Vec3, const Trace&)> land_sound;
    std::function<void(Vec3, Vec3, Bounds)> triggers;
    std::function<void(EntityHandle, Vec3, const ActorHit&, std::string_view)> material_action;
};

// Game owns entities and VM execution. Events describe concrete actor changes;
// the sink resolves entity meshes, audio/effects, and named script events.
class ActorRuntime {
public:
    using EventSink = std::function<void(const ActorEvent&)>;
    ActorRuntime(AssetStore& assets, EventSink events, CombatHooks combat);
    ~ActorRuntime();
    void clear();
    ActorState& create_player(EntityHandle entity, std::string name, const Object& definition);
    ActorState& create_bot(EntityHandle entity, std::string name, const Object& definition);
    ActorState* find(EntityHandle entity) noexcept;
    const ActorState* find(EntityHandle entity) const noexcept;
    ActorState* player() noexcept { return find(player_entity_); }
    const ActorState* player() const noexcept { return find(player_entity_); }
    std::deque<ActorState>& actors() noexcept { return actors_; }
    const std::deque<ActorState>& actors() const noexcept { return actors_; }
    void update(float seconds, const GameInput& input, CollisionWorld* collision);
    void set_cinematic(bool active) noexcept { cinematic_ = active; }
    // Game owns the original integral clock, separate from fixed1/30 physics.
    void synchronize_clock_tick(std::uint32_t absolute_tick) noexcept { time_ = absolute_tick; }
    std::uint32_t random_word() noexcept;
    std::uint32_t random_state() const noexcept { return random_state_; }
    void set_random_state(std::uint32_t state) noexcept { random_state_ = state; }
    void set_mouse_settings(float original_slider_setting, bool invert) noexcept;
    void set_player_input_mode(bool processing, bool use_only, bool menu) noexcept;
    void script_animation(EntityHandle actor);
    void capture_home_position(EntityHandle actor);
    void capture_home_forward(EntityHandle actor);
    RenderCamera camera(CollisionWorld* collision);
    Matrix frame(EntityHandle actor, int selector) const;
    void set_position(EntityHandle actor, Vec3 position);
    void prepare_render(std::uint32_t render_tick, std::uint32_t interval);
    void grant_weapon(EntityHandle actor, WeaponKind weapon, int ammunition);
    void grant_weapon(EntityHandle actor, std::string_view weapon, int ammunition);
    bool take_weapon(EntityHandle actor, std::array<int, 5>& pickup);
    bool take_ammunition(EntityHandle actor, int type, int count);
    void add_inventory(EntityHandle actor, std::string_view item);
    void set_health(EntityHandle actor, float health);
    void drop_weapon(EntityHandle actor);
    void reload(EntityHandle actor, bool fallback = false);
    void hurt(EntityHandle actor, float damage, EntityHandle source = 0);
    void add_waypoint(EntityHandle actor, ActorWaypoint waypoint);
    void clear_waypoints(EntityHandle actor);
    void rewind_waypoints(EntityHandle actor);
    void waypoint_index(EntityHandle actor, std::uint32_t zero_based_index);
    void waypoint_clear_indices(EntityHandle actor);
    void waypoint_start(EntityHandle actor);
    void waypoint_end(EntityHandle actor);
    void no_weapon(EntityHandle actor, bool enabled);
    void camera_shake(EntityHandle actor, float duration, float yaw, float pitch);
    void init_orientation(EntityHandle actor);
    bool inventory_contains(EntityHandle actor, std::string_view item) const;
    void remove_inventory(EntityHandle actor, std::string_view item);
    void remove(EntityHandle entity);
    void save(std::ostream& output) const;
    void load(std::istream& input);
    void save_player_stats(std::ostream& output, EntityHandle actor) const;
    void load_player_stats(std::istream& input, EntityHandle actor);
    void swap_state(ActorRuntime& other) noexcept;
    static WeaponKind weapon_kind(std::string_view name);
private:
    AssetStore& assets_;
    EventSink events_;
    CombatHooks combat_;
    std::deque<ActorState> actors_;
    std::unique_ptr<GameInput> previous_input_;
    double time_{};
    std::uint32_t random_state_{1};
    int ai_path_budget_{500};
    std::size_t ai_cursor_{};
    EntityHandle player_entity_{};
    // Borrowed query context, valid only during the current synchronous motion call.
    ActorState* motion_actor_{};
    std::size_t ai_iteration_{};
    PlayerMouseState mouse_state_;
    float mouse_base_weight_{.25f}, mouse_active_weight_{.25f};
    bool cinematic_{};
    bool invert_mouse_{};
    bool player_processing_{true}, use_only_{}, menu_visible_{};
    bool jump_latched_{};
    ActorHit ai_path_query_hit_, ai_path_block_hit_;
    void emit(ActorEvent event);
    CharacterMotionHooks motion_hooks(ActorState&);
    void remove_ai(EntityHandle);
    void jump(ActorState&, CollisionWorld*);
    void land(ActorState&, Vec3, const Trace&);
    void player_effect(ActorState&, float);
    void update_player(ActorState&, float seconds, const GameInput&, CollisionWorld*);
    void update_bot(ActorState&, float seconds, CollisionWorld*);
    void update_ai(CollisionWorld*);
    void ai_notify_shot(ActorState&);
    void read_ai(ActorState&);
    void ai_queue_target(ActorState&, Vec3);
    bool ai_follow_path(ActorState&, CollisionWorld*);
    bool ai_search_path(ActorState&, CollisionWorld*);
    void ai_begin_path(ActorState&, Vec3);
    bool ai_path_blocked(ActorState&, Vec3, Vec3, ActorHit&);
    bool ai_path_direct(ActorState&);
    void ai_path_ground(ActorState&, Vec3&);
    bool ai_path_step(ActorState&, Vec3&);
    void ai_path_obstacle_step(ActorState&);
    void ai_path_move(ActorState&, CollisionWorld*);
    int ai_visibility(const ActorState&, const ActorState&);
    bool ai_ready(ActorState&);
    bool ai_friendly_fire(const ActorState&);
    void ai_choose_movement(ActorState&, const ActorState&);
    bool ai_turn(ActorState&, Vec3, float, bool muzzle = false);
    void ai_attack(ActorState&, ActorState&, int visibility, CollisionWorld*);
    void set_posture(ActorState&, int);
    void command_move(ActorState&, int mode);
    void move(ActorState&, float seconds, CollisionWorld*);
    bool can_fire(const ActorState&, float factor) const;
    bool fire(ActorState&, CollisionWorld*);
    void reload(ActorState&, bool fallback = false);
    void update_combat(ActorState&);
    void dispatch_animation(ActorState&, std::uint32_t render_tick);
    void torso_animation(ActorState&, std::uint32_t render_tick,
                         std::string_view motion, std::string_view aim_variant,
                         std::string_view preferred_group, std::string_view fallback_group,
                         bool immediate);
    void animation_pair(ActorState&, int, std::string_view group, std::string_view clip,
                        std::uint32_t mode, bool immediate = false,
                        std::uint32_t previous_mode = 2, bool reverse_match = false);
    void animation_control(ActorState&, ActorEvent::AnimationOperation, int channel = 0);
    void animation_footstep(ActorState&, std::string_view clip);
    void select(ActorState&, WeaponKind);
    bool camera_obstructed(const ActorState&, const Matrix&) const;
    Matrix place_camera(ActorState&, const Matrix&);
};
} // namespace pusu
