#include "game.hpp"
#include "resources.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace pusu {
namespace {
bool prop_damage(float& health, float damage) noexcept {
    // 0041a8c0: COMISS(0, health) / JC treats unordered health as nonlethal.
    health -= damage;
    if (health <= 0) {
        health = 0;
        return true;
    }
    return false;
}
bool prop_blast_damage(Vec3 origin, Vec3 target, float radius, float damage, float& applied) noexcept {
    const float distance = length(target - origin);
    // Exact0042d3d0 boundary and origin-distance falloff, no occlusion test.
    if (distance > radius - .01f) return false;
    applied = (1.f - distance / radius) * damage;
    return true;
}
}

void Game::configure_prop(Entity& e) {
    if (e.kind != 0 || !e.model.definition) return;
    const auto& definition = *e.model.definition;
    if (definition.health && (!definition.damage || !definition.damage_radius))
        throw std::runtime_error("Destructible original PO has unspecified blast metadata: " + e.object_name);
    // 00450ff0 initializes health through virtual+14 to FLT_MAX before PO directives.
    e.health = definition.health.value_or(std::numeric_limits<float>::max());
    // All ten authored finite-health POs define both blast values. These zero
    // storage values are never substituted for a destructible PO directive.
    e.explosion_damage = definition.damage.value_or(0);
    e.explosion_radius = definition.damage_radius.value_or(0);
    e.destruction_started = false;
}

void Game::queue_prop_explosion(Entity& e) {
    if (std::any_of(explosions_.begin(), explosions_.end(), [&](const QueuedExplosion& queued) {
        return queued.entity == e.handle;
    })) return;
    // 00402160 prepends; snapshot exactly the original six-word payload.
    explosions_.push_front({e.handle, e.position, e.explosion_radius, e.explosion_damage});
}

void Game::damage_entity(std::uint32_t handle, float damage, std::uint32_t source,
                         const ActorHit& hit) {
    if (!std::isfinite(damage)) throw std::runtime_error("Non-finite native entity damage");
    auto& e = entity(handle);
    if (e.kind != 0) {
        actors_.hurt(handle, damage, source);
        return;
    }
    if (e.destruction_started || !prop_damage(e.health, damage)) return;
    const QueuedExplosion explosion{e.handle, e.position, e.explosion_radius, e.explosion_damage};
    // Player firearm caller00416d50 plays the authored environmental blow before
    // immediately invoking0042d3d0. Chained explosions do not replay this sound.
    if (e.model.definition && e.model.definition->blow_sound)
        media_.environment_sound("effect", "blow", *e.model.definition->blow_sound,
                                 1, explosion.position, true);
    (void)hit; // Impact material handling belongs to the firing caller.
    process_explosion(explosion);
}

void Game::destroy_prop(std::uint32_t handle) {
    auto& e = entity(handle);
    if (e.destruction_started) return;
    e.destruction_started = true;
    const auto name = e.name;
    const auto position = e.position;
    const auto model = e.model.name;
    const auto death = name + "_on_dead";
    if (runtime_.contains(death)) {
        // 0041aa70 pushes source,z,y,x; original scripts pop x,y,z,source.
        runtime_.push_word(handle);
        runtime_.push_float(position.z);
        runtime_.push_float(position.y);
        runtime_.push_float(position.x);
        runtime_.invoke(death);
        return;
    }
    const auto fragment_name = name + "_" + std::to_string(host_random());
    const auto particle_name = name + "_" + std::to_string(host_random());
    const auto group = model + "_dead_group";
    effects_.create(particle_name, group, true, position, std::nullopt, std::nullopt, std::nullopt);
    const auto sound = model + "_dead_" + std::to_string(host_random() % 3 + 1) + ".ogg";
    // Original raw sample loader prefixes sound/ and permits absent optional clips.
    media_.sound("sound/" + sound, 1, false, position, true);
    // Original0041aa70 independently selects sound, existence probe, and dead model.
    const auto probe = model + "_dead_" + std::to_string(host_random() % 3 + 1);
    if (!assets_.contains("object/po/" + probe + ".po")) return;
    const auto fragment = create_entity(fragment_name, 0);
    auto& dead = entity(fragment);
    const auto dead_model = model + "_dead_" + std::to_string(host_random() % 3 + 1);
    set_mesh(dead, dead_model);
    // virtual+20 copies only the live object's position, not its orientation.
    host_set_position(fragment, position);
    const auto& animation = dead_model; // 0041af40 reuses the assigned PO name buffer.
    try {
        host_keyframe(animation, 0, 0, fragment_name);
    } catch (const std::exception& error) {
        throw std::runtime_error("Original automatic prop death " + name + " (" + model +
                                 "), fragment " + dead_model + ", PKA " + animation + ": " + error.what());
    }
}

void Game::process_explosion(const QueuedExplosion& explosion) {
    if (!explosion.entity || explosion.entity > entities_.size() ||
        !entities_[explosion.entity - 1].alive) return;
    destroy_prop(explosion.entity);
    if (entities_[explosion.entity - 1].alive) host_delete_entity(explosion.entity);
    // 0041bb90 also removes every queued payload naming the removed entity.
    std::erase_if(explosions_, [&](const QueuedExplosion& item) { return item.entity == explosion.entity; });
    collect_explosion_entities(explosion.position, explosion.radius, explosion_candidates_);
    for (const auto handle : explosion_candidates_) {
        auto& e = entities_[handle - 1];
        if (!e.alive || e.kind != 0 || handle == player_ ||
            e.model.name.find("dead") != std::string::npos) continue; // Original004680a0 is case-sensitive strstr.
        float applied{};
        if (!prop_blast_damage(explosion.position, e.position, explosion.radius, explosion.damage, applied) ||
            !prop_damage(e.health, applied)) continue;
        if (std::any_of(explosions_.begin(), explosions_.end(), [&](const QueuedExplosion& item) {
            return item.entity == handle;
        })) return; // Original duplicate branch0042d5c2 exits the entire scan.
        queue_prop_explosion(e);
    }
}

void Game::host_explosion() {
    if (!explosions_.empty()) {
        const auto explosion = explosions_.front();
        process_explosion(explosion);
    }
    // 0042d890 schedules again even with an empty queue. The random normalizer
    // is the actual00476450 DWORD, not an invented blast/update frame rate.
    const float delay = (float(host_random()) * std::bit_cast<float>(std::uint32_t{0x38000100}) + 1.f) * .01f;
    host_timed_event("explosion", delay);
}

void prop_damage_check() {
    float health = .35f;
    if (prop_damage(health, .2f) || !prop_damage(health, .2f) || health != 0)
        throw std::logic_error("Original prop health subtraction/clamp changed");
    health = std::numeric_limits<float>::quiet_NaN();
    if (prop_damage(health, .2f) || !std::isnan(health))
        throw std::logic_error("Original unordered health comparison changed");
    float applied{};
    if (!prop_blast_damage({}, {50, 0, 0}, 100, .6f, applied) || applied != .3f ||
        !prop_blast_damage({}, {.99f, 0, 0}, 1, 1, applied) ||
        prop_blast_damage({}, {1, 0, 0}, 1, 1, applied) ||
        prop_blast_damage({}, {}, 0, 1, applied))
        throw std::logic_error("Original explosion falloff/boundary changed");
}
} // namespace pusu
