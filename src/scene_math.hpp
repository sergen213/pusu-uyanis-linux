#pragma once

#include "assets.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace pusu {
class MaterialLibrary;
class AssetStore;
struct Material;
// Original 00436620 -> 004430b0 logical actor frame0, not a GL camera conversion.
inline constexpr Matrix original_actor_basis{0,-1,0,0, 0,0,1,0, -1,0,0,0, 0,0,0,1};


inline Matrix identity_matrix() noexcept {
    Matrix out{};
    out[0]=out[5]=out[10]=out[15]=1;
    return out;
}
inline Matrix multiply(const Matrix& a,const Matrix& b) noexcept {
    Matrix out{};
    for(unsigned col=0;col!=4;++col)
        for(unsigned row=0;row!=4;++row)
            for(unsigned k=0;k!=4;++k)out[col*4+row]+=a[k*4+row]*b[col*4+k];
    return out;
}
inline Matrix inverse_rigid(const Matrix& m) noexcept {
    Matrix out{};
    for(unsigned col=0;col!=3;++col)
        for(unsigned row=0;row!=3;++row)out[col*4+row]=m[row*4+col];
    for(unsigned row=0;row!=3;++row)
        out[12+row]=-(out[row]*m[12]+out[4+row]*m[13]+out[8+row]*m[14]);
    out[15]=1;
    return out;
}
Matrix inverse(const Matrix& matrix);
Matrix inverse_unchecked(const Matrix& matrix) noexcept;
Vec3 transform_point(const Matrix& matrix, Vec3 point) noexcept;
Vec3 transform_vector(const Matrix& matrix, Vec3 vector) noexcept;
Quaternion normalized(Quaternion rotation);
Quaternion interpolate(Quaternion a, Quaternion b, float fraction);
Matrix transform(Vec3 position, Quaternion rotation, Vec3 scale = {1, 1, 1});
Vec3 operator+(Vec3 a, Vec3 b) noexcept;
Vec3 operator-(Vec3 a, Vec3 b) noexcept;
Vec3 operator*(Vec3 vector, float scale) noexcept;
Vec3 operator/(Vec3 vector, float scale) noexcept;
float dot(Vec3 a, Vec3 b) noexcept;
Vec3 cross(Vec3 a, Vec3 b) noexcept;
float length(Vec3 vector) noexcept;
Vec3 normalized(Vec3 vector) noexcept;
float original_sqrt(float value) noexcept;
float original_length(Vec3 vector) noexcept;
float original_distance(Vec3 a, Vec3 b) noexcept;
Vec3 original_normalized(Vec3 vector) noexcept;
Vec3 interpolate_actor_position(Vec3 previous, Vec3 current, std::uint32_t last_tick,
                                std::uint32_t render_tick, std::uint32_t interval) noexcept;
long double original_dot(Vec3 a, Vec3 b) noexcept;
long double original_plane_distance(Vec3 point, const Plane& plane) noexcept;
void rotate_local(Matrix& matrix, Vec3 authored_angles) noexcept;
void rotate_world(Matrix& matrix, Vec3 authored_angles) noexcept;


// 0044f130 controller words: clocks +00/+04, sample +08, flags +0c,
// mode +10, weight +20, fade clock +24, master +2c and coefficient +130.
struct AnimationControllerState {
    float elapsed{};
    bool loop{};
    float blend{1};
    bool active{};
    std::uint32_t start_tick{},animation_mode{1},flags{3};
    std::uint32_t pause_tick{},sample_tick{},fade_tick{};
    int master_channel{-1};
    float coefficient{1};
};
std::uint32_t animation_duration_ms(const Animation& animation) noexcept;
void reset_animation_controller(AnimationControllerState& state) noexcept;
void play_animation_controller(AnimationControllerState& state,std::uint32_t mode,
                               std::uint32_t now) noexcept;
void reverse_animation_controller(AnimationControllerState& state,std::uint32_t mode,
                                  std::uint32_t duration,std::uint32_t now) noexcept;
