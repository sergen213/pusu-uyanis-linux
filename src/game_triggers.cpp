#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pusu {
namespace {
// Trigger bounds only build the original leaf VIS; actual contact always uses
// the indexed convex brush (00422a70), including its serialized side order.
bool trigger_bounds_overlap(Bounds a, Bounds b) noexcept {
    // 004218d0 keeps these float center/half intermediates.
    const Vec3 center = (a.minimum + a.maximum) * 0.5f;
    const Vec3 half = a.maximum - center;
    return b.minimum.x - half.x <= center.x && center.x <= b.maximum.x + half.x &&
           b.minimum.y - half.y <= center.y && center.y <= b.maximum.y + half.y &&
           b.minimum.z - half.z <= center.z && center.z <= b.maximum.z + half.z;
}
}

void Game::reset_trigger_state(TriggerState& state, const Level& level) {
    ++state.generation;
    state.inside.assign(level.triggers.size(), 0);
    state.last_ticks.assign(level.triggers.size(), 0);
    state.encounter_stamps.assign(level.triggers.size(), 0);
    state.active.clear();
    state.active.reserve(level.triggers.size());
    state.leaf_offsets.assign(level.leaves.size() + 1, 0);
    state.leaf_triggers.clear();
    // xUpdateTriggerVIS rejects nonoverlapping ancestors as well as leaves.
    // Two load-time passes produce original record order without frame scratch.
    const auto visit = [&](auto&& self, std::int32_t child, Bounds bounds,
                           std::size_t depth, auto&& leaf) -> void {
        if (depth > level.nodes.size()) throw std::runtime_error("Cyclic trigger BSP tree");
        if (child < 0) {
            const auto index = static_cast<std::uint32_t>(~child);
            if (trigger_bounds_overlap(bounds, level.leaves.at(index).bounds.bounds)) leaf(index);
            return;
        }
        const auto& node = level.nodes.at(static_cast<std::size_t>(child));
        if (!trigger_bounds_overlap(bounds, node.bounds.bounds)) return;
        self(self, node.children[0], bounds, depth + 1, leaf);
        self(self, node.children[1], bounds, depth + 1, leaf);
    };
    const auto root = level.nodes.empty() ? -1 : 0;
    for (const auto& trigger : level.triggers)
        visit(visit, root, trigger.bounds, 0, [&](std::uint32_t leaf) { ++state.leaf_offsets[leaf + 1]; });
    for (std::size_t i = 1; i < state.leaf_offsets.size(); ++i)
        state.leaf_offsets[i] += state.leaf_offsets[i - 1];
    state.leaf_triggers.resize(state.leaf_offsets.back());
    auto cursor = state.leaf_offsets;
    for (std::uint32_t i = 0; i < level.triggers.size(); ++i)
        visit(visit, root, level.triggers[i].bounds, 0,
              [&](std::uint32_t leaf) { state.leaf_triggers[cursor[leaf]++] = i; });
}

