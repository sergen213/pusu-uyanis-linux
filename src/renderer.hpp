#pragma once

#include "assets.hpp"
#include "settings.hpp"
#include "media.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace pusu {
class AssetStore;
class MaterialLibrary;
struct Material;

inline constexpr Matrix render_identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
struct RenderCamera {
    Vec3 position;
    Matrix view{render_identity};
    Matrix frame0{render_identity}; // Original camera logical basis, before view inversion.
    float near_plane{4.0f}, far_plane{20000.0f};
    float reference_fov_degrees{};
};
struct RenderObject {
    const Mesh* mesh{};
    std::string_view material;
    Matrix transform{render_identity};
    // Animated model-space global bones, in mesh bone order; empty = bind pose.
    std::span<const Matrix> bones;
    std::array<float,4> color{1,1,1,1};
    bool visible{true};
    // Stable entity/part lifetime identity for original light-grid dirty/cache state.
    std::uint64_t lighting_id{};
    std::span<const std::array<std::uint8_t,4>> vertex_colors;
    // Mutable authored decal aggregation; zero denotes immutable original PM.
    std::uint64_t geometry_generation{};
    const Material* material_override{};
    // Current mesh OWNER's logical frame-0 translation; derived frame-1 is draw transform.
    std::optional<Vec3> lighting_origin;
    bool frustum_cull{true};
    const Matrix* culling_transform{};
    const Bounds* root_bounds{};
    bool culling_enabled{true};
};
struct RenderParticle {
    Vec3 position;
    Vec2 size{8,8};
    std::array<float,4> color{1,1,1,1};
    std::string_view material;
    float rotation{};
    bool autosprite{true};
    // Rotation-only local axes for authored no_autosprite; position above is origin.
    Matrix orientation{render_identity};
    bool triangle{};
    // Actual original oversize sprite triangle, emitted in world space by runtime.
    std::array<Vec3,3> triangle_positions{};
    std::array<Vec2,3> triangle_uv{{{0.5f,-0.8660254f},{-0.5773503f,1},{1.5773503f,1}}};
};
struct RenderFog {
    bool enabled{};
    int mode{0x2601}; // Original GL_LINEAR, GL_EXP(0x800), GL_EXP2(0x801).
    std::array<float,4> color{};
    float start{}, end{}, density{};
};
struct RenderScene {
    const Level* level{};
    std::uint64_t generation{};
    RenderCamera camera;
    std::span<const RenderObject> objects;
    std::span<const RenderParticle> particles;
    double time_seconds{};
    std::array<float,4> clear_color{0,0,0,1};
    RenderFog fog;
    bool player_effect{};
    std::uint32_t tick_milliseconds{};
    std::uint64_t player_effect_serial{};
    bool gameplay_frame{};
};
struct RenderRect { float x{}, y{}, width{}, height{}; };
struct InterfaceQuad {
    RenderRect rect;
    RenderRect uv{0,0,1,1};
    std::array<float,4> color{1,1,1,1};
    // Original authored shader name; empty draws an untextured solid quad.
    std::string_view shader;
    float rotation_degrees{};
    Vec2 rotation_center{};
    bool mask_legacy_margins{};
    bool full_viewport{};
    std::optional<float> native_bar_fraction{};
    bool history_capture_visible{};
    bool drawable_pixel_coordinates{};
    bool font_atlas{}; // shader holds the original font image path, not a material.
};

// Requires a current GL 3.3 context and initialized GLEW. No context ownership.
struct OriginalGraphicsOptions {
    bool motion_blur{}, texture_compression{true}, reflections{true};
    bool lightmap_compression{}, reflection_compression{true};
    int motion_blur_size{256}, motion_blur_frames{3};
    float motion_blur_wait_seconds{}, motion_blur_alpha{0.60000002384185791015625f};
    int texture_divisor{1}, lightmap_divisor{1}, reflection_divisor{1};
    int texture_filter{2}, lightmap_filter{2}, reflection_filter{2};
    bool operator==(const OriginalGraphicsOptions&) const = default;
};
// Implementation support; Main additionally queries driver anisotropy/MSAA limits.
// SDL's context owner applies vsync; this backend never supplies hardware ray tracing.
namespace renderer_features {
inline constexpr bool hardware_ray_tracing=false, msaa=true, anisotropy=true;
inline constexpr bool bloom=true, gamma=true, vsync=true;
}
class Renderer {
public:
    using LevelProgress=void(*)(void*,float);
    Renderer(AssetStore& assets, MaterialLibrary& materials, const Settings& settings);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void resize(int drawable_width, int drawable_height);
    void apply_settings(const Settings& settings);
    void apply_original_options(const OriginalGraphicsOptions& options);
    // Build the borrowed draw list before deciding whether to prepare. Its producer
    // must invalidate preparation when shader/font_atlas resource identities change;
    // rect, UV, tint and visibility changes using prepared identities need no GPU work.
    // Quad drawing only looks up prepared resources; it never loads them or owns UI state.
    void prepare(const RenderScene& scene, std::span<const InterfaceQuad> interface);
    void render(const RenderScene& scene, std::span<const InterfaceQuad> interface = {});
    void prepare_level(const Level& level,std::uint64_t generation,LevelProgress progress,void* context);
    // Actual load/cache publication after complete scene preparation, before first frame.
    // bsp_path is the original world PL path; only its final .pl suffix is removed.
    void prepare_reflections(const RenderScene& scene,std::string_view bsp_path);
    void render_loading(std::span<const InterfaceQuad> interface);
    void after_present(const RenderScene& scene,std::span<const InterfaceQuad> interface={});
private:
    struct State;
    std::unique_ptr<State> state_;
};

#ifdef PUSU_RENDERER_MATH_CHECK
// CPU rendering invariants; callable by a permanent standalone check without GL.
void renderer_math_check();
#endif
float renderer_horizontal_fov(float reference_degrees, float aspect);
std::array<std::uint8_t,4> original_world_color(std::array<std::uint8_t,4> stored);
} // namespace pusu
