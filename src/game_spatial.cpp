#include "game.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace pusu {
namespace {
constexpr float membership_epsilon = .001f;

long double membership_dot(Vec3 a, Vec3 b) noexcept {
    // 00444420 / 00444590 retain x87 precision, summing z+y before x.
    return (static_cast<long double>(a.z) * b.z + static_cast<long double>(a.y) * b.y) +
           static_cast<long double>(a.x) * b.x;
}

float membership_support(const Plane& plane, const Matrix& world, Vec3 half) noexcept {
    const Vec3 x{world[0], world[1], world[2]};
    const Vec3 y{world[4], world[5], world[6]};
    const Vec3 z{world[8], world[9], world[10]};
    const float dx = static_cast<float>(membership_dot(plane.normal, x));
    const float dy = static_cast<float>(std::abs(membership_dot(plane.normal, y)));
    const auto dz = std::abs(membership_dot(plane.normal, z));
    return static_cast<float>((std::abs(static_cast<long double>(dx)) * half.x + dz * half.z) +
                              static_cast<long double>(dy) * half.y);
}

template<class Work, class Visit>
void membership_bounds(const Level& level, const Matrix& world, Bounds bounds,
                       Work& stack, Visit&& visit) {
    const Vec3 half = (bounds.maximum - bounds.minimum) * .5f;
    const Vec3 center = transform_point(world, (bounds.minimum + bounds.maximum) * .5f);
    stack.clear();
    stack.push_back(level.nodes.empty() ? -1 : 0);
    while (!stack.empty()) {
        const auto work = stack.back();
        stack.pop_back();
        if (work < 0) {
            visit(static_cast<std::uint32_t>(~work));
            continue;
        }
        const auto& node = level.nodes[work];
        const auto& plane = level.planes[node.plane];
        const float radius = membership_support(plane, world, half);
        const auto distance = membership_dot(plane.normal, center) - plane.distance;
        // 004222e0 visits back before front; the stack reverses push order.
        if (distance + radius > -membership_epsilon) stack.push_back(node.children[0]);
        if (distance - radius < membership_epsilon) stack.push_back(node.children[1]);
    }
}

bool membership_spheres(Vec3 center, long double combined, const AdvancedBounds& bounds) noexcept {
    const Vec3 delta = bounds.center - center;
    return membership_dot(delta, delta) <= combined * combined;
}

template<class Work, class Visit>
void membership_sphere(const Level& level, Vec3 center, float radius,
                       Work& stack, Visit&& visit) {
    stack.clear();
    stack.push_back(level.nodes.empty() ? -1 : 0);
    while (!stack.empty()) {
        const auto work = stack.back();
        stack.pop_back();
        if (work < 0) {
            const auto leaf = static_cast<std::uint32_t>(~work);
            const auto& bounds = level.leaves[leaf].bounds;
            if (membership_spheres(center, static_cast<long double>(radius) + bounds.radius, bounds)) visit(leaf);
            continue;
        }
        const auto& node = level.nodes[work];
        if (!membership_spheres(center, radius + node.bounds.radius, node.bounds)) continue;
        // 00422eb0 has the opposite visitation order to bounds registration.
        stack.push_back(node.children[1]);
        stack.push_back(node.children[0]);
    }
}

std::uint32_t membership_next_stamp(std::uint32_t& stamp,
                                    std::vector<std::uint32_t>& seen) noexcept {
    if (++stamp == 0) {
        std::fill(seen.begin(), seen.end(), 0);
        ++stamp;
    }
    return stamp;
}
}

void Game::prepare_membership() {
    if (!level_) return;
    membership_stack_.reserve(level_->nodes.size() + 1);
    if (entities_.size() <= membership_capacity_) return;
    membership_capacity_ = std::max(entities_.size(), std::max(std::size_t{16}, membership_capacity_ * 2));
    membership_seen_.resize(membership_capacity_, 0);
    explosion_candidates_.reserve(membership_capacity_);
    for (auto& leaf : leaf_entities_) leaf.reserve(membership_capacity_);
}

void Game::clear_membership() {
    for (auto& e : entities_) e.membership_leaves.clear();
    leaf_entities_.clear();
    // 004200e0 constructs N+1 leaves; 004856b4 is N, the non-geometric fallback.
    fallback_leaf_ = level_ ? static_cast<std::uint32_t>(level_->leaves.size()) : 0;
    if (level_) leaf_entities_.resize(level_->leaves.size() + 1);
    membership_stack_.clear();
    std::fill(membership_seen_.begin(), membership_seen_.end(), 0);
    membership_stamp_ = 0;
    // A new level has new leaf vectors even when entity capacity is unchanged.
    for (auto& leaf : leaf_entities_) leaf.reserve(membership_capacity_);
    prepare_membership();
}

