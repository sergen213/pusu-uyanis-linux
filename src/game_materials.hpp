#pragma once

#include "renderer.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace pusu {
class AssetStore;
class MaterialLibrary;
class Media;
class ParticleRuntime;
struct ParticleImpact;
struct ParticleRandomSource;

// Original 004626c0 arguments: event category/name, normalized incoming
// direction, contact position/normal and remaining particle collision budget.
enum class PhysicalImpactKind : std::uint8_t { ammunition, particle };
struct PhysicalImpact {
    PhysicalImpactKind kind{PhysicalImpactKind::ammunition};
    std::string_view material, name;
    Vec3 direction{}, position{}, normal{};
    std::uint8_t collisions_left{};
    // Original ammunition caller auxiliary sound-variation state; particle
    // collisions instead use collisions_left and never mutate this pointer.
    std::uint32_t* previous_variant{};
};

struct ProjectedGeometry {
    Mesh mesh;
    std::vector<std::array<std::uint8_t,4>> colors;
    std::uint64_t generation{};
};
struct WorldProjection {
    std::vector<ProjectedGeometry> batches;
    std::size_t active_batches{};
};

class PhysicalMaterials {
public:
    PhysicalMaterials(AssetStore&, const MaterialLibrary&, Media&, ParticleRuntime&);
    ~PhysicalMaterials();
    PhysicalMaterials(const PhysicalMaterials&) = delete;
    PhysicalMaterials& operator=(const PhysicalMaterials&) = delete;
    // A scene shader is resolved through Material::physical_material_name;
    // a physical material name may be supplied directly by model hit regions.
    void impact(std::string_view shader_or_material, std::string_view event,
                Vec3 position, Vec3 direction, Vec3 normal,
                std::uint8_t remaining_collision_budget = 0);
    void impact(const ParticleImpact&);
    void impact(const PhysicalImpact&);
    // Scene-owned geometry remains borrowed until clear/set_level.
    void set_level(const Level*);
    // Original 004259a0 transient projection: supplied center, depth 20.
    // Requires set_level; no persistent decals, sounds or particles are created.
    // Borrow batches[0..active_batches) until the next projection or destruction.
    // Inactive batches retain their allocations; indices never exceed uint16.
    void project_world(WorldProjection&, Vec3 center, Vec3 normal, Vec3 u,
                       float width, float height);
    void footstep(std::string_view shader_or_material, Vec3 position,
                  Vec3 direction = {}, Vec3 normal = {0,0,1},
                  std::uint32_t* previous_variant = nullptr);
    void jump_up(std::string_view shader_or_material, Vec3 position);
    void jump(std::string_view shader_or_material, Vec3 position,
              Vec3 direction = {}, Vec3 normal = {0,0,1},
              std::uint32_t* previous_variant = nullptr);
    void fall(std::string_view shader_or_material, Vec3 position,
              Vec3 direction = {}, Vec3 normal = {0,0,1},
              std::uint32_t* previous_variant = nullptr);
    void death(std::string_view shader_or_material, Vec3 position,
               Vec3 direction = {}, Vec3 normal = {0,0,1});
    // Actor death and ammunition sounds use different original environment
    // folders; callers need not construct or guess filesystem paths.
    void sound_environment(bool enabled);
    // Binds the shared game MSVCRT stream; actual random effects require it.
    // Binding is external state and remains attached across load/swap_state.
    void set_random_source(ParticleRandomSource);
    // Rebuilds borrowed renderer views only after new clipped geometry lands.
    void update(float seconds);
    void clear();
    void save(std::ostream&) const;
    void load(std::istream&);
    void swap_state(PhysicalMaterials&) noexcept;
    // Borrowed meshes and material names remain valid until update/impact/clear.
    std::span<const RenderObject> objects() const noexcept;
    std::uint64_t resource_revision() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// Permanent parser, selection and persistent/transient projection checks; no GL/audio context.
void physical_material_runtime_check();
void physical_material_runtime_check(const AssetStore&);
} // namespace pusu
