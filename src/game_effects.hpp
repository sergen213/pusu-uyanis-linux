#pragma once

#include "renderer.hpp"
#include "scene_math.hpp"
#include <memory>
#include <iosfwd>
#include <optional>
#include <span>
#include <string_view>

namespace pusu {
class AssetStore;
class MaterialLibrary;

// Callback borrows the game entity store. Missing entities/attachments return
// nullopt; no allocation or ownership transfer occurs during an update.
struct ParticleAttachmentLookup {
    void* context{};
    std::optional<Matrix> (*transform)(void*, std::string_view entity,
                                      std::string_view attachment){};
};
struct ParticleImpact {
    std::string_view definition, material;
    Vec3 direction, position, normal;
    std::uint8_t collisions_left{};
};
struct ParticleSettlement {
    std::string_view object;
    Vec3 position;
    std::array<std::int32_t,5> pickup{};
};
struct ParticleRandomSource {
    void* context{};
    std::uint32_t (*word)(void*){}; // Shared original MSVCRT rand(), inclusive0..32767.
};

class ParticleRuntime {
public:
    ParticleRuntime(AssetStore& assets, const MaterialLibrary& materials);
    ~ParticleRuntime();
    ParticleRuntime(const ParticleRuntime&) = delete;
    ParticleRuntime& operator=(const ParticleRuntime&) = delete;
    void create(std::string_view name, std::string_view definition, bool active,
                Vec3 position, Vec3 emitter_size);
    // Original engine callers may also supply direction and two world-plane
    // sprite axes. Script create_particle_system leaves all three absent.
    void create(std::string_view name, std::string_view definition, bool active,
                Vec3 position, std::optional<Vec3> emitter_size, std::optional<Vec3> direction,
                std::optional<std::array<Vec3, 2>> plane_axes);
    void create_dropped_weapon(std::string_view name, std::string_view definition,
                               Vec3 position, Vec3 direction,
                               std::array<std::int32_t,5> pickup);
    void attach(std::string_view name, std::string_view entity,
                std::string_view attachment = {});
    void detach(std::string_view name);
    void start(std::string_view name);
    void stop(std::string_view name);
    void clear();
    void save(std::ostream& output) const;
    void load(std::istream& input);
    void swap_state(ParticleRuntime& other) noexcept;
    void set_camera(const RenderCamera& camera);
    void update(float seconds, CollisionWorld& collision,
                ParticleAttachmentLookup attachments = {});
    // Views are borrowed; any mutating runtime call may change or invalidate them.
    std::span<const RenderObject> objects() const noexcept;
    std::span<const RenderParticle> particles() const noexcept;
    void set_random_source(ParticleRandomSource random);
    // Original physical-material event kind 1. Dispatch after update returns:
    // handlers may create more groups without invalidating the simulation.
    std::span<const ParticleImpact> impacts() const noexcept;
    std::span<const ParticleSettlement> settlements() const noexcept;
    // Consumer preparation revision: newly drawable resources and object draw
    // count/slot mesh-material changes invalidate preparation. Transform-only
    // motion does not; final scene publication must observe this revision.
    std::uint64_t resource_revision() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// Authored parser and deterministic simulation invariants, without GL/assets IO.
void particle_runtime_check();
} // namespace pusu