void Game::refresh_membership(Entity& e) {
    for (const auto leaf : e.membership_leaves) {
        if (leaf < leaf_entities_.size()) std::erase(leaf_entities_[leaf], e.handle);
    }
    e.membership_leaves.clear();
    if (!level_ || !e.alive || e.kind == 3 || level_->leaves.empty()) return;
    prepare_membership();
    e.membership_leaves.reserve(level_->leaves.size());
    if (const auto* actor = actors_.find(e.handle)) {
        e.position = actor->position;
        e.orientation = actor->orientation;
    }
    // 00422400 reads frame0 verbatim. Actor canonical position is independent
    // of frame0 translation until original movement restores it (004179f0).
    const auto add = [&](std::uint32_t leaf) {
        // 00432a30 prepends both reciprocal lists; no name sorting or visibility filter.
        leaf_entities_[leaf].insert(leaf_entities_[leaf].begin(), e.handle);
        e.membership_leaves.push_back(leaf);
    };
    // 00422400 uses cached root PO bounds and current entity axes. Bone-attached
    // col_* bounds participate in leaf narrowphase, not the root's registration.
    if (e.model.has_bounds) membership_bounds(*level_, e.orientation, e.model.bounds, membership_stack_, add);
    if (e.membership_leaves.empty()) add(fallback_leaf_);
}

void Game::collect_explosion_entities(Vec3 center, float radius,
                                      std::vector<std::uint32_t>& result) {
    result.clear();
    if (!level_ || level_->leaves.empty()) return;
    const auto stamp = membership_next_stamp(membership_stamp_, membership_seen_);
    const float squared_radius = radius * radius;
    membership_sphere(*level_, center, radius, membership_stack_, [&](std::uint32_t leaf) {
        for (const auto handle : leaf_entities_[leaf]) {
            if (membership_seen_[handle - 1] == stamp) continue;
            membership_seen_[handle - 1] = stamp;
            const auto& e = entities_[handle - 1];
            const Vec3 delta = e.position - center;
            if (e.alive && e.kind == 0 && membership_dot(delta, delta) <= squared_radius) result.push_back(handle);
        }
    });
}

void game_spatial_check() {
    Level level;
    level.planes.push_back({{1, 0, 0}, 0});
    Node root;
    root.children = {-1, -2};
    root.bounds.radius = 10;
    level.nodes.push_back(root);
    level.leaves.resize(2);
    for (auto& leaf : level.leaves) leaf.bounds.radius = 10;
    std::vector<std::int32_t> stack;
    stack.reserve(2);
    std::vector<std::uint32_t> leaves;
    leaves.reserve(2);
    Matrix world = identity_matrix();
    membership_bounds(level, world, {{-2, -1, -1}, {2, 1, 1}}, stack,
                      [&](auto leaf) { leaves.push_back(leaf); });
    if (leaves != std::vector<std::uint32_t>{1, 0})
        throw std::logic_error("Original BSP registration must visit back before front");
    leaves.clear();
    world[0] = world[5] = 0;
    world[1] = 1;
    world[4] = -1;
    world[12] = 1.5f;
    membership_bounds(level, world, {{-2, -1, -1}, {2, 1, 1}}, stack,
                      [&](auto leaf) { leaves.push_back(leaf); });
    if (leaves != std::vector<std::uint32_t>{0})
        throw std::logic_error("BSP membership must project posed axes, not a guessed radius");
    world = identity_matrix();
    leaves.clear();
    world[12] = membership_epsilon;
    membership_bounds(level, world, {}, stack, [&](auto leaf) { leaves.push_back(leaf); });
    if (leaves != std::vector<std::uint32_t>{0})
        throw std::logic_error("Original BSP registration margin must be strict");
    leaves.clear();
    membership_sphere(level, {}, 1, stack, [&](auto leaf) { leaves.push_back(leaf); });
    if (leaves != std::vector<std::uint32_t>{0, 1})
        throw std::logic_error("Original explosion traversal must visit front before back");
    leaves.clear();
    level.leaves[0].bounds.center = {5, 0, 0};
    level.leaves[0].bounds.radius = .1f;
    membership_sphere(level, {}, 1, stack, [&](auto leaf) { leaves.push_back(leaf); });
    if (leaves != std::vector<std::uint32_t>{1})
        throw std::logic_error("Original explosion query must prune each leaf sphere");
    std::uint32_t stamp = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> seen{7, 3};
    if (membership_next_stamp(stamp, seen) != 1 || seen[0] || seen[1])
        throw std::logic_error("Membership timestamp wrap must clear dedup state");
}
} // namespace pusu