void Game::trace_trigger_state(TriggerState& state, CollisionWorld& collision,
                              Vec3 start, Vec3 end, Bounds hull,
                              std::uint64_t frame, std::uint32_t tick,
                              script::Runtime& runtime) {
    const auto generation = state.generation;
    const Vec3 center = (hull.minimum + hull.maximum) * 0.5f;
    start = start + center; end = end + center;
    const Vec3 extent = (hull.maximum - hull.minimum) * 0.5f;
    const Bounds centered_hull{extent * -1.0f, extent};
    const auto visit = [&](auto&& self, std::int32_t child, Vec3 a, Vec3 b,
                           std::size_t depth) -> void {
        if (state.generation != generation) return;
        if (depth > collision.level().nodes.size()) throw std::runtime_error("Cyclic trigger BSP tree");
        if (child < 0) {
            const auto leaf = static_cast<std::uint32_t>(~child);
            const auto first = state.leaf_offsets.at(leaf);
            const auto finish = state.leaf_offsets.at(leaf + 1);
            for (auto at = first; at < finish; ++at) {
                const auto index = state.leaf_triggers[at];
                if (state.encounter_stamps[index] == frame) continue;
                const auto result = collision.trace_trigger(collision.level().triggers[index].brush,
                                                             a, b, centered_hull);
                if (result == TriggerTrace::enter && !state.inside[index]) {
                    // 004231d9..0042321c: stamp and prepend BEFORE the callback.
                    state.encounter_stamps[index] = frame;
                    state.inside[index] = 1;
                    state.last_ticks[index] = tick;
                    state.active.push_back(index);
                    runtime.invoke_event(collision.level().triggers[index].name, "_on_enter");
                } else if (result == TriggerTrace::exit && state.inside[index]) {
                    // 0042313e: the callback still sees active membership and the
                    // old stamp. Re-find its entry after script mutations.
                    runtime.invoke_event(collision.level().triggers[index].name, "_on_exit");
                    if (state.generation != generation) return;
                    state.encounter_stamps[index] = frame;
                    const auto entry = std::find(state.active.rbegin(), state.active.rend(), index);
                    if (entry != state.active.rend()) state.active.erase(entry.base() - 1);
                    state.inside[index] = 0;
                }
                // An immediate load_bsp may replace both the level and collision
                // object. No borrowed geometry/list/string survives invocation.
                if (state.generation != generation) return;
            }
            return;
        }
        // 00423260 is a dedicated hull-expanded traversal, not the solid trace:
        // back first, then front, no first-contact short circuit.
        const auto node = collision.level().nodes.at(static_cast<std::size_t>(child));
        const auto plane = collision.level().planes.at(node.plane);
        const float radius = static_cast<float>(
            (static_cast<long double>(std::abs(plane.normal.z)) * extent.z +
             static_cast<long double>(std::abs(plane.normal.y)) * extent.y) +
             static_cast<long double>(std::abs(plane.normal.x)) * extent.x);
        float da = static_cast<float>(original_plane_distance(a, plane) - radius);
        float db = static_cast<float>(original_plane_distance(b, plane) - radius);
        Vec3 back_start = a, back_end = b;
        if (da < 0.001f || db < 0.001f) {
            if (da >= 0.001f) back_start = a + (b - a) * (da / (da - db));
            else if (db >= 0.001f) back_end = a + (b - a) * (da / (da - db));
            self(self, node.children[1], back_start, back_end, depth + 1);
        }
        if (state.generation != generation) return;
        da = static_cast<float>(original_plane_distance(a, plane) + radius);
        db = static_cast<float>(original_plane_distance(b, plane) + radius);
        Vec3 front_start = a, front_end = b;
        if (da > -0.001f || db > -0.001f) {
            if (da <= -0.001f) front_start = a + (b - a) * (da / (da - db));
            else if (db <= -0.001f) front_end = a + (b - a) * (da / (da - db));
            self(self, node.children[0], front_start, front_end, depth + 1);
        }
    };
    visit(visit, collision.level().nodes.empty() ? -1 : 0, start, end, 0);
}

void Game::repeat_trigger_state(TriggerState& state, const Level& level,
                               std::uint32_t tick, script::Runtime& runtime) {
    const auto generation = state.generation;
    for (std::size_t at = state.active.size(); at != 0;) {
        const auto index = state.active[--at];
        const float elapsed = static_cast<float>(tick - state.last_ticks[index]);
        // 00422e20: unsigned elapsed rounded to float, x87 frequency*PL float,
        // strict greater-than; one callback, never a catch-up loop.
        if (static_cast<long double>(elapsed) >
            1000.0L * level.triggers[index].repeat_interval_seconds) {
            runtime.invoke_event(level.triggers[index].name, "_on_inside");
            if (state.generation != generation) return;
            state.last_ticks[index] = tick;
        }
    }
}

void Game::trace_triggers(Vec3 start, Vec3 end, Bounds hull) {
    if (!collision_ || !level_) return;
    trace_trigger_state(triggers_, *collision_, start, end, hull,
                        frame_stamp_, game_tick_, runtime_);
}

void Game::update_triggers() {
    if (level_) repeat_trigger_state(triggers_, *level_, game_tick_, runtime_);
}

void Game::use_trigger_state(TriggerState& state, CollisionWorld& collision,
                             Vec3 point, std::uint64_t frame,
                             script::Runtime& runtime, std::string_view suffix) {
    const auto generation = state.generation;
    const auto leaf = static_cast<std::uint32_t>(collision.leaf_at(point));
    const auto first = state.leaf_offsets.at(leaf);
    const auto finish = state.leaf_offsets.at(leaf + 1);
    for (auto at = first; at < finish; ++at) {
        const auto index = state.leaf_triggers[at];
        if (state.encounter_stamps[index] == frame) continue;
        if (!collision.intersects_brush(collision.level().triggers[index].brush, point, {})) continue;
        // invoke_event owns the concatenated name before invoking the VM. Use
        // queries never update encounter stamps (00422d40).
        runtime.invoke_event(collision.level().triggers[index].name, suffix);
        if (state.generation != generation) return;
    }
}

void Game::use_triggers(Vec3 point, std::string_view suffix) {
    if (collision_ && level_)
        use_trigger_state(triggers_, *collision_, point, frame_stamp_, runtime_, suffix);
}
} // namespace pusu