void pause_animation_controller(AnimationControllerState& state,std::uint32_t duration,
                                std::uint32_t now) noexcept;
void fade_animation_controller(AnimationControllerState& state,std::uint32_t flag,
                               std::uint32_t duration,std::uint32_t now) noexcept;
void advance_animation_controller(AnimationControllerState& state,std::uint32_t duration,
                                  std::uint32_t now,const AnimationControllerState* master=nullptr,
                                  bool master_matches=false) noexcept;
// Output spans are caller-owned; the mesh and clips remain borrowed.
struct AnimationLayer;
class PoseEvaluator {
public:
    PoseEvaluator(const Mesh& mesh, const Animation* animation);
    void evaluate(float seconds, bool loop, std::span<Matrix> global_pose) const;
    float duration_seconds() const noexcept;
private:
    const Mesh& mesh_;
    const Animation* animation_;
    std::vector<const BoneTrack*> tracks_;
    std::vector<std::uint32_t> track_bones_;
    std::uint32_t duration_ms_{};
    std::vector<std::uint32_t> order_;
    friend void evaluate_layered_pose(const Mesh&, std::span<const AnimationLayer>,
                                     std::span<Matrix>, std::span<BoneFrame>,
                                     std::span<std::uint8_t>);
};
struct AnimationLayer {
    const PoseEvaluator* binding{};
    float seconds{}, weight{1};
    bool loop{};
    std::uint32_t mode{1}; // Original PA types: stop 0, hold 1, loop 2, reverse 3, skip 4.
};
void evaluate_layered_pose(const Mesh& mesh, std::span<const AnimationLayer> layers,
                           std::span<Matrix> global_pose,
                           std::span<BoneFrame> local_scratch,
                           std::span<std::uint8_t> touched_scratch);
void skin_mesh(const Mesh& mesh, std::span<const Matrix> global_pose,
               std::span<Vec3> positions, std::span<Vec3> normals);
std::uint32_t keyframe_duration_ticks(const Keyframes& animation) noexcept;
std::uint32_t keyframe_playback_duration_ticks(const Keyframes& animation);
void evaluate_keyframes(const Keyframes& animation, std::uint32_t elapsed_ms,
                        std::uint32_t duration_ms, bool loop,
                        std::span<Matrix> transforms);
Matrix evaluate_keyframe_track(const TransformTrack& track, std::uint32_t elapsed_ms,
                               std::uint32_t duration_ms, bool camera);
Matrix attachment_transform(const Matrix& object_world, const Mesh& mesh,
                            std::span<const Matrix> global_pose,
                            const Attachment& attachment);

struct SceneWeapon {
    std::string name;
    std::int32_t count{};
};
struct SceneEntry {
    std::string name, mesh, source;
    Vec3 position{}, local_rotation{}, world_rotation{};
    Matrix orientation{identity_matrix()};
    std::array<bool,3> align_positive{}, align_negative{};
    std::optional<SceneWeapon> weapon, ammo;
    std::array<std::string,2> take_weapons;
    std::int32_t drop_ammo{};
    bool drop_health{};
};
struct SceneDefinition {
    std::vector<SceneEntry> entries;
};
SceneDefinition read_scene(const AssetStore& assets, std::string_view logical_name,
                           Vec3 offset = {});

struct Trace {
    bool hit{};
    float fraction{1}, distance{}, leaf_distance{}, plane_distance{};
    Vec3 end{}, normal{};
    std::int32_t brush{-1}, shader{-1};
    std::uint32_t contents{}, surface_flags{};
    float reflection{0.5f};
    std::string_view material_name, shader_name;
};
enum class TriggerTrace : std::uint32_t { miss = 0, enter = 1, exit = 2, inside = 3 };

