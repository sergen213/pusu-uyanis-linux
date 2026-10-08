#pragma once

#include "assets.hpp"
#include "game_actors.hpp"
#include "game_effects.hpp"
#include "game_materials.hpp"
#include "interface.hpp"
#include "media.hpp"
#include "renderer.hpp"
#include "scene_math.hpp"
#include "script_vm.hpp"
#include "resources.hpp"
#include "settings.hpp"
#include <deque>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pusu {
class AssetStore;
class Interface;
class MaterialLibrary;
class Media;
struct MenuAction;
struct HudState;

// Action values are resolved through the original configurable input bindings.
// Mouse deltas are relative device motion, not authored-menu coordinates.
struct GameInput {
    float mouse_x{},mouse_y{};
    bool forward_down{},backward_down{},left_down{},right_down{};
    bool fire{}, use{}, jump{}, crouch{}, walk{}, reload{};
    bool change_weapon{},drop_weapon{},aim{},pause{};
    bool pistol{}, rifle{};
};

class Game {
public:
    Game(AssetStore& assets, const MaterialLibrary& materials, Settings& settings,
         Interface& interface, Media& media);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    void boot();
    void new_game();
    void update(float seconds, const GameInput& input);
    void resize(unsigned width,unsigned height) noexcept { viewport_width_=width;viewport_height_=height; }
    using LoadingPresenter = void (*)(void*,std::string_view,float);
    using LevelPreparer = void (*)(void*,const Level&,std::uint64_t,std::string_view);
    using ScenePreparer = void (*)(void*,const RenderScene&,std::string_view);
    void set_loading_presenter(void* context,LoadingPresenter presenter,
                               LevelPreparer preparer=nullptr,ScenePreparer scene_preparer=nullptr) noexcept {
        loading_context_=context;loading_presenter_=presenter;level_preparer_=preparer;
        scene_preparer_=scene_preparer;
    }
    void action(const MenuAction& action);
    void save(const std::filesystem::path& destination) const;
    void load(const std::filesystem::path& source);
    // Returned spans borrow game-owned cached data until the next update/load.
    RenderScene frame() const noexcept;
    const RenderCamera& view() const noexcept { return camera_; }
    const script::Runtime& runtime() const noexcept { return runtime_; }
    bool wants_quit() const noexcept { return quit_; }
    bool menu_visible() const noexcept { return menu_; }
    bool scene_changed() noexcept;
    float camera_fov() const noexcept { return camera_fov_; }

private:
    struct Entity {
        struct Part {
            std::string mesh_name, shader;
            const Mesh* mesh{};
            std::vector<Matrix> pose;
            std::optional<PoseEvaluator> evaluator;
            std::vector<BoneFrame> animation_locals;
            std::vector<std::uint8_t> animation_touched;
            std::array<std::optional<PoseEvaluator>,6> layer_evaluators;
            std::array<const Animation*,6> layer_animations{};
            std::uint32_t trace_mask{0x108600};
            int material{-1};
            bool visible{true};
            std::uint64_t lighting_id{};
            Bounds collision_bounds{};
            std::optional<Material> runtime_material;
        };
        struct Model {
            std::string name;
            const Object* definition{};
            std::vector<Part> parts;
            std::vector<Model> linked;
            std::map<std::string,const std::pair<const std::string,Animation>*,std::less<>> character_clips;
            std::optional<Attachment> attachment;
            std::size_t attachment_part{};
            Matrix world{identity_matrix()};
            bool visible{true};
            bool renderable{true};
            bool frustum_cull{true};
            int region{};
            Bounds bounds{};
            bool has_bounds{};
        };
        std::uint32_t handle{}, position_address{};
        std::string name, object_name;
        int kind{};
        Vec3 position{};
        Matrix orientation{identity_matrix()};
        Model model;
        float camera_fov{90}, near_plane{4}, far_plane{20000};
        bool camera_fov_explicit{};
        bool alive{true}, visible{true};
        int pickup_kind{};
        std::array<int, 5> pickup_weapon{};
        int pickup_ammunition_type{}, pickup_ammunition_count{};
        float pickup_health{};
        float health{std::numeric_limits<float>::max()}, explosion_damage{}, explosion_radius{};
        bool destruction_started{};
        std::vector<std::uint32_t> membership_leaves;
        WorldProjection projection_geometry;
    };
    struct TimedEvent { std::string name; double deadline{}; std::uint64_t serial{}; };
    struct CharacterPlayback : AnimationControllerState {
        std::uint32_t entity{};
        std::string clip,master_clip;
        int channel{}; // Actual original slots: legs0/1, torso2/3, aim4/5.
    };
    struct KeyframePlayback {
        std::string name, linked, event;
        const Keyframes* clip{};
        std::vector<std::uint32_t> targets;
        std::vector<std::size_t> target_tracks;
        std::vector<Matrix> target_base;
        std::uint32_t previous_camera{}, camera_target{};
        std::vector<Matrix> transforms;
        float duration{};
        std::uint32_t start_tick{},frozen_phase{};
        int mode{}, repeats{1}, completed{};
        bool active{}, paused{};
        bool pause_dirty{}, camera_selected{};
    };
    struct KeyframeStep {
        bool ended{};
        std::uint32_t elapsed_ticks{},sample_ticks{};
    };
    static KeyframeStep advance_keyframe(KeyframePlayback& playback,std::uint32_t now,
                                        std::uint32_t duration_ticks) noexcept;
    friend void game_keyframe_check();
    struct SoundSource {
        std::string name, path;
        Vec3 position{};
        std::vector<std::uint64_t> channels;
        std::uint8_t volume{255};
        bool loop{true}, spatial{true}, stream{};
        float min_distance{400};
    };
    const MaterialLibrary& materials_;
    struct ScriptVariable { std::uint32_t value{}; int type{}; std::string text; bool owns_string{}; };
    struct QueuedExplosion {
        std::uint32_t entity{};
        Vec3 position{};
        float radius{}, damage{};
    };
    struct MenuSaveSlot { std::filesystem::path path; std::string name,label,shader; };
    AssetStore& assets_;
    Settings& settings_;
    Interface& interface_;
    Media& media_;
    script::Runtime runtime_;
    std::ofstream warning_log_;
    ActorRuntime actors_;
    ParticleRuntime effects_;
    PhysicalMaterials physical_materials_;
    std::vector<ActorEvent> actor_events_;
    std::unique_ptr<Level> level_;
    std::unique_ptr<CollisionWorld> collision_;
    std::map<std::string, std::optional<Mesh>, std::less<>> meshes_;
    std::map<std::string, Object, std::less<>> objects_;
    std::map<std::string, Animation, std::less<>> animations_;
    std::map<std::string, Keyframes, std::less<>> keyframes_;
    std::deque<Entity> entities_;
    std::deque<QueuedExplosion> explosions_;
    std::vector<std::uint32_t> explosion_candidates_;
    std::vector<MenuSaveSlot> menu_save_slots_;
    std::vector<int> menu_chapters_;
    std::vector<std::vector<std::uint32_t>> leaf_entities_;
    std::vector<std::uint32_t> membership_seen_;
    std::uint32_t membership_stamp_{},fallback_leaf_{};
    std::vector<std::int32_t> membership_stack_;
    std::size_t membership_capacity_{};
    std::vector<TimedEvent> events_;
    std::vector<CharacterPlayback> character_playbacks_;
    std::vector<KeyframePlayback> keyframe_playbacks_;
    std::vector<SoundSource> sounds_;
    std::map<std::string, ScriptVariable, std::less<>> variables_;
    std::vector<RenderObject> draws_;
    std::vector<RenderParticle> particles_;
    RenderCamera camera_;
    std::optional<MediaSnapshot> saved_media_;
    std::optional<InterfaceSnapshot> saved_interface_;
    std::string level_name_,bsp_path_;
    double time_{}, clock_{};
    std::uint32_t player_{};
    float camera_fov_{};
    bool menu_{true}, quit_{}, scene_changed_{};
    bool processing_{true}, input_processing_{true}, rendering_{true};
    bool use_only_{}, cinematic_{}, hud_visible_{true};
    std::string pending_level_, sound_set_, music_, fullscreen_shader_, subtitle_shader_;
    std::map<std::string, std::string, std::less<>> language_;
    std::array<std::string, 4> subtitles_;
    struct TriggerState {
        std::vector<std::uint8_t> inside;
        std::vector<std::uint32_t> last_ticks,active,leaf_triggers;
        std::vector<std::size_t> leaf_offsets;
        std::vector<std::uint64_t> encounter_stamps;
        std::uint64_t generation{};
    };
    TriggerState triggers_;
    std::uint32_t next_entity_{1};
    Vec2 pending_mouse_{};
    std::uint64_t scene_generation_{};
    std::uint64_t next_event_serial_{},next_lighting_id_{},effects_resource_revision_{};
    std::uint64_t materials_resource_revision_{};
    unsigned viewport_width_{},viewport_height_{};
    std::uint64_t frame_stamp_{};
    std::uint32_t game_tick_{},next_tick_{},main_camera_{};
    std::uint32_t selected_camera_{};
    float fade_seconds_{};
    int environment_{1}, fog_type_{0x2601};
    std::array<float, 4> fog_color_{};
    std::uint32_t fog_start_{}, fog_end_{};
    float fog_density_{};
    bool pending_image_{}, pending_checkpoint_{}, no_weapon_{}, fog_enabled_{};
    bool fullscreen_{}, subtitle_{}, fade_in_{};
    bool prerender_pending_{};
    bool scheduler_ready_{};
    bool player_effect_{};
    std::uint64_t player_effect_serial_{};
    bool camera_blocked_{};
    std::uint32_t camera_transition_tick_{};
    void* loading_context_{};
    LoadingPresenter loading_presenter_{};
    LevelPreparer level_preparer_{};
    ScenePreparer scene_preparer_{};
    std::string loading_shader_;
    void load_language();
    void dispatch_actor_events();
    void release_position_bindings();
    enum class SnapshotPurpose { exact, checkpoint };
    void save(const std::filesystem::path&, SnapshotPurpose) const;
    void load(const std::filesystem::path&, SnapshotPurpose);
    void load_checkpoint(const std::filesystem::path&);
    void write_save(std::ostream&, SnapshotPurpose) const;
    void read_save(std::istream&);
    void swap_saved_state(Game&) noexcept;
    friend void game_save_check(Game&);
    Matrix attachment_world(std::uint32_t handle, std::string_view attachment);
    bool has_attachment(std::uint32_t handle, std::string_view attachment) const;
    double animation_duration(std::uint32_t handle, std::string_view clip);
    const std::pair<const std::string,Animation>* character_animation(Entity::Model&,std::string_view);
    ActorHit trace_scene(Vec3 start, Vec3 end, std::uint32_t source,
                         std::uint32_t shader_mask, std::uint32_t scene_mask, bool pickups);
    ActorHit trace_scene(Vec3 start,Vec3 end,std::uint32_t source);
    ActorHit trace_motion(Vec3 start,Vec3 direction,float distance,Bounds hull,
                          std::uint32_t source,std::uint32_t shader_mask,std::uint32_t flags,
                          bool pickups,std::uint32_t pickup_actor,bool box_query);
    ActorHit trace_query(Vec3 start,Vec3 end,Vec3 direction,float distance,Bounds hull,
                         std::uint32_t source,std::uint32_t shader_mask,std::uint32_t flags,
                         bool pickups,std::uint32_t pickup_actor,bool box_query=false);
    void trace_triggers(Vec3 start,Vec3 end,Bounds hull);
    void use_triggers(Vec3 position,std::string_view suffix);
    void use_entities(Vec3 position);
    static void reset_trigger_state(TriggerState&,const Level&);
    static void trace_trigger_state(TriggerState&,CollisionWorld&,Vec3,Vec3,Bounds,
                                    std::uint64_t,std::uint32_t,script::Runtime&);
    static void repeat_trigger_state(TriggerState&,const Level&,std::uint32_t,script::Runtime&);
    static void use_trigger_state(TriggerState&,CollisionWorld&,Vec3,std::uint64_t,
                                  script::Runtime&,std::string_view);
    friend void game_trigger_check();
    void host_character_layer(std::uint32_t handle,std::string_view clip,bool loop,
                              int channel,float blend,bool restart,std::uint32_t animation_mode=1);
    CharacterPlayback* character_controller(std::uint32_t handle,int channel) noexcept;
    const CharacterPlayback* character_controller(std::uint32_t handle,int channel) const noexcept;
    ActorAnimationController animation_controller(std::uint32_t handle,int channel) const noexcept;
    void character_animation_event(const ActorEvent&);
    void host_character_pair(const ActorEvent&);
    void advance_character_controllers();
    void bind_character_layer(Entity::Model&,const Animation&,int channel);
    void rebind_character_model(Entity&);
    void evaluate_character_model(Entity::Model&,std::uint32_t handle);
    void update_camera_scene();
    void host_weapon_phase(std::uint32_t,int);
    void configure_actor_material(Entity&);
    void host_weapon_light(const ActorEvent&);
    void update_actor_projection(Entity&);
    void on_actor_event(const ActorEvent&);
    void apply_actor_event(const ActorEvent&);
    void resolve_part_material(Entity::Part&,std::uint32_t constructor_contents);
    void prepare_membership();
    void clear_membership();
    void refresh_membership(Entity&);
    friend void game_spatial_check();
    void configure_prop(Entity&);
    void damage_entity(std::uint32_t,float,std::uint32_t,const ActorHit&);
    void queue_prop_explosion(Entity&);
    void destroy_prop(std::uint32_t);
    void process_explosion(const QueuedExplosion&);
    void collect_explosion_entities(Vec3,float,std::vector<std::uint32_t>&);
    void refresh_load_menu();
    void select_load_chapter();
    void select_load_checkpoint();