// Construct once per loaded level. A hull is its local-space min/max relative
// to the traced origin. Queries do not own or copy the level or allocate.
class CollisionWorld {
public:
    explicit CollisionWorld(const Level& level);
    CollisionWorld(const Level& level, const MaterialLibrary& materials);
    const Level& level() const noexcept { return level_; }
    Trace trace(Vec3 start, Vec3 end, Bounds hull, std::uint32_t shader_mask = 0x200);
    Trace trace_ray(Vec3 start, Vec3 direction, float distance, Bounds hull,
                    std::uint32_t shader_mask = 0x200);
    Trace trace_leaf(std::uint32_t leaf, Vec3 start, Vec3 end, Bounds hull,
                     std::uint32_t shader_mask, Vec3 direction) const;
    std::uint32_t point_contents(Vec3 point) const;
    std::int32_t leaf_at(Vec3 point) const;
    bool intersects_brush(std::uint32_t brush, Vec3 origin, Bounds hull) const;
    TriggerTrace trace_trigger(std::uint32_t brush, Vec3 start, Vec3 end, Bounds hull) const;
    bool visible(Vec3 from, Vec3 to) const;
private:
    const Level& level_;
    std::vector<std::uint32_t> shader_flags_;
    // Original shader cull GLenum values: 0, GL_FRONT (0x404), GL_BACK (0x405).
    std::vector<std::uint32_t> shader_cull_;
    std::vector<const Material*> materials_;
    Trace trace_segment(Vec3 start, Vec3 end, Vec3 direction, float distance,
                        Bounds hull, std::uint32_t shader_mask);
    bool trace_leaf(std::uint32_t leaf, Vec3 start, Vec3 end, Vec3 direction,
                    Bounds hull, std::uint32_t shader_mask, Trace& result) const;
    bool trace_node(std::int32_t node, Vec3 start, Vec3 finish,
                    Vec3 direction, Bounds hull, std::uint32_t shader_mask,
                    Trace& result, std::size_t depth);
    bool trace_node_point(std::int32_t node, Vec3 start, Vec3 finish,
                          Vec3 direction, std::uint32_t shader_mask,
                          Trace& result, std::size_t depth);
    bool trace_brush(std::uint32_t brush, Vec3 start, Vec3 end, Vec3 direction,
                     Bounds hull, Trace& candidate) const;
};

struct CharacterMotionState {
    Vec3 position{}, movement_direction{};
    Vec3 gravity_direction{0,0,-1};
    Bounds hull{{-11,-11,-49.08399963378906f},{11,11,49.08399963378906f}};
    float walk_speed{86.4f}, run_speed{171.52f}, acceleration{1600}, gravity{800};
    float movement_speed{}, vertical_speed{};
    std::int32_t movement_step{}, gravity_step{};
    // Original mover +30/+3c/+48/+54, +2c..+2f, +68 and +69.
    std::array<Vec3,4> movement_commands{};
    std::array<bool,4> movement_command_flags{};
    bool movement_walk{}, input_gas{};
    // Original gravity +30..+34, +38 and +3c (actor +3dc..+3e0/+3e4/+3e8).
    std::uint8_t crouch_phase{5};
    bool airborne{true}, jumping{}, crouching{}, fully_crouched{};
    std::int32_t posture_state{};
    std::uint32_t fall_time_tick{};
};
struct CharacterMotionInput {
    bool player{}, movement_bypass{};
    std::uint32_t tick{}, ticks_per_second{};
};
// Fixed original contexts: 00424790 owner hull versus 0041bf00 point support.
enum class CharacterTraceKind { actor_hull, jump_support };
struct CharacterMotionHooks {
    void* context{};
    // Borrowed callback preserves the original direction, including zero rays.
    Trace (*trace)(void*,Vec3,Vec3,float,Bounds,std::uint32_t,CharacterTraceKind){};
    void (*land)(void*,Vec3,const Trace&){};
};
// Exactly one original tick: scheduler advances 33ms, physics recurrence uses
// original float 1/30 independently (0042d960 / 0041e9d0).
bool advance_character(CharacterMotionState& state, CharacterMotionInput input,
                       CollisionWorld& world, CharacterMotionHooks hooks = {});
// Original 0041bf00; caller supplies actor +23c, not a generic jump impulse.
bool jump_character(CharacterMotionState& state, float speed, CollisionWorld& world,
                    CharacterMotionHooks hooks = {});

} // namespace pusu