    void register_hosts();
    void register_hosts(script::Runtime& target);
    void warn_missing_call(std::string_view name) noexcept;
    void warn(std::string_view prefix,std::string_view name) noexcept;
    void load_level(std::string_view name, bool loading_image);
    void load_bsp(std::string_view name);
    void create_entities(std::string_view name);
    void create_sound_sources(std::string_view name);
    Entity& entity(std::uint32_t handle);
    const Entity& entity(std::uint32_t handle) const;
    std::uint32_t create_entity(std::string name, int kind);
    void set_mesh(Entity& entity, std::string_view name);
    void build_model(Entity::Model& model, std::string_view name);
    void configure_actor_model(Entity& entity);
    void configure_corpse(Entity&);
    void rebuild_frame();
    void update_player(float seconds, const GameInput& input);
    void update_events();
    void update_animations(float seconds);
    void update_particles(float seconds);
    void update_triggers();
    void update_hud(float seconds);
    void queue_level(std::string name, bool image, bool checkpoint = false);
    void save_checkpoint(std::string_view name);
    void save_player_stats();
    void load_player_stats();
    std::uint32_t host_create_entity(std::string name, int kind, std::string definition = {});
    void host_set_mesh(std::uint32_t handle, std::string_view mesh);
    std::uint32_t host_find_bot(std::string_view name) const;
    std::uint32_t host_player() const;
    std::uint32_t host_position_address(std::uint32_t handle);
    Vec3 host_position(std::uint32_t handle);
    void host_set_position(std::uint32_t handle, Vec3 position);
    void host_rotate(std::uint32_t handle, int axis, float degrees, bool world);
    void host_align(std::uint32_t handle, int axis, std::uint32_t coordinate_bits);
    void host_delete_entity(std::uint32_t handle);
    void host_entity_visible(std::string_view name, bool visible);
    void host_linked_visible(std::string_view name, std::string_view linked, bool visible);
    void host_shader(std::string_view name, std::string_view old_shader,
                     std::string_view new_shader, bool all);
    void host_bot_weapon(std::uint32_t actor, std::uint32_t value, int weapon);
    void host_bot_enabled(std::string_view name, bool enabled);
    void host_no_weapon(bool enabled);
    void host_hurt(float damage);
    void host_init_orientation();
    void host_waypoint(std::string_view name, int operation, int index = 0);
    void host_camera_fov(std::string_view name, float degrees);
    static Matrix keyframe_forward_binding_base(const Matrix& raw,std::string_view target_name);
    void host_keyframe(std::string_view name, int mode, int count, std::string_view linked);
    void host_pause_keyframe(std::string_view name, float seconds, std::string_view linked = {});
    void host_resume_keyframe(std::string_view name, std::string_view linked = {});
    void host_character(std::string_view name, std::string_view animation, bool loop);
    void host_sound(std::string_view file, bool loop, bool stream, int category);
    void host_named_sound(std::string_view name, int category);
    void host_stop_sound(std::string_view name);
    void host_sound_volume(std::string_view name, std::uint32_t volume);
    void host_music(std::string_view file);
    void host_stop_music();
    void host_environment(int type);
    bool host_inventory_contains(std::string_view item) const;
    void host_inventory_remove(std::string_view item);
    void host_particle(std::string_view name, std::string_view definition,
                       Vec3 position, Vec3 velocity, bool active);
    void host_particle_active(std::string_view name, bool active);
    void host_timed_event(std::string name, float delay);
    void host_explosion();
    void host_shake(float x, float y, float z);
    bool host_svar_exists(std::string_view name) const;
    std::uint32_t host_svar_value(std::string_view name) const;
    void host_svar_word(std::string name, std::uint32_t value, int type);
    void host_svar_string(std::string name, std::string value);
    void host_fog_enabled(bool value);
    void host_fog_type(int gl_type);
    void host_fog_color(std::array<float, 4> color);
    void host_fog_range(std::uint32_t start, std::uint32_t end);
    void host_fog_density(float value);
    void host_rendering(bool enabled);
    void host_cinematic(bool enabled);
    void host_fade(bool fade_in);
    void host_fade_time(float seconds);
    void host_fullscreen(std::string_view shader, bool enabled);
    void host_subtitle(std::string_view shader, bool enabled);
    void host_subtitle_text(int line, std::string_view string_id);
    std::uint32_t host_random();
};
#ifdef PUSU_GAME_SAVE_CHECK
void game_save_check(Game&);
#endif
void game_audio_check();
void prop_damage_check();
void game_spatial_check();
void game_trigger_check();
} // namespace pusu
