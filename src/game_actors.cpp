#include "game_actors.hpp"
#include "game.hpp"
#include "resources.hpp"
#include "scene_math.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pusu {

namespace {
void player_yaw(ActorState&, float) noexcept;
bool inventory_equal(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size()) return false;
    // 0046acf0: fold only ASCII A..Z, never locale/Turkish high bytes.
    const auto fold = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c;
    };
    for (std::size_t i = 0; i != first.size(); ++i)
        if (fold(static_cast<unsigned char>(first[i])) !=
            fold(static_cast<unsigned char>(second[i]))) return false;
    return true;
}
}

ActorState* ActorRuntime::find(EntityHandle entity) noexcept {
    const auto found = std::find_if(actors_.begin(), actors_.end(),
        [entity](const ActorState& actor) { return !actor.retired && actor.entity == entity; });
    return found == actors_.end() ? nullptr : &*found;
}
const ActorState* ActorRuntime::find(EntityHandle entity) const noexcept {
    const auto found = std::find_if(actors_.begin(), actors_.end(),
        [entity](const ActorState& actor) { return !actor.retired && actor.entity == entity; });
    return found == actors_.end() ? nullptr : &*found;
}
void ActorRuntime::emit(ActorEvent event) {
    if (events_) events_(event);
}
void ActorRuntime::add_waypoint(EntityHandle entity, ActorWaypoint waypoint) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered) throw std::runtime_error("waypoint target has no registered AI");
    waypoint.direction = normalized(waypoint.direction);
    actor->ai.waypoints.push_back(std::move(waypoint));
}
void ActorRuntime::clear_waypoints(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered) throw std::runtime_error("waypoint target has no registered AI");
    actor->ai.waypoints.clear();
    actor->ai.route.clear();
    actor->ai.navigation_state = 0;
    actor->ai.route_cursor = 0;
    actor->ai.dwell_time = time_;
}
void ActorRuntime::rewind_waypoints(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered) throw std::runtime_error("waypoint target has no registered AI");
    actor->ai.route_cursor = 0;
    actor->ai.dwell_time = time_;
}
void ActorRuntime::waypoint_index(EntityHandle entity, std::uint32_t index) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered) throw std::runtime_error("waypoint target has no registered AI");
    if (index >= actor->ai.waypoints.size())
        throw std::runtime_error("authored waypoint index is out of range");
    actor->ai.route.push_back(static_cast<int>(index));
}
void ActorRuntime::waypoint_clear_indices(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered) throw std::runtime_error("waypoint target has no registered AI");
    actor->ai.route.clear();
}
void ActorRuntime::waypoint_start(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered) throw std::runtime_error("waypoint target has no registered AI");
    if (!actor->ai.route.empty()) {
        const auto index = actor->ai.route.front();
        if (index < 0 || static_cast<std::size_t>(index) >= actor->ai.waypoints.size())
            throw std::runtime_error("authored waypoint route is out of range");
        actor->ai.navigation_state = 2;
        actor->movement_walk = !actor->ai.waypoints[index].run;
        ai_queue_target(*actor, actor->ai.waypoints[index].position);
    }
}
void ActorRuntime::waypoint_end(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered) throw std::runtime_error("waypoint target has no registered AI");
    actor->ai.navigation_state = 0; // Original waypoint_end, 0043ae50.
}
void ActorRuntime::no_weapon(EntityHandle entity, bool enabled) {
    auto* actor = find(entity);
    if (!actor || actor->kind != ActorKind::player)
        throw std::runtime_error("no-weapon target is not the player");
    actor->no_weapon = enabled;
    ActorEvent event;
    event.kind = ActorEvent::Kind::weapon_visibility;
    event.entity = entity;
    event.value = enabled ? 0 : 1;
    emit(std::move(event));
}
void ActorRuntime::camera_shake(EntityHandle entity, float duration, float yaw, float pitch) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("shake target is not an actor");
    actor->shake_time = time_;
    actor->shake_duration = duration;
    actor->shake_yaw = yaw;
    actor->shake_pitch = pitch;
    actor->shake_active = true;
}
void ActorRuntime::init_orientation(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("orientation target is not an actor");
    // 004263d0 installs B, then virtual+20 sets its already-equal raw T.
    // 00411740 therefore leaves both canonical snapshots and their tick intact.
    actor->orientation = original_actor_basis;
    if (combat_.frame_changed) combat_.frame_changed(entity, true);
}
void ActorRuntime::set_position(EntityHandle entity, Vec3 position) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("position target is not an actor");
    // Vtable+1c=00411830 and +20=00411ed0 both call 00411740(reset1).
    if (actor->orientation[12] == position.x &&
        actor->orientation[13] == position.y && actor->orientation[14] == position.z) return;
    actor->orientation[12] = position.x;
    actor->orientation[13] = position.y;
    actor->orientation[14] = position.z;
    if (combat_.frame_changed) combat_.frame_changed(entity, true);
    actor->position = actor->previous_position = position;
    actor->position_time_tick = static_cast<std::uint32_t>(time_);
}
void ActorRuntime::remove_ai(EntityHandle entity) {
    // 0043aca0 unregisters only AI; the physical actor/corpse remains.
    const auto sentinel = actors_.size();
    ai_iteration_ = sentinel;
    std::size_t predecessor = sentinel;
    for (std::size_t removed = 0; removed != sentinel; ++removed) {
        auto& actor = actors_[removed];
        if (!actor.ai.registered) continue;
        if (actor.entity != entity) { predecessor = removed; continue; }
        actor.ai.registered = false;
        ai_iteration_ = predecessor; // 0043a8e0's working iterator side effect.
        if (ai_cursor_ == removed) {
            ai_cursor_ = sentinel;
            for (std::size_t i = removed + 1; i < sentinel; ++i)
                if (actors_[i].ai.registered) { ai_cursor_ = i; break; }
            if (ai_cursor_ == sentinel)
                for (std::size_t i = 0; i < removed; ++i)
                    if (actors_[i].ai.registered) { ai_cursor_ = i; break; }
        }
        return;
    }
}
void ActorRuntime::remove(EntityHandle entity) {
    remove_ai(entity);
    if (auto* actor = find(entity)) {
        actor->retired = true; // Never erase a node borrowed by VM position aliases.
        actor->enabled = actor->alive = false;
        if (player_entity_ == entity) player_entity_ = 0;
    }
}

void ActorRuntime::capture_home_position(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered)
        throw std::runtime_error("AI home target has no registered AI");
    actor->ai.initial_position =
        Vec3{actor->orientation[12], actor->orientation[13], actor->orientation[14]}; // 0043ae20.
}
void ActorRuntime::capture_home_forward(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || !actor->ai.registered)
        throw std::runtime_error("AI home target has no registered AI");
    actor->ai.initial_forward =
        Vec3{-actor->orientation[8], -actor->orientation[9], -actor->orientation[10]};
}


// Original weapon registry 00439750 / descriptor constructor 004395a0.
// Kept in original offset order so unused authored values are not reinterpreted.
namespace {
std::uint32_t original_random(std::uint32_t& state) noexcept {
    state = state * 214013u + 2531011u; // MSVC rand, 00468568.
    return (state >> 16) & 32767u;
}
float original_elapsed_ms(double now, double then) noexcept {
    const auto current = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now));
    const auto previous = static_cast<std::uint32_t>(static_cast<std::uint64_t>(then));
    return static_cast<float>(current - previous);
}
long double original_random_between(std::uint32_t& state, float minimum, float maximum) noexcept {
    // Preserve 00443000's x87 return; callers storing float round at that store.
    const long double random = static_cast<long double>(original_random(state)) * 3.0518509447574615e-05f;
    return random * (static_cast<long double>(maximum) - minimum) + minimum;
}
std::int64_t original_ftol(long double value) noexcept {
    // 00467dbc corrects FIST64's rounding toward zero; invalid returns indefinite.
    if (!(value >= -9223372036854775808.0L && value < 9223372036854775808.0L))
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(value);
}
std::uint8_t original_health_alpha(float health) noexcept {
    // 00412760 keeps this expression in x87 precision; 00467dbc truncates.
    const long double alpha = (1.0L - static_cast<long double>(health)) * 255.0L;
    return static_cast<std::uint8_t>(original_ftol(alpha));
}
Matrix combat_axis_rotation(Vec3 axis, float degrees) {
    axis = normalized(axis);
    const float sine = std::sin(degrees * .01745329238474369f);
    const float cosine = std::cos(degrees * .01745329238474369f);
    const float t = 1 - cosine;
    Matrix out = identity_matrix();
    out[0] = axis.x * axis.x * t + cosine;
    out[1] = axis.x * axis.y * t + sine * axis.z;
    out[2] = axis.x * axis.z * t - sine * axis.y;
    out[4] = axis.x * axis.y * t - sine * axis.z;
    out[5] = axis.y * axis.y * t + cosine;
    out[6] = axis.y * axis.z * t + sine * axis.x;
    out[8] = axis.x * axis.z * t + sine * axis.y;
    out[9] = axis.y * axis.z * t - sine * axis.x;
    out[10] = axis.z * axis.z * t + cosine;
    return out;
}
Matrix original_actor_frame(const ActorState& actor, int mode) {
    Matrix frame = actor.orientation;
    if (!mode) return frame;
    // 00417ed0 initializes 004a33c0 with 00436b90(180), then 00436b40(90).
    static const Matrix correction = [] {
        const auto rotation = [](float degrees, bool z) {
            const long double radians = static_cast<long double>(degrees) * .01745329238474369f;
            const float cosine = static_cast<float>(std::cos(radians));
            const float sine = static_cast<float>(std::sin(static_cast<float>(radians)));
            Matrix value = identity_matrix();
            if (z) {
                value[0] = value[5] = cosine;
                value[1] = sine;
                value[4] = -sine;
            } else {
                value[5] = value[10] = cosine;
                value[6] = sine;
                value[9] = -sine;
            }
            return value;
        };
        return multiply(rotation(180, true), rotation(90, false));
    }();
    frame = multiply(frame, correction);
    Vec3 up{0, 0, 1};
    Vec3 forward{frame[4], frame[5], 0};
    Vec3 right = cross(forward, up);
    forward = cross(up, right);
    // 0041169c/004116b4/004116cc normalize the three derived axes via00444c90.
    right = original_normalized(right);
    forward = original_normalized(forward);
    up = original_normalized(up);
    frame[0] = right.x; frame[1] = right.y; frame[2] = right.z;
    frame[4] = forward.x; frame[5] = forward.y; frame[6] = forward.z;
    frame[8] = up.x; frame[9] = up.y; frame[10] = up.z;
    return frame;
}
Vec3 combat_position(const ActorState& actor) noexcept {
    // Original mode0 frame getters read RAW T, not canonical physical +22c.
    return {actor.orientation[12], actor.orientation[13], actor.orientation[14]};
}
Vec3 combat_origin(const ActorState& actor) {
    return combat_position(actor) + Vec3{0, 0, actor.fully_crouched ? 32.673500061035156f : 49.08399963378906f} +
        transform_vector(actor.orientation, {0, 0, -1}) * 20;
}
int combat_hit_direction(const ActorState& shooter, const ActorState& target) {
    const auto shooter_position = combat_position(shooter);
    const auto target_position = combat_position(target);
    const auto displacement = shooter_position - target_position;
    const auto relative = normalized(Vec3{displacement.x, displacement.y, 0});
    const float front = dot(transform_vector(target.orientation, {0, 0, -1}), relative);
    const float side = dot(transform_vector(target.orientation, {-1, 0, 0}), relative);
    if (front < -.5f) return 9;
    const float height = shooter_position.z - target_position.z;
    if (height > 120) return front <= -.5f || front >= .9f ? 0 : side >= 0 ? 4 : 5;
    if (height < -120) return front <= -.5f || front >= .9f ? 1 : side >= 0 ? 6 : 7;
    return front <= -.5f || front >= .9f ? 8 : side >= 0 ? 3 : 2;
}
struct WeaponDescriptor {
    const char* name;
    int weapon_class;
    const char* torso;
    const char* legs;
    int rounds_per_minute;
    float field_8c, reload_ms;
    int ammo_type, capacity;
    std::array<float, 3> field_9c_a4;
    float field_b8;
    std::array<float, 3> field_a8_b0;
    float field_bc;
    bool field_e0;
    std::array<float, 8> field_c0_dc;
    int pellets;
    float interval_ms() const noexcept {
        // 004395a0 divides game-clock frequency by RPM * float00477780.
        return rounds_per_minute ? 1000.0f / (rounds_per_minute * 0.01666666753590107f) : 0;
    }
};
constexpr std::array<WeaponDescriptor, 7> weapon_descriptors{{
    {"no_weapon",2,"no_weapon","no_weapon",0,0,0,0,0, {0,0,0},0,{0,0,0},0,false,{0,0,0,0,0,0,0,0},0},
    {"gun_pistol_cz75",0,"pistol","no_weapon",240,0,3000,2,10, {0,.05f,0},0,{1,.35f,.05f},.009765625f,false,{1,0,0,0,1,0,0,0},1},
    {"gun_pistol_desert",0,"pistol","no_weapon",180,0,2000,3,8, {0,.075f,0},0,{1,.6f,.3f},.009765625f,false,{2,0,.8f,.2f,2,0,.8f,.2f},1},
    {"gun_rifle_uzi",1,"pistol","no_weapon",750,0,2000,2,20, {0,.05f,0},0,{1,.2f,.1f},.009765625f,false,{1,0,.8f,.2f,1,0,.8f,.2f},1},
    {"gun_rifle_ak101",1,"rifle","rifle",760,0,2000,1,30, {0,.075f,0},0,{1,.35f,.1f},.0146484375f,false,{2,.6f,.8f,.2f,1,.6f,.8f,.2f},1},
    {"gun_rifle_m4",1,"rifle","rifle",900,0,2600,1,25, {0,.05f,0},0,{1,.15f,.1f},.0048828125f,true,{1,.6f,.8f,.2f,1,.6f,.8f,.2f},1},
    {"gun_shotgun_baba",1,"rifle","rifle",90,0,4052,4,7, {0,.075f,0},0,{.6f,.5f,.1f},.0390625f,false,{2,.6f,.8f,.2f,1,.6f,.2f,.2f},6}
}};
const WeaponDescriptor& weapon_descriptor(WeaponKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= weapon_descriptors.size()) throw std::runtime_error("invalid weapon ID");
    return weapon_descriptors[index];
}
// Original producers use stack path buffers, not allocating strings per frame.
struct ActorAnimationPath {
    std::array<char, 256> bytes{};
    std::size_t size{};
    void append(std::string_view text) {
        if (text.size() > bytes.size() - size)
            throw std::runtime_error("actor animation path exceeds original buffer");
        std::copy(text.begin(), text.end(), bytes.begin() + size);
        size += text.size();
    }
    ActorAnimationPath(std::string_view group, std::string_view clip) {
        append(group);
        if (!group.empty()) append("/");
        append(clip);
    }
    operator std::string_view() const noexcept { return {bytes.data(), size}; }
};
bool controller_matches(const ActorAnimationController& state, std::string_view clip,
                        std::uint32_t mode) noexcept {
    if (state.clip.empty() || (state.status & 3) == 3 || state.mode != mode) return false;
    auto name = state.clip;
    if (name.size() >= 3 && inventory_equal(name.substr(name.size() - 3), ".pa"))
        name.remove_suffix(3);
    if (name.size() < clip.size() || !inventory_equal(name.substr(name.size() - clip.size()), clip))
        return false;
    return name.size() == clip.size() || name[name.size() - clip.size() - 1] == '/' ||
           name[name.size() - clip.size() - 1] == '\\';
}
struct CombatDeathPaths {
    std::string_view leaf, leg, torso;
};
constexpr std::array<CombatDeathPaths, 12> combat_death_paths{{
    {"dying_oy_01", "common/die/leg/dying_oy_01", "common/die/torso/dying_oy_01"},
    {"dying_oy_02", "common/die/leg/dying_oy_02", "common/die/torso/dying_oy_02"},
    {"dying_oy_04", "common/die/leg/dying_oy_04", "common/die/torso/dying_oy_04"},
    {"dying_oy_05", "common/die/leg/dying_oy_05", "common/die/torso/dying_oy_05"},
    {"dying_oh_01", "common/die/leg/dying_oh_01", "common/die/torso/dying_oh_01"},
    {"dying_oh_02", "common/die/leg/dying_oh_02", "common/die/torso/dying_oh_02"},
    {"dying_oum_01", "common/die/leg/dying_oum_01", "common/die/torso/dying_oum_01"},
    {"dying_odd_01", "common/die/leg/dying_odd_01", "common/die/torso/dying_odd_01"},
    {"dying_ay_01", "common/die/leg/dying_ay_01", "common/die/torso/dying_ay_01"},
    {"dying_ay_02", "common/die/leg/dying_ay_02", "common/die/torso/dying_ay_02"},
    {"dying_ah_01", "common/die/leg/dying_ah_01", "common/die/torso/dying_ah_01"},
    {"dying_ah_02", "common/die/leg/dying_ah_02", "common/die/torso/dying_ah_02"}
}};
const CombatDeathPaths& combat_death_path(std::string_view leaf) {
    for (const auto& paths : combat_death_paths)
        if (paths.leaf == leaf) return paths;
    throw std::runtime_error("unknown native dying animation");
}
WeaponKind active_weapon(const ActorState& actor) {
    return actor.no_weapon && actor.kind == ActorKind::player ? WeaponKind::none :
        actor.weapon_slots[actor.selected_slot];
}
WeaponState& selected_weapon(ActorState& actor) {
    return actor.weapons[static_cast<std::size_t>(active_weapon(actor))];
}
void add_reserve(ActorState& actor, int type, int count) {
    if (!actor.ammunition[type])
        actor.ammunition_order.insert(actor.ammunition_order.begin(), static_cast<std::uint8_t>(type));
    actor.ammunition[type] += count;
}
std::size_t weapon_slot(WeaponKind kind) {
    return weapon_descriptor(kind).weapon_class == 1 ? 1 : 0;
}
int magazine_transfer(ActorState& actor, WeaponKind kind, int maximum, bool transfer) {
    const auto& descriptor = weapon_descriptor(kind);
    if (kind == WeaponKind::none || maximum <= 0) return 0;
    auto& magazine = actor.weapons[static_cast<std::size_t>(kind)].magazine;
    auto& reserve = actor.ammunition[descriptor.ammo_type];
    const int amount = std::min({descriptor.capacity - magazine, reserve, maximum});
    if (amount <= 0) return 0;
    if (transfer) {
        magazine += amount;
        reserve -= amount;
        if (!reserve) {
            const auto found = std::find(actor.ammunition_order.begin(), actor.ammunition_order.end(),
                                         static_cast<std::uint8_t>(descriptor.ammo_type));
            if (found != actor.ammunition_order.end()) actor.ammunition_order.erase(found);
        }
    }
    return amount;
}
void combat_state(ActorState& actor, int state, double time) {
    actor.previous_combat_state = actor.combat_state;
    actor.combat_state = state;
    actor.combat_time = time;
}
}

WeaponKind ActorRuntime::weapon_kind(std::string_view name) {
    for (std::size_t i = 0; i < weapon_descriptors.size(); ++i) {
        const std::string_view registered = weapon_descriptors[i].name;
        if (name == registered || (i && name.size() == registered.size() + 10 &&
            name.starts_with(registered) && name.substr(registered.size()) == "_dropped_2"))
            return static_cast<WeaponKind>(i);
    }
    return WeaponKind::none;
}
void ActorRuntime::grant_weapon(EntityHandle entity, std::string_view name, int ammunition) {
    grant_weapon(entity, weapon_kind(name), ammunition);
}
void ActorRuntime::grant_weapon(EntityHandle entity, WeaponKind kind, int ammunition) {
    if (kind == WeaponKind::none) return;
    const auto& descriptor = weapon_descriptor(kind);
    std::array<int, 5> payload{static_cast<int>(kind), descriptor.ammo_type, ammunition,
                               descriptor.ammo_type, 0};
    take_weapon(entity, payload);
}
bool ActorRuntime::take_weapon(EntityHandle entity, std::array<int, 5>& payload) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("weapon target is not an actor");
    const auto kind = static_cast<WeaponKind>(payload[0]);
    const auto& descriptor = weapon_descriptor(kind);
    if (kind == WeaponKind::none) return false;
    const auto slot = weapon_slot(kind);
    const bool granted = actor->weapon_slots[slot] == WeaponKind::none;
    const bool unarmed = active_weapon(*actor) == WeaponKind::none;
    const bool attachment_present = combat_.has_attachment && combat_.has_attachment(entity, descriptor.name);
    if (granted) {
        actor->weapon_slots[slot] = kind;
        auto& state = actor->weapons[static_cast<std::size_t>(kind)];
        state.kind = kind;
        state.owned = true;
        payload[0] = 0;
        if (unarmed && attachment_present) {
            actor->selected_slot = static_cast<std::uint8_t>(slot);
            actor->weapon = kind;
            ActorEvent event;
            event.kind = ActorEvent::Kind::weapon_visibility;
            event.entity = entity;
            event.weapon = kind;
            event.value = 1;
            emit(std::move(event));
        }
        actor->weapon = actor->weapon_slots[actor->selected_slot];
    }
    bool consumed = false;
    for (const auto equipped : actor->weapon_slots) {
        if (weapon_descriptor(equipped).ammo_type == descriptor.ammo_type) {
            const int count = payload[2] + payload[4];
            if (count) { add_reserve(*actor, descriptor.ammo_type, count); consumed = true; }
            break;
        }
    }
    if (granted && ((unarmed && attachment_present) || active_weapon(*actor) != kind))
        magazine_transfer(*actor, kind, payload[2], true);
    if (selected_weapon(*actor).magazine == 0 &&
        weapon_descriptor(active_weapon(*actor)).ammo_type == descriptor.ammo_type) reload(*actor, false);
    if (consumed) {
        payload[1] = payload[2] = payload[3] = payload[4] = 0;
        if (!granted) {
            ActorEvent event;
            event.kind = ActorEvent::Kind::sound;
            event.entity = entity;
            event.name = "sound/gun/ammo_taken";
            event.environment_sound = true;
            event.position = combat_position(*actor);
            emit(std::move(event));
        }
    }
    return granted;
}
bool ActorRuntime::take_ammunition(EntityHandle entity, int type, int count) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("ammo target is not an actor");
    if (type <= 0 || type >= static_cast<int>(actor->ammunition.size()) || count <= 0) return false;
    for (const auto equipped : actor->weapon_slots) {
        if (equipped != WeaponKind::none && weapon_descriptor(equipped).ammo_type == type) {
            add_reserve(*actor, type, count);
            return true;
        }
    }
    return false;
}
bool ActorRuntime::inventory_contains(EntityHandle entity, std::string_view item) const {
    const auto* actor = find(entity);
    if (!actor) throw std::runtime_error("inventory target is not an actor");
    return std::find_if(actor->inventory.begin(), actor->inventory.end(),
        [item](const std::string& stored) { return inventory_equal(stored, item); }) != actor->inventory.end();
}
void ActorRuntime::add_inventory(EntityHandle entity, std::string_view item) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("inventory target is not an actor");
    // 00414760 prepends; duplicates are deliberately retained.
    actor->inventory.insert(actor->inventory.begin(), std::string(item));
}
void ActorRuntime::remove_inventory(EntityHandle entity, std::string_view item) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("inventory target is not an actor");
    const auto found = std::find_if(actor->inventory.begin(), actor->inventory.end(),
        [item](const std::string& stored) { return inventory_equal(stored, item); });
    if (found != actor->inventory.end()) actor->inventory.erase(found);
}
void ActorRuntime::select(ActorState& actor, WeaponKind kind) {
    if (kind == WeaponKind::none || kind == actor.weapon || actor.combat_state == 4 || actor.combat_state == 5) return;
    if (actor.weapon_slots[weapon_slot(kind)] != kind) return;
    actor.selected_slot = static_cast<std::uint8_t>(weapon_slot(kind));
    actor.weapon = kind;
    combat_state(actor, 1, time_);
    ActorEvent event;
    event.kind = ActorEvent::Kind::weapon_visibility;
    event.entity = actor.entity;
    event.weapon = kind;
    event.value = actor.no_weapon ? 0 : 1;
    emit(std::move(event));
}
void ActorRuntime::reload(EntityHandle entity, bool fallback) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("reload target is not an actor");
    reload(*actor, fallback);
}
void ActorRuntime::reload(ActorState& actor, bool fallback) {
    if (actor.reload_blocked || (actor.movement_reload_blocked &&
        original_elapsed_ms(time_, actor.movement_action_time) > 500)) return;
    if (actor.combat_state != 0 && !(actor.combat_state == 3 && actor.shoot_flag == 0)) return;
    const auto kind = active_weapon(actor);
    if (magazine_transfer(actor, kind, 1000, false)) {
        if (actor.kind == ActorKind::player && actor.player_camera.special) {
            actor.player_camera.special = 3;
            ActorEvent zoom;
            zoom.kind = ActorEvent::Kind::sound;
            zoom.name = "sound/gun/zoom";
            zoom.environment_sound = true;
            emit(std::move(zoom));
        }
        combat_state(actor, 2, time_);
        selected_weapon(actor).reload_remaining = weapon_descriptor(kind).reload_ms * .001f;
        ActorEvent sound;
        sound.kind = ActorEvent::Kind::sound;
        sound.entity = actor.entity;
        sound.weapon = kind;
        sound.position = combat_position(actor);
        sound.name = std::string("sound/gun/") + weapon_descriptor(kind).name + "/reload";
        sound.environment_sound = true;
        emit(std::move(sound));
        if (kind != WeaponKind::baba && combat_.has_attachment &&
            combat_.has_attachment(actor.entity, weapon_descriptor(kind).name)) {
            ActorEvent particle;
            particle.kind = ActorEvent::Kind::particle;
            particle.entity = actor.entity;
            particle.weapon = kind;
            const auto attachment = combat_.attachment(actor.entity, weapon_descriptor(kind).name);
            particle.position = transform_point(attachment, {});
            particle.direction = transform_vector(attachment, {0, -1, 0});
            particle.basis_right = transform_vector(attachment, {0, 0, -1});
            particle.basis_up = transform_vector(attachment, {-1, 0, 0});
            particle.name = kind == WeaponKind::ak101 || kind == WeaponKind::m4 ?
                "gun_rifle_magazine" : "gun_pistol_magazine";
            emit(std::move(particle));
        }
        return;
    }
    if (!fallback) return;
    const auto other = actor.weapon_slots[1 - actor.selected_slot];
    if (other != WeaponKind::none && (actor.weapons[static_cast<std::size_t>(other)].magazine ||
        actor.ammunition[weapon_descriptor(other).ammo_type])) { select(actor, other); return; }
    if (!actor.no_ammo_latch) {
        ActorEvent event;
        event.kind = ActorEvent::Kind::sound;
        event.entity = actor.entity;
        event.weapon = actor.weapon;
        event.position = combat_position(actor);
        event.name = std::string("sound/gun/") + weapon_descriptor(actor.weapon).name + "/no_ammo";
        event.environment_sound = true;
        emit(std::move(event));
        actor.no_ammo_latch = true;
    }
}
void ActorRuntime::set_health(EntityHandle entity, float health) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("health target is not an actor");
    actor->health = std::min(health, 1.0f); // 00412760 has no lower clamp.
    ActorEvent event;
    event.kind = ActorEvent::Kind::damage_visual;
    event.entity = entity;
    event.value = original_health_alpha(actor->health);
    emit(std::move(event));
}

void ActorRuntime::drop_weapon(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("drop target is not an actor");
    if (actor->combat_state == 4 || actor->combat_state == 5) return;
    const auto kind = active_weapon(*actor);
    if (kind == WeaponKind::none || !combat_.has_attachment ||
        !combat_.has_attachment(entity, weapon_descriptor(kind).name)) return;
    const auto& descriptor = weapon_descriptor(kind);
    ActorEvent event;
    event.kind = ActorEvent::Kind::drop_weapon;
    event.entity = entity;
    event.weapon = kind;
    event.name = std::string(descriptor.name) + "_gun";
    // 00412390 uses actor frame0 position and frame1 axes, not the gun's pose.
    event.position = combat_position(*actor);
    const auto frame = original_actor_frame(*actor, 1);
    event.direction = transform_vector(frame, {0, -1, 0});
    event.basis_right = transform_vector(frame, {0, 0, -1});
    event.basis_up = transform_vector(frame, {-1, 0, 0});
    event.pickup = {static_cast<int>(kind), descriptor.ammo_type, selected_weapon(*actor).magazine,
                     descriptor.ammo_type, 0};
    emit(std::move(event));
    selected_weapon(*actor) = {};
    actor->weapon_slots[actor->selected_slot] = WeaponKind::none;
    actor->weapon = WeaponKind::none;
    ActorEvent visibility;
    visibility.kind = ActorEvent::Kind::weapon_visibility;
    visibility.entity = entity;
    visibility.value = 0;
    emit(std::move(visibility));
}

void ActorRuntime::hurt(EntityHandle entity, float damage, EntityHandle source) {
    auto* actor = find(entity);
    if (!actor) return; // Game dispatches prop damage separately.
    actor->health -= damage; // 0041a8c0; no invented armor reduction or pain state.
    if (!(actor->health <= 0)) return; // COMISS's unordered branch is nonlethal.
    actor->health = 0;
    actor->alive = false;
    actor->attacking = false;
    if (actor->kind == ActorKind::player) {
        actor->player_camera.death_roll_time = 0; // 0041faf0 before base164d0.
        ActorEvent player_death;
        player_death.kind = ActorEvent::Kind::player_death;
        player_death.entity = entity;
        player_death.phase = 0; // Synchronous HUD/shared camera gate before death script.
        emit(std::move(player_death));
    }
    set_posture(*actor, 2); // 004164d0 stops the mover before _on_dead.
    ActorEvent script;
    script.kind = ActorEvent::Kind::script;
    script.entity = entity;
    script.other = source;
    script.name = actor->name + "_on_dead";
    emit(std::move(script));
    ActorEvent material;
    material.kind = ActorEvent::Kind::damage_visual;
    material.entity = entity;
    material.phase = 1; // 004164d0 changes alpha only when root pass count is3.
    material.value = 255;
    emit(std::move(material));

    const auto kind = active_weapon(*actor);
    const auto& descriptor = weapon_descriptor(kind);
    unsigned count = static_cast<unsigned>(actor->drop_ammunition) >> 1;
    const auto first = static_cast<unsigned>(original_ftol(original_random_between(
        random_state_, 1, static_cast<float>(descriptor.capacity))));
    if (first <= count) count = static_cast<unsigned>(original_ftol(original_random_between(
        random_state_, 1, static_cast<float>(descriptor.capacity))));
    ActorEvent drop;
    drop.kind = ActorEvent::Kind::drop_weapon;
    drop.entity = entity;
    drop.weapon = kind;
    drop.name = std::string(descriptor.name) + "_dropped";
    drop.position = combat_position(*actor);
    drop.position.z -= 32.7226676940918f; // float00475bd0.
    drop.pickup = {static_cast<int>(kind), descriptor.ammo_type, static_cast<int>(count),
                    descriptor.ammo_type, actor->drop_ammunition - static_cast<int>(count)};
    emit(std::move(drop));
    ActorEvent visibility;
    visibility.kind = ActorEvent::Kind::weapon_visibility;
    visibility.entity = entity;
    visibility.value = 0;
    emit(std::move(visibility));
    if (actor->blood_enabled) {
        if (combat_.has_attachment && combat_.has_attachment(entity, "col_govde")) {
            ActorEvent spray;
            spray.kind = ActorEvent::Kind::particle;
            spray.entity = entity;
            spray.position = transform_point(combat_.attachment(entity, "col_govde"), {});
            spray.direction = {0, 0, -1};
            spray.basis_right = {1, 0, 0};
            spray.basis_up = {0, 1, 0};
            spray.name = "grp_spray_blood_02";
            emit(std::move(spray));
        }
        ActorEvent blood;
        blood.kind = ActorEvent::Kind::particle;
        blood.entity = entity;
        blood.position = combat_position(*actor);
        blood.direction = actor->hit_direction;
        blood.basis_right = cross(actor->hit_direction, {0, 0, 1});
        blood.basis_up = cross(blood.basis_right, actor->hit_direction);
        blood.name = "grp_spray_blood_01";
        emit(std::move(blood));
    }
    if (actor->drop_health) {
        ActorEvent drop;
        drop.kind = ActorEvent::Kind::drop_health;
        drop.entity = entity;
        drop.position = combat_position(*actor);
        drop.name = "obj_firstaid_kit_small";
        drop.value = original_random_between(random_state_, .3f, .6f);
        emit(std::move(drop));
    }
    combat_state(*actor, 4, time_);
    // 004164d0 keeps two five-animation banks, selected by collision hit region.
    constexpr std::array<const char*, 10> other_deaths{
        "dying_oy_01","dying_oy_02","dying_oy_02","dying_oy_04","dying_oy_05",
        "dying_oh_01","dying_oh_02","dying_oum_01","dying_odd_01","dying_oh_01"};
    constexpr std::array<const char*, 10> region9_deaths{
        "dying_ay_01","dying_ay_01","dying_ay_01","dying_ay_02","dying_ay_02",
        "dying_ah_01","dying_ah_01","dying_ah_01","dying_ah_02","dying_ah_02"};
    if (!combat_.trace_motion)
        throw std::runtime_error("death animation requires Scene motion queries");
    Vec3 direction = transform_vector(original_actor_frame(*actor, 1), {0, -1, 0});
    if (actor->hit_region != 9) direction = direction * -1;
    const auto body = original_actor_frame(*actor, 0);
    const Vec3 start{body[12], body[13], body[14]};
    // 004164d0 calls the owner-hull 00424790 wrapper before either rand draw.
    const auto hit = combat_.trace_motion(start, direction, 100, actor->hull,
                                          0, 0x200, 7, true, entity, true);
    const auto random = original_random(random_state_);
    const bool backward = !hit.hit &&
        (actor->hit_weapon == WeaponKind::baba || actor->hit_weapon == WeaponKind::magnum ||
         (original_random(random_state_) & 1));
    actor->animation = (actor->hit_region == 9 ? region9_deaths : other_deaths)[random % 5 + (backward ? 5 : 0)];
    actor->death_surface_sound = false;
    animation_control(*actor, ActorEvent::AnimationOperation::reset_all);
    const auto& dying_paths = combat_death_path(actor->animation);
    for (const int channel : {0, 2}) {
        ActorEvent animation;
        animation.kind = ActorEvent::Kind::animation;
        animation.entity = entity;
        animation.animation_clip = channel == 0 ? dying_paths.leg : dying_paths.torso;
        animation.animation_channel = channel;
        animation.animation_mode = 1;
        animation.animation_immediate = true;
        animation.restart_animation = true;
        emit(std::move(animation));
    }
    if (combat_.corpse) combat_.corpse(entity); // Concrete model effects before dying sound.
    if (actor->kind == ActorKind::bot) {
        ActorEvent sound;
        sound.kind = ActorEvent::Kind::sound;
        sound.entity = entity;
        sound.position = combat_position(*actor);
        sound.name = "sound/bot/dying";
        sound.environment_sound = true;
        emit(std::move(sound));
    }
    remove_ai(entity); // 004164d0 after bot dying sound, including player/not-found.
    if (actor->kind == ActorKind::player) {
        actor->player_camera.death_orientation = actor->orientation;
        actor->player_camera.death_captured = true;
        for (auto& registered : actors_)
            if (registered.ai.registered) registered.ai.enabled = false;
        ai_iteration_ = actors_.size(); // 0041faf0's registry traversal ends at sentinel.
        ActorEvent sound;
        sound.kind = ActorEvent::Kind::sound;
        sound.entity = entity;
        sound.name = "sound/effect/menu/hearthbeat_flat";
        emit(std::move(sound));
    }
}
void ActorRuntime::animation_control(ActorState& actor, ActorEvent::AnimationOperation operation,
                                     int channel) {
    ActorEvent event;
    event.kind = ActorEvent::Kind::animation;
    event.entity = actor.entity;
    event.animation_operation = operation;
    event.animation_channel = channel;
    emit(std::move(event));
}

void ActorRuntime::animation_pair(ActorState& actor, int channel, std::string_view group,
                                  std::string_view clip, std::uint32_t mode, bool immediate,
                                  std::uint32_t previous_mode, bool reverse_match) {
    const ActorAnimationPath path(group, clip);
    ActorEvent event;
    event.kind = ActorEvent::Kind::animation;
    event.entity = actor.entity;
    event.animation_clip = path;
    event.animation_channel = channel;
    event.animation_mode = mode;
    event.animation_immediate = immediate;
    event.animation_previous_mode = previous_mode;
    event.animation_reverse_match = reverse_match;
    emit(std::move(event));
}

void ActorRuntime::torso_animation(ActorState& actor, std::uint32_t tick,
                                   std::string_view motion, std::string_view variant,
                                   std::string_view preferred, std::string_view fallback,
                                   bool immediate) {
    const auto group_for = [&](std::string_view clip) {
        return combat_.animation_exists(actor.entity, ActorAnimationPath(preferred, clip)) ?
            preferred : fallback; // 00411a40, authored descriptor fallback only.
    };
    if (actor.combat_state != 3) {
        animation_control(actor, ActorEvent::AnimationOperation::reset_aim_unless_master);
        const auto clip = actor.combat_state == 1 ? std::string_view("arm") :
                          actor.combat_state == 2 ? std::string_view("reload") : motion;
        animation_pair(actor, 2, actor.combat_state == 0 ? group_for(clip) : preferred, clip,
                       actor.combat_state == 0 ? 2 : 1,
                       actor.combat_state == 0 && immediate);
        return;
    }
    animation_pair(actor, 2, "common", "motionless", 4, true);
    const auto first_master = combat_.animation_controller(actor.entity, 2);
    const int master_slot = controller_matches(first_master, "common/motionless", 4) ? 2 : 3;
    const auto master = combat_.animation_controller(actor.entity, master_slot);
    if (actor.shoot_flag == 2) {
        actor.shoot_flag = 1;
        animation_control(actor, ActorEvent::AnimationOperation::reset_aim);
    }
    // 00413830 ->00436370 RAW forward.z, independent of horizontal yaw.
    const float pitch = std::clamp((1 - actor.orientation[10]) * .5f, 0.0f, 1.0f);
    const bool lower = pitch < .5f;
    const bool shooting = actor.shoot_flag == 1;
    constexpr std::array<std::string_view, 3> aim{"aim_down", "aim_forward", "aim_up"};
    constexpr std::array<std::string_view, 3> shoot{"shoot_down", "shoot_forward", "shoot_up"};
    const auto& clips = shooting ? shoot : aim;
    for (int index = 0; index != 2; ++index) {
        ActorAnimationPath leaf({}, variant);
        if (!variant.empty()) leaf.append("_");
        leaf.append(clips[(lower ? 0 : 1) + index]);
        const ActorAnimationPath path(group_for(leaf), leaf);
        ActorEvent event;
        event.kind = ActorEvent::Kind::animation;
        event.entity = actor.entity;
        event.animation_operation = ActorEvent::AnimationOperation::direct;
        event.animation_clip = path;
        event.animation_channel = 4 + index;
        event.animation_mode = 1;
        event.animation_master_channel = master_slot;
        event.animation_master_clip = master.clip;
        event.animation_blend = index == 0 ? (lower ? 1 - 2 * pitch : 2 - 2 * pitch) :
                                            (lower ? 2 * pitch : 2 * pitch - 1);
        emit(std::move(event));
    }
    if (shooting) {
        // Original completion query00414123 uses SECOND actual aim slot5.
        const auto second = combat_.animation_controller(actor.entity, 5);
        if (!second.clip.empty() &&
            tick - second.start_tick >= std::max(second.duration_ms, 1u) - 5u)
            actor.shoot_flag = 0;
    }
}

void ActorRuntime::animation_footstep(ActorState& actor, std::string_view clip) {
    const auto first = combat_.animation_controller(actor.entity, 0);
    const auto state = controller_matches(first, clip, 2) ? first :
                       combat_.animation_controller(actor.entity, 1);
    if (state.clip.empty()) return;
    const float phase = static_cast<float>(state.sample_tick) / std::max(state.duration_ms, 1u);
    if (phase > .10000000149011612f && phase < .20000000298023224f && !actor.footstep_first) {
        actor.footstep_first = true;
        actor.footstep_second = false;
    } else if (phase > .6000000238418579f && phase < .699999988079071f && !actor.footstep_second) {
        actor.footstep_first = false;
        actor.footstep_second = true;
    } else return;
    const auto hit = combat_.trace_motion(combat_position(actor), {0, 0, -1}, 25, actor.hull,
                                          0, 0x200, 0x600, true, actor.entity, true);
    if (hit.hit && combat_.animation_surface_sound)
        combat_.animation_surface_sound(actor.entity, false, actor.kind == ActorKind::player,
                                        combat_position(actor), hit, &actor.footstep_variant);
}

void ActorRuntime::dispatch_animation(ActorState& actor, std::uint32_t tick) {
    // 00435ed0 ->00414fa0, including stationary and disabled registry actors.
    if (actor.combat_state == 7) return;
    if (!combat_.animation_controller || !combat_.animation_exists)
        throw std::runtime_error("actor animation dispatcher requires live Game controllers");
    const auto& descriptor = weapon_descriptor(active_weapon(actor));
    const std::string_view preferred = descriptor.torso, fallback = descriptor.legs;
    const auto live = [](const ActorAnimationController& state) {
        return !state.clip.empty() && (state.status & 3) != 3;
    };
    const auto complete = [&](const ActorAnimationController& state) {
        return live(state) && tick - state.start_tick >= std::max(state.duration_ms, 1u) - 5u;
    };
    if (actor.combat_state == 6) {
        const auto state = combat_.animation_controller(actor.entity, 0);
        if (state.mode != 2 && complete(state)) {
            animation_control(actor, ActorEvent::AnimationOperation::pause_controller, 0);
            combat_state(actor, actor.previous_combat_state, tick);
        }
        return;
    }
    if (actor.combat_state == 1) {
        for (const int slot : {2, 3}) {
            const auto state = combat_.animation_controller(actor.entity, slot);
            if ((controller_matches(state, ActorAnimationPath(preferred, "arm"), state.mode) ||
                 controller_matches(state, ActorAnimationPath(fallback, "arm"), state.mode)) &&
                complete(state)) {
                combat_state(actor, 0, tick);
                break;
            }
        }
    } else if (actor.combat_state == 2) {
        const auto elapsed = static_cast<std::uint32_t>(tick - static_cast<std::uint32_t>(actor.combat_time));
        if (descriptor.reload_ms < static_cast<float>(elapsed)) {
            magazine_transfer(actor, active_weapon(actor), 1000, true);
            const bool aiming = actor.previous_combat_state == 3;
            combat_state(actor, aiming ? 3 : 0, tick);
            if (aiming) actor.shoot_flag = 0;
        }
    } else if (actor.combat_state == 3 && actor.shoot_flag == 0 &&
               tick - static_cast<std::uint32_t>(actor.combat_time) > 2000) {
        combat_state(actor, 0, tick);
    }
    if (actor.combat_state == 4) {
        const auto state = combat_.animation_controller(actor.entity, 0);
        if (live(state) && tick - state.start_tick >= std::max(state.duration_ms, 1u) - 2000u &&
            !actor.death_surface_sound) {
            actor.death_surface_sound = true;
            const auto hit = combat_.trace_motion(combat_position(actor), {0, 0, -1}, 25, actor.hull,
                                                  0, 0x200, 0x600, true, actor.entity, true);
            if (hit.hit && actor.kind == ActorKind::player && combat_.animation_surface_sound)
                combat_.animation_surface_sound(actor.entity, true, true, combat_position(actor), hit, nullptr);
        }
        if (complete(state)) {
            animation_control(actor, ActorEvent::AnimationOperation::pause_controller, 0);
            combat_state(actor, 5, tick);
        }
        return;
    }
    if (actor.combat_state == 5) return;
    const bool bypass = actor.kind == ActorKind::player && combat_.camera_placement_bypass &&
                        combat_.camera_placement_bypass();
    const auto reset_steps = [&] { actor.footstep_first = actor.footstep_second = false; };
    if (!bypass && actor.jumping) {
        reset_steps();
        animation_control(actor, ActorEvent::AnimationOperation::reset_aim_unless_master);
        animation_pair(actor, 0, "leg", "jump", 1, true);
        animation_pair(actor, 2, "no_weapon", "jump", 1, true);
        return;
    }
    if (actor.posture_state == 1 || actor.posture_state == 2) {
        reset_steps();
        animation_control(actor, ActorEvent::AnimationOperation::reset_aim_unless_master);
        const auto group = combat_.animation_exists(actor.entity, ActorAnimationPath(preferred, "crouching")) ?
                           preferred : fallback;
        const bool enter = actor.posture_state == 1;
        animation_pair(actor, 0, "leg", "crouching", enter ? 1 : 3, false, enter ? 3 : 1, true);
        animation_pair(actor, 2, group, "crouching", enter ? 1 : 3, false, enter ? 3 : 1, true);
        return;
    }
    if (actor.posture_state == 3) {
        reset_steps();
        if (!actor.input_gas) {
            animation_pair(actor, 0, "leg", actor.combat_state == 3 ? "crouch_still" : "crouch_idle", 2);
            torso_animation(actor, tick, "crouch_idle", "crouch", preferred, fallback, false);
        } else {
            animation_pair(actor, 0, "leg", "crouch_walk", 2);
            torso_animation(actor, tick, "crouch_walk", {}, preferred, fallback, false);
        }
        return;
    }
    if (!bypass && actor.airborne &&
        (actor.crouching || tick - actor.fall_time_tick > 500)) {
        reset_steps();
        animation_control(actor, ActorEvent::AnimationOperation::reset_aim_unless_master);
        if (actor.crouch_phase == 2 || actor.crouch_phase == 4) {
            const bool end = actor.crouch_phase == 4;
            const std::string_view clip = end ? "jump_fall_end" : "jump_fall";
            animation_pair(actor, 0, "leg", clip, 1, end);
            animation_pair(actor, 2, "no_weapon", clip, 1, end);
        }
        return;
    }
    if (actor.movement_speed != 0 || actor.input_gas) {
        std::string_view clip, variant;
        bool immediate = false;
        const bool running = !actor.movement_walk && !actor.movement_command_flags[3]; //0041e1b0.
        if (!running) {
            const Vec3 forward{-actor.orientation[8], -actor.orientation[9], -actor.orientation[10]};
            if (dot(forward, actor.position - actor.previous_position) < 0) {
                clip = "walk_backward";
                immediate = true;
            } else clip = "walk";
        } else if (actor.movement_command_flags[1]) {
            clip = variant = actor.movement_command_flags[2] ? "strafe_right45" : "strafe_right";
            immediate = true;
        } else if (!actor.movement_command_flags[0]) clip = "run";
        else {
            clip = variant = actor.movement_command_flags[2] ? "strafe_left45" : "strafe_left";
            immediate = true;
        }
        animation_pair(actor, 0, "leg", clip, 2, immediate);
        // Original14240 compares the literal fatih/leg name before selecting member0/1.
        animation_footstep(actor, ActorAnimationPath("fatih/leg", clip));
        torso_animation(actor, tick, clip, variant, preferred, fallback, immediate);
        return;
    }
    reset_steps();
    bool immediate = false;
    constexpr std::array<std::string_view, 4> strafes{
        "leg/strafe_left", "leg/strafe_right", "leg/strafe_left45", "leg/strafe_right45"};
    for (const int slot : {0, 1})
        for (const auto clip : strafes)
            if (controller_matches(combat_.animation_controller(actor.entity, slot), clip, 2)) immediate = true;
    animation_pair(actor, 0, "leg", actor.combat_state == 3 ? "still" : "idle", 2, immediate);
    torso_animation(actor, tick, "idle", {}, preferred, fallback, immediate);
}


void ActorRuntime::update_combat(ActorState& actor) {
    const auto kind = active_weapon(actor);
    auto& weapon = selected_weapon(actor);
    const auto elapsed = original_elapsed_ms(time_, actor.combat_time);
    const auto shot_elapsed = original_elapsed_ms(time_, actor.shot_time);
    weapon.cooldown = std::max(0.0f, weapon_descriptor(kind).interval_ms() - shot_elapsed) * .001f;
    weapon.reload_remaining = actor.combat_state == 2 ?
        std::max(0.0f, weapon_descriptor(kind).reload_ms - static_cast<float>(elapsed)) * .001f : 0;
    if (actor.weapon_phase) {
        const auto age = original_elapsed_ms(time_, actor.shot_visual_time);
        const int phase = age > 1000.0f * .06666667014360428f ? 0 :
                          age > 1000.0f * .02083333395421505f ? 1 : 2;
        if (phase != actor.weapon_phase) {
            actor.weapon_phase = phase;
            ActorEvent event;
            event.kind = ActorEvent::Kind::weapon_phase;
            event.entity = actor.entity;
            event.weapon = kind;
            event.phase = phase;
            emit(std::move(event));
            if (!phase) {
                ActorEvent shadow;
                shadow.kind = ActorEvent::Kind::shot;
                shadow.entity = actor.entity;
                shadow.name = "golge";
                shadow.value = 60;
                emit(std::move(shadow));
            }
        }
    }
    if (actor.kind == ActorKind::bot && shot_elapsed > 1000.0f * .05000000074505806f) {
        actor.shot_light = false;
        ActorEvent light;
        light.kind = ActorEvent::Kind::weapon_light;
        light.entity = actor.entity;
        light.value = 5;
        light.radius = 255;
        emit(std::move(light));
    }
    if (!selected_weapon(actor).magazine && shot_elapsed > weapon_descriptor(kind).interval_ms())
        reload(actor, false);
}

bool ActorRuntime::can_fire(const ActorState& actor, float factor) const {
    // 00411210 only tests readiness; it neither emits nor consumes a round.
    if (actor.combat_state != 0 && actor.combat_state != 3) return false;
    if (actor.movement_reload_blocked && original_elapsed_ms(time_, actor.movement_action_time) > 500) return false;
    if (actor.posture_state == 1 || actor.posture_state == 2) return false;
    return original_elapsed_ms(time_, actor.shot_time) > weapon_descriptor(active_weapon(actor)).interval_ms() * factor;
}

bool ActorRuntime::fire(ActorState& actor, CollisionWorld*) {
    // Actual virtual+48, 00416d50. AI readiness00411210 is a separate gate.
    if ((actor.combat_state != 0 && actor.combat_state != 3) || actor.movement_reload_blocked ||
        actor.posture_state == 1 || actor.posture_state == 2) return false;
    const auto kind = active_weapon(actor);
    const auto& descriptor = weapon_descriptor(kind);
    const bool fresh_pistol = !actor.trigger_held && (kind == WeaponKind::cz75 || kind == WeaponKind::magnum);
    if (!fresh_pistol && original_elapsed_ms(time_, actor.shot_time) <= descriptor.interval_ms()) return false;
    actor.shot_time = time_;
    if (!selected_weapon(actor).magazine) { reload(actor, true); return false; }
    combat_state(actor, 3, time_);
    actor.shoot_flag = 2;
    --selected_weapon(actor).magazine;
    actor.shot_visual_time = time_;
    actor.weapon_phase = 2;
    ActorEvent phase;
    phase.kind = ActorEvent::Kind::weapon_phase;
    phase.entity = actor.entity;
    phase.weapon = kind;
    phase.phase = 2;
    emit(std::move(phase));
    if (actor.kind == ActorKind::bot) {
        actor.shot_light = true;
        ActorEvent light;
        light.kind = ActorEvent::Kind::weapon_light;
        light.entity = actor.entity;
        light.value = 1;
        light.radius = 200;
        emit(std::move(light));
    }
    ActorEvent sound;
    sound.kind = ActorEvent::Kind::sound;
    sound.entity = actor.entity;
    sound.weapon = kind;
    sound.position = combat_position(actor);
    sound.name = std::string("sound/gun/") + descriptor.name + "/shoot";
    sound.environment_sound = true;
    emit(std::move(sound));

    if (actor.kind == ActorKind::player) {
        const auto offset = actor.fully_crouched && !actor.player_camera.special ? 6 : 4;
        float pitch = descriptor.field_c0_dc[offset];
        float yaw = descriptor.field_c0_dc[offset + 1];
        if (actor.player_camera.special) { pitch *= 1.5f; yaw *= 1.5f; }
        const auto pitched = multiply(actor.orientation, combat_axis_rotation({1, 0, 0}, pitch));
        if (-pitched[6] < -.30000001192092896f) {
            actor.orientation = pitched;
            if (combat_.frame_changed) combat_.frame_changed(actor.entity, false);
        }
        if (!(original_random(random_state_) & 1)) yaw = -yaw;
        player_yaw(actor, yaw); // virtual+40 updates player special_rotation, then 00411500.
        if (combat_.frame_changed) combat_.frame_changed(actor.entity, false);
    }
    const auto origin = combat_origin(actor);
    const auto incoming = transform_vector(actor.orientation, {0, 0, -1});
    ActorEvent flash;
    flash.kind = ActorEvent::Kind::shot;
    flash.entity = actor.entity;
    flash.weapon = kind;
    flash.name = "golge_aydinlik";
    flash.position = origin;
    flash.direction = incoming;
    flash.value = 240;
    emit(std::move(flash));

    if (combat_.has_attachment && combat_.has_attachment(actor.entity, descriptor.name)) {
        const auto attachment = combat_.attachment(actor.entity, descriptor.name);
        const auto position = transform_point(attachment, {});
        const auto left = transform_vector(attachment, {-1, 0, 0});
        const auto forward = transform_vector(attachment, {0, 0, -1});
        const auto down = transform_vector(attachment, {0, -1, 0});
        const auto emit_particle = [&](std::string name, Vec3 direction, Vec3 first, Vec3 second) {
            ActorEvent particle;
            particle.kind = ActorEvent::Kind::particle;
            particle.entity = actor.entity;
            particle.weapon = kind;
            particle.position = position;
            particle.direction = direction;
            particle.basis_right = first; // Factory argument7, authored plane axis0.
            particle.basis_up = second;   // Factory argument8, authored plane axis1.
            particle.name = std::move(name);
            emit(std::move(particle));
        };
        emit_particle(kind == WeaponKind::baba ? "drop_shell_pump" : "drop_shell_metal",
                      left, forward, down);
        emit_particle(std::string(descriptor.name) + "_smoke", down, left, down);
        if (actor.kind == ActorKind::bot)
            emit_particle(std::string(descriptor.name) + "_bullet", incoming, left, forward);
    }
    if (!combat_.trace) throw std::runtime_error("firing requires original mesh collision queries");
    if (!combat_.camera_fov) throw std::runtime_error("firing requires the actual camera projection");
    for (int pellet = 0; pellet < descriptor.pellets; ++pellet) {
        // Virtual+44 first traces the camera/actor aim line, then00411c80 projects
        // its random target back onto the hit plane before the final muzzle trace.
        Matrix aim = actor.orientation;
        Vec3 aim_origin = origin;
        if (actor.kind == ActorKind::player) {
            if (!combat_.camera) throw std::runtime_error("player firing requires the actual camera pose");
            aim = combat_.camera();
            aim_origin = transform_point(aim, {});
        }
        const auto aim_direction = transform_vector(aim, {0, 0, -1});
        const auto first_end = aim_origin + aim_direction * 1000000.0f;
        const auto preliminary = combat_.trace(aim_origin, first_end, actor.entity, 0x400, 7, false);
        const auto target = preliminary.hit ? preliminary.position : first_end;
        const float spread = actor.kind == ActorKind::player ? descriptor.field_bc : descriptor.field_b8;
        const float ability = actor.kind == ActorKind::bot ? actor.ai.attack_ability : 1;
        if (!combat_.camera_view_distance || !combat_.viewport_width)
            throw std::runtime_error("firing requires the actual camera frustum and viewport");
        const float view_distance = combat_.camera_view_distance();
        const long double width = combat_.viewport_width();
        const float tangent = static_cast<float>(std::tan(
            static_cast<long double>(combat_.camera_fov()) * .008726646192371845f));
        const float span = static_cast<float>(static_cast<long double>(view_distance) * tangent * 2);
        const float scaled_distance = static_cast<float>((((static_cast<long double>(spread) * ability *
            width * .5f) / width) * span) * length(target - aim_origin));
        float radius = scaled_distance / view_distance;
        if (actor.kind == ActorKind::player && actor.player_camera.special) radius = 0;
        const Vec3 random_direction = normalized(Vec3{
            static_cast<float>(original_random_between(random_state_, -.5f, .5f)),
            static_cast<float>(original_random_between(random_state_, -.5f, .5f)),
            static_cast<float>(original_random_between(random_state_, -.5f, .5f))});
        Vec3 point = target + random_direction * radius;
        point = point - preliminary.normal * (dot(point, preliminary.normal) - preliminary.plane_distance);
        const auto direction = normalized(point - origin);
        const auto hit = combat_.trace(origin, origin + direction * 10000000.0f, actor.entity, 0x400, 7, false);
        if (!hit.hit) continue;
        auto* victim = find(hit.entity);
        // 00416d50 trace+54/+1d9 gates only linked physical proxies, not root actors.
        const bool dead_proxy=hit.owner && victim && victim->combat_state==5;
        if (!dead_proxy && combat_.impact)
            combat_.impact(hit, direction, 0); // Original physical-material type0/resource empty.
        if (dead_proxy) continue;
        if (actor.kind == ActorKind::player && victim) {
            if (!ai_visibility(*victim, actor)) ai_queue_target(*victim, combat_position(actor));
            ai_notify_shot(*victim);
        }
        if (victim && victim->kind == ActorKind::bot) {
            victim->hit_direction = direction;
            victim->hit_time = time_;
            victim->hit_weapon = kind;
            victim->hit_region = combat_hit_direction(actor, *victim);
        }
        // 0041a8f0's bool parameter is attacker kind==player. Bot physical shots
        // cannot damage props or actors; their AI immediate player-hit path owns it.
        if (actor.kind != ActorKind::player || !hit.entity) continue;
        float damage = descriptor.field_a8_b0[1];
        if (victim) {
            if (hit.mesh.find("col_kafa") != std::string_view::npos) damage = descriptor.field_a8_b0[0];
            else if (hit.mesh.find("col_ust_bacak") != std::string_view::npos ||
                     hit.mesh.find("col_alt_bacak") != std::string_view::npos)
                damage = descriptor.field_a8_b0[2];
        }
        if (!combat_.damage) throw std::runtime_error("firing requires Game entity damage dispatch");
        combat_.damage(hit, damage, actor.entity);
    }
    return true;
}

void ActorRuntime::script_animation(EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor) throw std::runtime_error("script animation target is not an actor");
    combat_state(*actor, 7, time_);
}

void ActorRuntime::land(ActorState& actor, Vec3 start, const Trace& hit) {
    if (!combat_.land_sound)
        throw std::runtime_error("landing requires Game physical-material sound dispatch");
    combat_.land_sound(actor.entity, actor.kind != ActorKind::player, start, hit);
}

std::uint32_t ActorRuntime::random_word() noexcept {
    return original_random(random_state_);
}

// Original AI 0043c1e0/0043ca40. Time values are milliseconds except authored
// durations, which are seconds. Combat and navigation are independent machines.
namespace {
Vec3 ai_matrix_position(const Matrix& matrix) {
    return {matrix[12], matrix[13], matrix[14]};
}
Vec3 ai_forward(const ActorState& actor) { return transform_vector(actor.orientation, {0,0,-1}); }
Vec3 ai_right(const ActorState& actor) { return transform_vector(actor.orientation, {-1,0,0}); }
long double ai_dot_extended(Vec3 a, Vec3 b) {
    return (static_cast<long double>(a.z) * b.z + static_cast<long double>(a.y) * b.y) +
           static_cast<long double>(a.x) * b.x;
}
float ai_elapsed(double now, double then) {
    // Original DWORD clock subtraction, including wraparound.
    return static_cast<float>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(now)) -
                              static_cast<std::uint32_t>(static_cast<std::uint64_t>(then)));
}
std::vector<std::string> ai_tokens(std::string_view line) {
    std::vector<std::string> tokens;
    std::string token;
    bool quoted = false, saw_quote = false;
    const auto finish = [&]() {
        if (token.empty()) return false;
        // 00463c60 removes first/last bytes of any completed quoted token,
        // not only tokens whose first byte was a quote. No escape syntax.
        if (saw_quote && !quoted && token.size() >= 2)
            token = token.substr(1, token.size() - 2);
        const bool comment = token.starts_with("//");
        if (!comment) tokens.push_back(std::move(token));
        token.clear();
        quoted = saw_quote = false;
        return comment;
    };
    for (const unsigned char c : line) {
        if (!c) break;
        // 004866c0 additionally admits these twelve Turkish ANSI bytes.
        constexpr std::string_view extra{"\xfd\xdd\xdc\xfc\xde\xfe\xd0\xf0\xc7\xe7\xd6\xf6", 12};
        const bool printable = (c >= 32 && c < 127) || extra.find(static_cast<char>(c)) != std::string_view::npos;
        // Nonprinting bytes terminate tokens even inside quotes.
        if (!printable || (c == ' ' && !quoted)) {
            if (finish()) return tokens;
        } else {
            token.push_back(static_cast<char>(c));
            if (c == '"') { quoted = !quoted; saw_quote = true; }
        }
    }
    finish();
    return tokens;
}
float ai_number(const std::string& token) { return static_cast<float>(std::strtod(token.c_str(), nullptr)); }
int ai_integer(const std::string& token) {
    std::size_t i = 0;
    while (i < token.size() && (token[i] == ' ' || (token[i] >= '\t' && token[i] <= '\r'))) ++i;
    bool negative = false;
    if (i < token.size() && (token[i] == '+' || token[i] == '-')) negative = token[i++] == '-';
    std::uint32_t value = 0;
    while (i < token.size() && token[i] >= '0' && token[i] <= '9')
        value = value * 10 + static_cast<unsigned>(token[i++] - '0');
    if (negative) value = 0u - value;
    return std::bit_cast<std::int32_t>(value);
}
const ActorWaypoint& ai_waypoint(const AiState& ai) {
    // Preserve deferred index consumption. Original parser/add-index accepts
    // arbitrary indices; fail at dereference rather than silently repairing it.
    return ai.waypoints.at(static_cast<std::size_t>(ai.route.at(static_cast<std::size_t>(ai.route_cursor))));
}
}
void ActorRuntime::read_ai(ActorState& actor) {
    actor.ai = AiState{};
    auto& ai = actor.ai;
    ai.initial_position = ai_matrix_position(actor.orientation);
    ai.initial_forward = ai_forward(actor);
    ai.move_decision_time = time_;
    const std::string filename = "level/ai/" + actor.name + ".txt";
    if (!assets_.contains(filename)) return;
    // Raw asset bytes: original ANSI files are not UTF-8 documents.
    const auto bytes = assets_.bytes(filename);
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto tokens = ai_tokens(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        start = end == std::string_view::npos ? text.size() : end + 1;
        if (tokens.empty()) continue;
        const auto& key = tokens[0];
        const auto count = tokens.size();
        if (key == "reaction_time" && count == 3) {
            ai.reaction_min = ai_number(tokens[1]); ai.reaction_max = ai_number(tokens[2]);
        } else if (key == "attack_last_see_point_percent" && count == 2) ai.last_attack_probability = ai_number(tokens[1]);
        else if (key == "attack_last_see_point_time" && count == 2) ai.last_attack_time = ai_number(tokens[1]);
        else if (key == "moveto_last_see_point_percent" && count == 2) ai.last_move_probability = ai_number(tokens[1]);
        else if (key == "dexterity" && count == 2) ai.dexterity = ai_number(tokens[1]);
        else if (key == "attack_percents" && count == 4) {
            float scale = 1;
            for (std::size_t i = 0; i < 3; ++i) {
                const float value = ai.attack_weights[i] = ai_number(tokens[i + 1]);
                if (value > 1 && 1 / value < scale) scale = 1 / value;
            }
            scale *= 255;
            for (auto& value : ai.attack_weights) value *= scale;
        } else if (key == "attack_ability" && count == 2) {
            ai.attack_ability = static_cast<float>(static_cast<long double>(std::strtod(tokens[1].c_str(), nullptr)) * 1.25L);
        } else if (key == "radius" && count == 2) ai.radius = ai_number(tokens[1]);
        else if (key == "movement_when_attacking_minmax_time" && count == 3) {
            ai.movement_min = ai_number(tokens[1]); ai.movement_max = ai_number(tokens[2]);
        } else if (key == "movement_when_attacking_run_percent" && count == 2) ai.run_probability = ai_number(tokens[1]);
        else if (key == "see_player_sound" && count == 2) ai.see_player_sound = tokens[1];
        else if (key == "add_waypoint" && count == 10) {
            ActorWaypoint waypoint;
            waypoint.position = {ai_number(tokens[1]), ai_number(tokens[2]), ai_number(tokens[3])};
            waypoint.direction = original_normalized(Vec3{ai_number(tokens[4]), ai_number(tokens[5]), ai_number(tokens[6])});
            waypoint.dwell_seconds = ai_number(tokens[7]);
            waypoint.crouch = ai_integer(tokens[8]) != 0;
            waypoint.run = ai_integer(tokens[9]) != 0;
            ai.waypoints.push_back(waypoint);
        } else if (key == "autostart_waypoints" && count > 1) {
            for (std::size_t i = 1; i < count; ++i)
                ai.route.push_back(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(ai_integer(tokens[i])) - 1u));
            ai.autostart = true;
        }
    }
}
int ActorRuntime::ai_visibility(const ActorState& actor, const ActorState& player) {
    constexpr float half_height = 49.0839996337890625f;
    const Vec3 start = ai_matrix_position(actor.orientation) + Vec3{0,0,half_height};
    const Vec3 player_side = ai_right(player); // 004363a0, not player forward.
    const Vec3 player_position = ai_matrix_position(player.orientation);
    const std::array<Vec3,3> targets{{player_position + Vec3{0,0,-half_height} + player_side * 10,
                                   player_position,
                                   player_position + Vec3{0,0,half_height} - player_side * 10}};
    int mask = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto hit = combat_.trace(start, targets[i], actor.entity, 0x400, 7, false);
        if (!hit.hit || (hit.owner != 0 && hit.actor_type == 2)) mask |= 1 << i;
    }
    return mask;
}
bool ActorRuntime::ai_friendly_fire(const ActorState& actor) {
    const Vec3 origin = ai_matrix_position(actor.orientation);
    const auto hit = combat_.trace(origin, origin + ai_forward(actor) * 100000,
                                   actor.entity, 0x400, 4, false);
    return hit.hit && hit.owner != 0 && hit.actor_type == 1;
}
void ActorRuntime::ai_choose_movement(ActorState& actor, const ActorState& player) {
    auto& ai = actor.ai;
    const float distance = original_distance(ai_matrix_position(player.orientation), ai_matrix_position(actor.orientation));
    float probability = ai.run_probability;
    if (distance > 500) probability *= 3;
    else if (distance < 100) { ai.run = false; ai.move_mode = 1; return; }
    if (original_random_between(random_state_, 0, 1) < probability) {
        ai.run = true;
        const float choice = original_random_between(random_state_, 0, 5);
        if (choice > 4) ai.move_mode = 2;
        else if (choice > 3) ai.move_mode = 4;
        else if (choice > 2) ai.move_mode = 0;
        else if (choice > 1) ai.move_mode = 5;
        else if (choice > 0) ai.move_mode = 3;
    } else {
        ai.run = false;
        const float choice = original_random_between(random_state_, 0, 2);
        if (choice > 1) ai.move_mode = 0;
        else if (choice > 0) ai.move_mode = 1;
    }
}
bool ActorRuntime::ai_ready(ActorState& actor) {
    const long double choice = original_random_between(random_state_, 0, 1);
    const float numerator = choice <= .6 ? 1.0f : choice > .8 ? 2.0f : .5f;
    return can_fire(actor, numerator / actor.ai.dexterity);
}
void ActorRuntime::update_bot(ActorState& actor, float, CollisionWorld* collision) {
    auto& ai = actor.ai;
    if (!ai.enabled) return;
    if (ai.autostart) {
        waypoint_start(actor.entity);
        ai.autostart = false;
    }
    ActorState* player = this->player();
    const int visibility = player ? ai_visibility(actor, *player) : 0;
    if (ai.combat_state == 1) {
        ai.acquisition = false;
        if (!visibility) {
            ai.combat_state = 0;
            ai.navigation_state = 4;
            ai.movement_pending = false;
        } else {
            if (ai_ready(actor)) {
                ai.last_seen_valid = true;
                ai.reaction_min = ai.reaction_max = 0;
                ai.last_seen_position = ai_matrix_position(player->orientation);
                ai.last_seen_time = time_;
                ai.movement_pending = false;
                ai_attack(actor, *player, visibility, collision);
            }
            if (!ai.movement_decision) {
                if (!ai.sound_played && !ai.see_player_sound.empty()) {
                    ActorEvent event;
                    event.kind = ActorEvent::Kind::sound;
                    event.entity = actor.entity;
                    event.name = ai.see_player_sound;
                    event.category = "see_player";
                    event.position = ai_matrix_position(actor.orientation);
                    event.environment_sound = true;
                    emit(std::move(event));
                }
                ai.sound_played = true;
                ai_choose_movement(actor, *player);
                ai.movement_decision = true;
                ai.move_duration = original_random_between(random_state_, 2, 2.5f);
                ai.move_segment_time = time_;
            } else {
                ai_turn(actor, ai_matrix_position(player->orientation), 360);
                if (ai_elapsed(time_, ai.move_segment_time) < 1000.0L * ai.move_duration && ai_matrix_position(actor.orientation).z != 0) {
                    actor.movement_walk = !ai.run;
                    if (ai.move_mode != 1 && original_distance(ai_matrix_position(player->orientation), ai_matrix_position(actor.orientation)) < 100) {
                        ai_choose_movement(actor, *player);
                    } else {
                        switch (ai.move_mode) {
                        case 0: command_move(actor, 0); break;
                        case 1: command_move(actor, 1); break;
                        case 2: command_move(actor, 2); break;
                        case 3: command_move(actor, 3); break;
                        case 4: command_move(actor, 2); command_move(actor, 0); break;
                        case 5: command_move(actor, 3); command_move(actor, 0); break;
                        }
                    }
                } else {
                    ai.movement_decision = false;
                    ai.move_decision_time = time_;
                    ai.movement_auxiliary_time = 0;
                }
            }
        }
    } else if (ai.combat_state == 0) {
        if (visibility) {
            ai.last_seen_valid = true;
            ai.last_seen_position = ai_matrix_position(player->orientation);
            if (!ai.acquisition) {
                ai.last_seen_time = time_;
                ai.acquisition = true;
            } else if (ai_elapsed(time_, ai.last_seen_time) > 1000.0L * .2f) {
                ai.combat_state = 1;
                ai.reaction_delay = original_random_between(random_state_, ai.reaction_min, ai.reaction_max);
                ai.move_decision_time = ai.reaction_time = time_;
            }
            ai.movement_pending = false;
        } else {
            ai.acquisition = false;
            switch (ai.navigation_state) {
            case 1:
                if (!ai.route.empty() && ai_elapsed(time_, ai.dwell_time) > 1000.0L * ai_waypoint(ai).dwell_seconds) {
                    ai.route_cursor = (ai.route_cursor + 1) % static_cast<int>(ai.route.size());
                    actor.movement_walk = !ai_waypoint(ai).run;
                    ai.navigation_state = 3;
                    if (actor.posture_state != 0) set_posture(actor, 2);
                }
                break;
            case 2:
                if (!ai.movement_pending) {
                    if (ai.route.empty()) ai.navigation_state = 0;
                    else {
                        ai.navigation_state = 1;
                        ai.dwell_time = time_;
                        const Vec3 direction = ai_waypoint(ai).direction;
                        if (direction.x != 99999) ai_turn(actor, ai_matrix_position(actor.orientation) + direction * 1000, 360);
                    }
                }
                break;
            case 3:
                if (actor.posture_state == 0) {
                    ai.navigation_state = 2;
                    ai_queue_target(actor, ai_waypoint(ai).position);
                }
                break;
            case 4:
                if (!ai.last_seen_valid) { ai.navigation_state = 0; ai.movement_pending = false; }
                else if (ai_elapsed(time_, ai.last_seen_time) >= 1000.0L * ai.last_attack_time) {
                    if (original_random_between(random_state_, 0, 1) >= ai.last_move_probability) {
                        ai.last_seen_valid = false;
                        actor.movement_walk = true;
                        ai_queue_target(actor, ai.initial_position);
                        ai.navigation_state = 11;
                    } else {
                        ai_queue_target(actor, ai.last_seen_position);
                        ai.navigation_state = 5;
                        ai.search_time = time_;
                    }
                } else if (ai_ready(actor) && original_random_between(random_state_, 0, 1) < ai.last_attack_probability) {
                    ai_turn(actor, ai.last_seen_position, 360);
                    ai.movement_pending = false;
                    if (!ai_friendly_fire(actor)) fire(actor, collision);
                }
                break;
            case 5:
                if (!ai.movement_pending) { ai.navigation_state = 6; ai.search_time = time_; }
                else actor.movement_walk = ai_elapsed(time_, ai.search_time) < 1500;
                break;
            case 6:
                // The original rerolls every update, not once on state entry.
                if (ai_elapsed(time_, ai.search_time) > 1000.0L * static_cast<float>(original_random_between(random_state_, 2.25f, 3.75f))) {
                    ai.navigation_state = 11;
                    actor.movement_walk = true;
                    ai_queue_target(actor, ai.initial_position);
                }
                break;
            case 11:
                if (!ai.movement_pending) {
                    ai.navigation_state = 0;
                    ai.movement_pending = false;
                    ai_turn(actor, ai_matrix_position(actor.orientation) + ai.initial_forward * 1000, 360);
                }
                break;
            }
        }
    }
    if (ai.movement_pending && ai_follow_path(actor, collision)) ai.movement_pending = false;
}
void ActorRuntime::ai_attack(ActorState& actor, ActorState& player, int visibility, CollisionWorld* collision) {
    auto& ai = actor.ai;
    const Vec3 source_position = ai_matrix_position(actor.orientation);
    const Vec3 player_position = ai_matrix_position(player.orientation);
    const Vec3 displacement = player_position - source_position;
    const long double distance_probability = 1.0L - ai_dot_extended(displacement, displacement) *
                                                2.0000000233721948e-7f;
    float probability = static_cast<float>(distance_probability);
    if (distance_probability < .30000001192092896f) probability = .20000000298023224f;
    if (player.movement_speed != 0 || player.input_gas) probability *= .75f;
    const bool direct_hit = original_random_between(random_state_, 0, 1) < static_cast<long double>(probability) * ai.attack_ability &&
                           ai_elapsed(time_, ai.reaction_time) > 1000.0L * ai.reaction_delay;
    const float upper = visibility & 4 ? ai.attack_weights[0] : 0;
    const float center = visibility & 2 ? ai.attack_weights[1] : 0;
    const float lower = visibility & 1 ? ai.attack_weights[2] : 0;
    const float selected = original_random_between(random_state_, 0, (lower + center) + upper);
    const bool friendly = ai_friendly_fire(actor);
    Vec3 target = player_position;
    if (direct_hit) {
        player.hit_direction = original_normalized(displacement);
        player.hit_time = time_;
        player.hit_weapon = active_weapon(actor);
        Vec3 horizontal_source = source_position;
        horizontal_source.z = player_position.z;
        const Vec3 bearing = original_normalized(horizontal_source - player_position);
        const float front = static_cast<float>(ai_dot_extended(ai_forward(player), bearing));
        const float side = static_cast<float>(ai_dot_extended(ai_right(player), bearing));
        const float height = source_position.z - player_position.z;
        const bool flank = front > -.5f && front < .89999997615814209f;
        if (front < -.5f) player.hit_region = 9;
        else if (height > 120) player.hit_region = flank ? (side >= 0 ? 4 : 5) : 0;
        else if (height < -120) player.hit_region = flank ? (side >= 0 ? 6 : 7) : 1;
        else player.hit_region = flank ? (side >= 0 ? 3 : 2) : 8;
        player_effect(player, .5f);
        ai_notify_shot(actor);
        int region = 2;
        if (selected < upper) { region = 0; target.z += 16.361333847045898f; }
        else if (selected > upper && selected < upper + center) region = 1;
        else target.z -= 81.806663513183594f;
        if (!friendly && !combat_.bot_damage_blocked()) {
            constexpr std::array<float, 3> yaw{1,.3f,.1f}, pitch{1,.2f,.1f};
            camera_shake(player.entity, .1f, yaw[region], pitch[region]);
            hurt(player.entity, weapon_descriptor(active_weapon(actor)).field_9c_a4[region], actor.entity);
        }
    }
    // Physical bot shots are visual/prop-collision actions; direct damage above
    // is an independent original branch and may occur before firing's own gate.
    ai_turn(actor, target, 360, true);
    if (!friendly && ai_elapsed(time_, ai.reaction_time) > 1000.0L * ai.reaction_delay) fire(actor, collision);
}
void ActorRuntime::update_ai(CollisionWorld* collision) {
    ai_path_budget_ = 500;
    // 004164d0 unregisters AI through 0043aca0 without removing the corpse
    // actor. Membership is independent of kind, enabled state and retirement.
    const auto registered = [&](std::size_t index) {
        return index < actors_.size() && actors_[index].ai.registered;
    };
    const auto next = [&](std::size_t index) {
        const std::size_t first = index < actors_.size() ? index + 1 : 0;
        for (std::size_t i = first; i < actors_.size(); ++i) if (registered(i)) return i;
        for (std::size_t i = 0; i < first && i < actors_.size(); ++i) if (registered(i)) return i;
        return actors_.size();
    };
    if (!registered(ai_cursor_)) ai_cursor_ = next(ai_cursor_);
    if (ai_cursor_ == actors_.size()) return;
    const std::size_t start = ai_cursor_;
    ai_iteration_ = start;
    do {
        auto& actor = actors_[ai_iteration_];
        if (actor.ai.enabled) update_bot(actor, .033f, collision);
        // 0043a990 owns the same iterator; do not restore the local actor index
        // after notification. Sentinel.next is the registry's first bot.
        ai_iteration_ = next(ai_iteration_);
    } while (ai_iteration_ != start && ai_path_budget_ > 0);
    // Persistent 004a8924 is deliberately NOT advanced at frame/budget end.
}
void ActorRuntime::ai_notify_shot(ActorState& source) {
    // Native bug fix for 0043a990/0043ab8a: q<=.5 bypassed all twelve
    // target-scratch writes on first use. Seed that first target from the
    // function's actual source argument, exactly as its q>1.5 branch does;
    // subsequent low samples still reuse the previous perturbed target.
    Vec3 target = ai_matrix_position(source.orientation);
    constexpr float eye = 39.0839996337890625f;
    for (ai_iteration_ = 0; ai_iteration_ < actors_.size(); ++ai_iteration_) {
        auto& candidate = actors_[ai_iteration_];
        auto& ai = candidate.ai;
        if (!ai.registered || !ai.enabled ||
            candidate.entity == source.entity || ai.movement_pending || ai.combat_state != 0 || ai.navigation_state != 0)
            continue;
        const auto hit = combat_.trace(ai_matrix_position(candidate.orientation) + Vec3{0,0,eye},
                                       ai_matrix_position(source.orientation) + Vec3{0,0,eye}, candidate.entity, 0x400, 7, false);
        // The original requires a hit with ANY actor owner, not clear LOS
        // and not necessarily a hit on the function's source actor.
        if (!hit.hit || hit.owner == 0) continue;
        const float choice = static_cast<float>(original_random_between(random_state_, 0, 3));
        if (choice > 1.5f) target = ai_matrix_position(source.orientation);
        else if (choice > .5f) target = ai_matrix_position(player()->orientation);
        Vec3 offset{static_cast<float>(original_random_between(random_state_, 0, 1)),
                    static_cast<float>(original_random_between(random_state_, 0, 1)),
                    static_cast<float>(original_random_between(random_state_, 0, 1))};
        offset = original_normalized(offset);
        const float distance = static_cast<float>(original_random_between(random_state_, 0, 150));
        target = target + offset * distance;
        ai_queue_target(candidate, target);
    }
    // ai_iteration_ is now the original registry sentinel. The enclosing
    // scheduler sees sentinel.next=head instead of the interrupted AI node.
}
// Original path routines 0043a670, 0043b450-0043c1b8. Simulated search
// changes only the actor matrix; canonical CharacterMotionState is untouched.
namespace {
void ai_matrix_position(Matrix& matrix, Vec3 position) {
    matrix[12] = position.x; matrix[13] = position.y; matrix[14] = position.z;
}
float ai_original_dot(Vec3 a, Vec3 b) {
    return (a.z * b.z + a.y * b.y) + a.x * b.x;
}
float ai_original_angle(Vec3 a, Vec3 b) {
    return static_cast<float>(std::acos(static_cast<double>(std::clamp(ai_original_dot(a, b), -1.0f, 1.0f))) * 57.295780181884765625f);
}
// 0043b7f0: leave an obstacle only after crossing the target/contact line
// with the original angular and detour-length tests. These vectors are NOT
// normalized: 00444420 is a plain dot product.
bool ai_crossed_obstacle(const AiState& ai, Vec3 position) {
    const Vec3 contact = ai.obstacle_contact;
    const Vec3 target = ai.target;
    const float dx = contact.x - target.x;
    const float dy = contact.y - target.y;
    const float area = std::abs(((target.y - contact.y) * position.x +
                                (contact.y - position.y) * target.x +
                                (position.y - target.y) * contact.x) * .5f);
    const double distance = (static_cast<double>(area) + area) /
        std::sqrt(static_cast<double>(dy) * dy + static_cast<double>(dx) * dx);
    if (distance > 16) return false;
    const float projection = std::abs(static_cast<float>(ai_dot_extended(target - contact, position - contact)));
    if (std::abs(std::cos(3.0)) > projection) return false;
    const float from_contact = original_distance(position, contact);
    const float target_contact = original_distance(target, contact);
    const float detour = static_cast<float>(static_cast<double>(target_contact) + from_contact);
    return static_cast<double>(original_distance(target, position)) * 1.0499999523162841796875f < detour;
}
}

namespace {
void actor_cache_hit(ActorHit& cached, const ActorHit& hit) noexcept {
    cached = hit;
    // 0043a2f0/0043b450/0043bd30 retain contact geometry, never borrowed views.
    cached.mesh = {}; cached.shader = {};
}
}

// 0043a670. Snapshot the query result immediately; the next query overwrites
// the original engine's shared hit position/normal fields.
bool ActorRuntime::ai_path_blocked(ActorState& actor, Vec3 start, Vec3 end, ActorHit& result) {
    const Vec3 direction = original_normalized(end - start);
    const float distance = original_distance(start, end);
    result = combat_.trace_motion(start, direction, distance, actor.hull,
                                 actor.entity, 0x200, 3, false, actor.entity, true);
    if (result.hit) {
        actor_cache_hit(ai_path_query_hit_, result);
        actor_cache_hit(ai_path_block_hit_, result);
    }
    return result.hit;
}

// 0043b970: direct reachability does not release the obstacle latch when the
// candidate and target are on different heights.
bool ActorRuntime::ai_path_direct(ActorState& actor) {
    if (ai_path_budget_ > 0) --ai_path_budget_;
    ActorHit hit;
    const Vec3 origin = ai_matrix_position(actor.orientation);
    return !ai_path_blocked(actor, origin, actor.ai.target, hit) &&
        std::abs(origin.z - actor.ai.target.z) < .009999999776482582f;
}

// 0043a2f0/0043a370 deliberately retain the preceding block-query contact
// when the downward sweep misses, matching the original shared globals.
void ActorRuntime::ai_path_ground(ActorState& actor, Vec3& position) {
    position.z += 20;
    ActorHit hit;
    ai_path_blocked(actor, position, position - Vec3{0,0,1000}, hit);
    position = ai_path_block_hit_.position;
}

// 0043c160: guard the request before the target is grounded or path reset.
void ActorRuntime::ai_queue_target(ActorState& actor, Vec3 target) {
    if (actor.ai.radius == 0 || original_distance(ai_matrix_position(actor.orientation), target) < 16) return;
    ai_begin_path(actor, target);
}

// 0043bcb0: unconditional target grounding, snapshots and latch reset.
void ActorRuntime::ai_begin_path(ActorState& actor, Vec3 target) {
    auto& ai = actor.ai;
    ai.target = target;
    ai_path_ground(actor, ai.target);
    ai.saved_orientation = actor.orientation;
    ai.working_orientation = ai.saved_orientation;
    ai.obstacle_following = false;
    ai.obstacle_side_valid = false;
    ai.movement_pending = true;
    ai.turned = false;
    ai.path.clear();
}

// 00439d70/0043a090. Measure pitch at fully corrected yaw, restore the
// matrix, then apply the caller's yaw limit and measured pitch independently.
// Aiming recomputes 00439ff0's muzzle origin after the temporary yaw.
// The rotation helpers commit through 0041c800(..., 0): notify dirtiness after
// every actual commit, including temporary yaw. Raw matrix copies and shadow
// position writes never notify; flag zero never relinks BSP membership.
bool ActorRuntime::ai_turn(ActorState& actor, Vec3 target, float limit, bool muzzle) {
    const Matrix saved = actor.orientation;
    const auto turn_origin = [&]() {
        if (muzzle) return combat_origin(actor);
        return ai_matrix_position(actor.orientation);
    };
    const Vec3 origin = turn_origin();
    Vec3 forward = ai_forward(actor);
    forward.z = 0;
    forward = original_normalized(forward);
    Vec3 direction = target - origin;
    direction.z = 0;
    direction = original_normalized(direction);
    const float yaw = ai_original_angle(forward, direction);
    const float side = cross(direction, forward).z;
    if (yaw > 0) {
        rotate_world(actor.orientation, {0, side > 0 ? -yaw : yaw, 0});
        combat_.frame_changed(actor.entity, false);
    }
    const Vec3 corrected_forward = original_normalized(ai_forward(actor));
    const Vec3 full_direction = original_normalized(target - turn_origin());
    const float pitch = ai_original_angle(corrected_forward, full_direction);
    actor.orientation = saved;
    bool completed = false;
    if (yaw > 0) {
        if (side > 0) {
            completed = yaw <= limit;
            rotate_world(actor.orientation, {0, -(completed ? yaw : limit), 0});
            combat_.frame_changed(actor.entity, false);
        } else if (side < 0) {
            completed = yaw <= limit;
            rotate_world(actor.orientation, {0, completed ? yaw : limit, 0});
            combat_.frame_changed(actor.entity, false);
        }
    } else if (yaw == 0) completed = true;
    if (pitch > 0) {
        Matrix candidate = actor.orientation;
        rotate_local(candidate, {full_direction.z > corrected_forward.z ? pitch : -pitch, 0, 0});
        if (-candidate[6] < -.300000011920928955078125f) {
            actor.orientation = candidate;
            combat_.frame_changed(actor.entity, false);
        }
    }
    return completed;
}

// 0043b450: 30-unit shadow step, original upward/forward/downward probes,
// and the original strict .7 support-normal threshold.
bool ActorRuntime::ai_path_step(ActorState& actor, Vec3& position) {
    Vec3 forward = ai_forward(actor);
    forward.z = 0;
    forward = original_normalized(forward);
    const Vec3 origin = ai_matrix_position(actor.orientation);
    position = origin + forward * 30;
    if (ai_path_budget_ > 0) --ai_path_budget_;
    ActorHit hit;
    if (!ai_path_blocked(actor, origin, position, hit)) {
        if (ai_path_budget_ > 0) --ai_path_budget_;
        ai_path_ground(actor, position);
        return true;
    }
    const auto query = [&](Vec3 start, Vec3 direction, float distance, std::uint32_t flags) {
        if (ai_path_budget_ > 0) --ai_path_budget_;
        const auto result = combat_.trace_motion(start, direction, distance,
                                                actor.hull, 0, 0x200, flags, true, actor.entity, true);
        if (result.hit) actor_cache_hit(ai_path_query_hit_, result);
        return result;
    };
    float clearance = 20;
    hit = query(origin, {0,0,1}, 20, 7);
    if (hit.hit) clearance = hit.distance;
    const Vec3 gravity_direction = actor.gravity_direction;
    Vec3 raised = origin - gravity_direction * clearance;
    forward = ai_forward(actor);
    hit = query(raised, forward, 30, 3);
    if (!hit.hit) {
        raised = raised + forward * 30;
        hit = query(raised, gravity_direction, 40, 3);
        if (!hit.hit) return false;
    }
    if (!(ai_path_query_hit_.normal.z > .699999988079071044921875f)) return false;
    position = ai_path_query_hit_.position;
    return true;
}

// 0043b710: obstacle-side latch fixes the initial ninety-degree turn; failed
// candidates turn back in thirty-degree increments until a valid step exists.
void ActorRuntime::ai_path_obstacle_step(ActorState& actor) {
    auto& ai = actor.ai;
    rotate_world(actor.orientation, {0, ai.obstacle_side ? -90.0f : 90.0f, 0});
    combat_.frame_changed(actor.entity, false);
    Vec3 candidate;
    while (!ai_path_step(actor, candidate)) {
        rotate_world(actor.orientation, {0, ai.obstacle_side ? 30.0f : -30.0f, 0});
        combat_.frame_changed(actor.entity, false);
    }
    ai_matrix_position(actor.orientation, candidate);
}

// 0043bd30: one simulated transform step. The path is newest-first, like the
// original push-front linked list, not travel-order waypoints.
bool ActorRuntime::ai_search_path(ActorState& actor, CollisionWorld*) {
    auto& ai = actor.ai;
    actor.orientation = ai.working_orientation;
    const Vec3 origin = ai_matrix_position(actor.orientation);
    if (original_distance(origin, ai.target) < 16) return true;
    if (!ai.obstacle_following) {
        ai_turn(actor, ai.target, 360);
        Vec3 candidate;
        if (ai_path_step(actor, candidate)) {
            ai_matrix_position(actor.orientation, candidate);
            ai.obstacle_side_valid = false;
        } else {
            ai.obstacle_contact = ai_path_query_hit_.position;
            ai.obstacle_following = true;
            Vec3 normal = ai_path_query_hit_.normal;
            normal.z = 0;
            normal = original_normalized(normal);
            Vec3 tangent = cross(normal, {0,0,1});
            if (ai.obstacle_side_valid) {
                ai.obstacle_side = ai.obstacle_previous_side;
            } else {
                ai.obstacle_side = !(ai_dot_extended(tangent, ai_forward(actor)) < 0);
            }
            if (!ai.obstacle_side) tangent = tangent * -1;
            ai_turn(actor, ai_matrix_position(actor.orientation) + tangent * 1000, 360);
            ai.obstacle_side_valid = true;
            ai.obstacle_previous_side = ai.obstacle_side;
            if (ai_path_step(actor, candidate)) ai_matrix_position(actor.orientation, candidate);
            else ai_path_obstacle_step(actor);
        }
    } else if (ai_path_direct(actor) || ai_crossed_obstacle(ai, origin)) {
        ai.obstacle_following = false;
    } else {
        ai_path_obstacle_step(actor);
    }
    ai.working_orientation = actor.orientation;
    ai.path.push_front(ai_matrix_position(actor.orientation));
    return false;
}

// 0043bbc0: search backward from newest candidate. The first unobstructed
// shortcut consumes itself and every older node; remaining nodes are newer.
void ActorRuntime::ai_path_move(ActorState& actor, CollisionWorld*) {
    auto& ai = actor.ai;
    actor.orientation = ai.saved_orientation;
    const Vec3 origin = ai_matrix_position(actor.orientation);
    for (std::size_t i = 0; i < ai.path.size() && ai_path_budget_ > 0; ++i) {
        --ai_path_budget_;
        ActorHit hit;
        if (!ai_path_blocked(actor, origin, ai.path[i], hit)) {
            ai_turn(actor, ai.path[i], 360);
            ai.path.erase(ai.path.begin() + static_cast<std::ptrdiff_t>(i), ai.path.end());
            break;
        }
    }
    // Shared 0043a4f0/00411370 command preserves the original posture gate,
    // player camera-placement bypass and multi-command composition.
    command_move(actor, 0);
}

// 0043c060: retain the real matrix across search, and perform at most one new
// shadow step per actor update while sharing the runtime's 500-query budget.
bool ActorRuntime::ai_follow_path(ActorState& actor, CollisionWorld* collision) {
    auto& ai = actor.ai;
    ai.saved_orientation = actor.orientation;
    if (original_distance(ai_matrix_position(actor.orientation), ai.target) < 16) return true;
    if (ai_path_budget_ > 0 && ai.path.size() < 2) ai_search_path(actor, collision);
    actor.orientation = ai.saved_orientation;
    if (!ai.turned && !ai.path.empty()) {
        ai_turn(actor, ai.path.front(), 360);
        ai.turned = true;
    }
    ai.saved_orientation = actor.orientation;
    ai_path_move(actor, collision);
    return false;
}


namespace {
float player_mouse_weight(float setting) noexcept {
    // 00458830 / 0045aa50; negative and positive sliders have distinct slopes.
    return setting < 0 ? .25f - setting * -.0021875000093132257f
                       : .25f + setting * .007499999832361937f;
}
Vec2 player_mouse_poll(PlayerMouseState& state, Vec2 raw, float active_weight) noexcept {
    // 0043e230: unnormalized ten-sample DirectInput filter, one poll per game tick.
    state.samples[state.cursor % 10] = raw;
    ++state.cursor;
    state.idle_samples = raw.x != 0 || raw.y != 0 ? 0 : state.idle_samples + 1;
    Vec2 result{};
    if (state.idle_samples >= 10) return result;
    for (unsigned i = 0; i != state.samples.size(); ++i) {
        const unsigned cursor = (state.cursor - i - 1) % 10;
        result.x += state.samples[cursor].x * active_weight;
        result.y += state.samples[cursor].y * active_weight;
        active_weight *= .2f;
    }
    return result;
}
float player_recovery_seconds(double now, double then) noexcept {
    // 0041f075 converts each DWORD before subtraction, unlike the wrapped gate.
    const double current = static_cast<uint32_t>(static_cast<uint64_t>(now));
    const double start = static_cast<uint32_t>(static_cast<uint64_t>(then));
    const float difference = static_cast<float>(current - start);
    return (difference - 500) / 1000;
}
Matrix player_camera_matrix(Matrix base, const PlayerCameraState& state, float distance) noexcept {
    // 0041eed0; the lift is world Z, and distance follows positive column two.
    if (state.reverse) rotate_local(base, {0, 180, 0});
    base[14] += 70;
    rotate_local(base, {state.pitch, 0, 0});
    base[12] += base[8] * distance;
    base[13] += base[9] * distance;
    base[14] += base[10] * distance;
    return base;
}
void player_pitch(ActorState& actor, float degrees) noexcept {
    // 0041f4e0 -> 00411550 rejects the entire proposed orientation.
    if (actor.combat_state == 4 || actor.combat_state == 5) return;
    if (actor.player_camera.special) actor.player_camera.special_rotation += degrees;
    Matrix candidate = actor.orientation;
    rotate_local(candidate, {degrees, 0, 0});
    if (candidate[6] > .3f) actor.orientation = candidate;
}
void player_yaw(ActorState& actor, float degrees) noexcept {
    // 0041f520 -> 00411500 -> 00436be0, around the current orientation row two.
    if (actor.combat_state == 4 || actor.combat_state == 5) return;
    if (actor.player_camera.special) actor.player_camera.special_rotation += degrees;
    rotate_world(actor.orientation, {0, -degrees, 0});
}
bool player_zoom_update(PlayerCameraState& state, float base_weight,
                        float& active_weight, bool menu) noexcept {
    // 0045ee00 is an enabled child of the input controller, after mouse polling.
    if (state.special == 1) {
        state.scope_visible = true;
        if (!state.scope_entry_completed) {
            state.scope_fov -= 6;
            if (state.scope_fov < 15) {
                active_weight = base_weight * .0833333358168602f;
                state.special = 2;
                state.scope_fov = 15;
                state.scope_entry_completed = true;
            }
            return true;
        }
    } else if (state.special == 3) {
        if (state.scope_visible) {
            state.scope_fov += 6;
            state.scope_entry_completed = false;
            if (state.scope_fov > 90) {
                state.special = 0;
                active_weight = base_weight;
                state.scope_visible = false;
                state.scope_fov = 90;
                state.scope_rotation_anchor = state.special_rotation;
            }
            return true;
        }
    } else if (state.special == 2 && !menu) {
        active_weight = base_weight * .0833333358168602f;
    }
    return false;
}
}

void ActorRuntime::set_mouse_settings(float sensitivity, bool invert) noexcept {
    const float weight = player_mouse_weight(sensitivity);
    if (weight != mouse_base_weight_) mouse_active_weight_ = weight;
    mouse_base_weight_ = weight;
    invert_mouse_ = invert;
}

void ActorRuntime::set_player_input_mode(bool processing, bool use_only, bool menu) noexcept {
    player_processing_ = processing;
    use_only_ = use_only;
    menu_visible_ = menu;
}

bool ActorRuntime::camera_obstructed(const ActorState& actor, const Matrix& candidate) const {
    const auto& state = actor.player_camera;
    const Vec3 position{candidate[12], candidate[13], candidate[14]};
    const Vec3 x{candidate[0], candidate[1], candidate[2]};
    const Vec3 y{candidate[4], candidate[5], candidate[6]};
    const Vec3 width = x * state.half_width, height = y * state.half_height;
    // 0041ec50: corners clockwise, then center. Query includes scene actors;
    // source is ignored, shader mask 0x200 and original category mask seven.
    const std::array<Vec3, 5> ends{{position + width + height,
        position - width + height, position - width - height,
        position + width - height, position}};
    for (const auto end : ends)
        if (combat_.trace(state.ray_origin, end, actor.entity, 0x200, 7, false).hit)
            return true;
    return false;
}
Matrix ActorRuntime::place_camera(ActorState& actor, const Matrix& base) {
    auto& state = actor.player_camera;
    if (state.special) return player_camera_matrix(base, state, 0);
    if (combat_.camera_placement_bypass()) return base; // Original console field +0x10c.
    state.ray_origin = {base[12], base[13], base[14] + 70};
    Matrix candidate = player_camera_matrix(base, state, state.distance);
    if (!camera_obstructed(actor, candidate)) {
        if (!state.recovering) {
            state.recovery_time = time_;
            state.recovering = true;
        }
        const float elapsed = original_elapsed_ms(time_, state.recovery_time);
        if (elapsed > 500 && std::abs(state.distance - state.desired_distance) > .001f) {
            const float previous = state.distance;
            const float seconds = player_recovery_seconds(time_, state.recovery_time);
            const float acceleration = state.recovery_acceleration *
                (actor.combat_state == 4 || actor.combat_state == 5 ? .25f : 1);
            state.distance = std::min(state.desired_distance,
                state.distance_anchor + acceleration * seconds * seconds * .5f);
            candidate = player_camera_matrix(base, state, state.distance);
            if (camera_obstructed(actor, candidate)) {
                state.distance = state.distance_anchor = previous;
                candidate = player_camera_matrix(base, state, state.distance);
                state.recovering = false;
            }
        }
    } else {
        state.recovery_time = time_;
        float distance = state.distance;
        do {
            distance -= 3;
            candidate = player_camera_matrix(base, state, distance);
        } while (distance >= 0 && camera_obstructed(actor, candidate));
        state.distance = state.distance_anchor = std::max(distance, 0.f);
        state.pitch_anchor = state.pitch;
        state.recovering = false;
        candidate = player_camera_matrix(base, state, state.distance);
    }
    // 0041ef40 deliberately latches this flag; it does not clear the far case.
    if (original_distance(Vec3{candidate[12], candidate[13], candidate[14]},
        Vec3{actor.orientation[12], actor.orientation[13], actor.orientation[14]}) < 45)
        state.near = true;
    return candidate;
}
RenderCamera ActorRuntime::camera(CollisionWorld*) {
    auto* selected = player();
    if (!selected) throw std::runtime_error("player camera has no player");
    auto& actor = *selected;
    auto& state = actor.player_camera;
    const bool dead = actor.combat_state == 4 || actor.combat_state == 5;
    if (!combat_.camera_update_blocked() || dead) {
        if (state.hit_effect_active &&
            original_elapsed_ms(time_, state.hit_effect_time) > state.hit_effect_duration * 1000) {
            state.hit_effect_active = false;
            ActorEvent event;
            event.kind = ActorEvent::Kind::player_effect;
            event.entity = actor.entity;
            event.phase = 0;
            emit(std::move(event));
        }
        // 0041f780 shake mutates the player body, not a render-only offset.
        if (actor.shake_active) {
            if (original_elapsed_ms(time_, actor.shake_time) <= actor.shake_duration * 1000) {
                player_yaw(actor, original_random_between(random_state_, -actor.shake_yaw, actor.shake_yaw));
                if (combat_.frame_changed) combat_.frame_changed(actor.entity, false);
                player_pitch(actor, original_random_between(random_state_, -actor.shake_pitch, actor.shake_pitch));
            } else actor.shake_active = false;
        }
        Matrix base = actor.orientation;
        if (dead) {
            mouse_active_weight_ = mouse_base_weight_;
            state.fov = 90;
            combat_.set_camera_fov(90);
            // 0041faf0 captures at death; retain exact captured basis throughout.
            base = state.death_orientation;
            if (combat_.has_attachment(actor.entity, "col_govde")) {
                const Matrix body = combat_.attachment(actor.entity, "col_govde");
                base[12] = body[12]; base[13] = body[13]; base[14] = body[14];
            }
            if (original_elapsed_ms(time_, state.death_roll_time) > 1000 * .03333333507180214f) {
                state.death_roll_time = time_;
                rotate_world(base, {0, .3f, 0});
            }
            state.death_orientation = base;
            state.desired_distance = 250;
        }
        state.world = place_camera(actor, base);
    }
    return RenderCamera{{state.world[12], state.world[13], state.world[14]},
        inverse_rigid(state.world), state.world, 4, 20000, state.fov};
}

void ActorRuntime::command_move(ActorState& actor, int mode) {
    // 00411370, 00411400, 00411850, 004118a0 store independent controller slots.
    if (mode < 0 || mode > 3) throw std::runtime_error("invalid original movement command");
    const bool placement_bypass = combat_.camera_placement_bypass();
    const bool bypass = actor.kind == ActorKind::player && placement_bypass;
    if (!bypass && actor.posture_state != 0) return;
    Vec3 direction;
    unsigned slot;
    if (mode < 2) {
        direction = {actor.orientation[8], actor.orientation[9], actor.orientation[10]};
        if (mode == 0) direction = direction * -1;
        if (actor.kind == ActorKind::player && !placement_bypass) {
            direction.z = 0;
            direction = original_normalized(direction);
        }
        slot = mode == 0 ? 2 : 3;
    } else {
        direction = {actor.orientation[0], actor.orientation[1], actor.orientation[2]};
        if (mode == 2) direction = direction * -1;
        slot = mode == 2 ? 0 : 1;
    }
    actor.movement_commands[slot] = direction;
    actor.input_gas = true;
}

void ActorRuntime::set_posture(ActorState& actor, int action) {
    // 00411900; the public player wrapper 0041f4c0 additionally rejects death.
    const bool bypass = action == 1 && actor.kind == ActorKind::player && combat_.camera_placement_bypass();
    if (action == 1 && actor.airborne && !bypass) return;
    if (actor.combat_state == 2) return;
    if (actor.movement_speed != 0 &&
        ((action == 1 && !actor.fully_crouched) || action == 2)) {
        // Motion virtual +4 = 0041e120 -> 0041e9a0, preserving command slots.
        actor.movement_speed = 0;
        actor.movement_step = 0;
        actor.movement_direction = {};
    }
    actor.posture_state = action; // 0041c090.
}

void ActorRuntime::player_effect(ActorState& actor, float duration) {
    // 0041ec10 enables temporary historical-frame blending, independent of shake.
    actor.player_camera.hit_effect_active = true;
    actor.player_camera.hit_effect_time = time_;
    actor.player_camera.hit_effect_duration = duration;
    ActorEvent event;
    event.kind = ActorEvent::Kind::player_effect;
    event.entity = actor.entity;
    event.phase = 1;
    event.value = duration;
    emit(std::move(event));
}

void ActorRuntime::update_player(ActorState& actor, float, const GameInput& input,
                                 CollisionWorld* collision) {
    // Poll 0043e230 precedes controller 004576a0, even when player input is off.
    const Vec2 mouse = player_mouse_poll(mouse_state_, {input.mouse_x, input.mouse_y},
                                        mouse_active_weight_);
    const auto& previous = *previous_input_;
    const auto released = [](bool before, bool now) { return before && !now; };
    const auto emit_use = [&] {
        ActorEvent event;
        event.kind = ActorEvent::Kind::use;
        event.entity = actor.entity;
        event.name = "_on_use";
        event.position = {actor.orientation[12], actor.orientation[13], actor.orientation[14]};
        emit(std::move(event));
    };
    const auto zoom_sound = [&] {
        ActorEvent event;
        event.kind = ActorEvent::Kind::sound;
        event.entity = actor.entity;
        event.name = "zoom";
        event.category = "gun";
        event.environment_sound = true;
        emit(std::move(event));
    };
    auto& state = actor.player_camera;
    if (player_processing_) {
        const bool dead = actor.combat_state == 4 || actor.combat_state == 5;
        bool zoom_action = false;
        // Original bound one-shots are RELEASE edges, including mouse buttons.
        if (released(previous.aim, input.aim) &&
            weapon_descriptor(active_weapon(actor)).field_e0 &&
            (!actor.airborne || combat_.camera_placement_bypass())) {
            zoom_action = true; // Also suppresses this tick's held reload.
            if (!dead) {
                if (!state.special) {
                    state.special = 1;
                    state.special_rotation = 0;
                } else state.special = 3;
                zoom_sound();
            }
        }
        if (!dead) {
            if (input.forward_down) command_move(actor, 0);
            if (input.backward_down) command_move(actor, 1);
            if (input.left_down) command_move(actor, 2);
            if (input.right_down) command_move(actor, 3);
        }
        // 00457ac3 invokes low-level fire before writing trigger+241 below.
        if (input.fire) fire(actor, collision);
        if (!state.special && !actor.no_weapon) {
            if (released(previous.pistol, input.pistol) && actor.selected_slot == 1 &&
                actor.weapon_slots[0] != WeaponKind::none)
                select(actor, actor.weapon_slots[0]);
            if (released(previous.rifle, input.rifle) && actor.selected_slot == 0 &&
                actor.weapon_slots[1] != WeaponKind::none)
                select(actor, actor.weapon_slots[1]);
            if (released(previous.change_weapon, input.change_weapon))
                select(actor, actor.weapon_slots[1 - actor.selected_slot]);
        }
        if (!zoom_action && input.reload) {
            const auto kind = active_weapon(actor);
            if (state.special && selected_weapon(actor).magazine < weapon_descriptor(kind).capacity && !dead) {
                state.special = 3;
                zoom_sound(); // Input exit request occurs even if reload cannot proceed.
            }
            reload(actor, false);
        }
        if (!state.special && released(previous.drop_weapon, input.drop_weapon))
            drop_weapon(actor.entity);
        if (!state.special && released(previous.use, input.use)) emit_use();
        actor.movement_walk = input.walk;
        if (!dead) {
            if (input.crouch) set_posture(actor, 1);
            if (released(previous.crouch, input.crouch)) set_posture(actor, 2);
        }
        if (input.jump && !jump_latched_ && !state.special &&
            actor.combat_state != 2 && actor.combat_state != 1) {
            const Vec3 position{actor.orientation[12], actor.orientation[13], actor.orientation[14]};
            const auto support = combat_.trace_motion(position, {0, 0, -1}, 25, actor.hull,
                                                       0, 0x200, 7, true, actor.entity, true);
            if (support.hit) combat_.material_action(actor.entity, position, support, "jump_up");
            if (!dead && actor.posture_state == 0) jump(actor, collision);
            jump_latched_ = true;
        }
        if (released(previous.jump, input.jump)) jump_latched_ = false;
        actor.trigger_held = input.fire;
        if (!input.fire) actor.no_ammo_latch = false; // Original byte +240.
        state.pitch_moved = false;
        state.near = false;
        if (mouse.y != 0) {
            player_pitch(actor, invert_mouse_ ? mouse.y : -mouse.y);
            state.pitch_moved = true; // Set even when the proposed pitch is rejected.
        }
        if (mouse.x != 0) {
            player_yaw(actor, mouse.x);
            if (combat_.frame_changed) combat_.frame_changed(actor.entity, false);
        }
    } else if (use_only_ && !state.special && released(previous.use, input.use)) {
        emit_use();
    }
    // 00458666 updates controller children after input; no dt-based interpolation.
    if (player_zoom_update(state, mouse_base_weight_, mouse_active_weight_, menu_visible_))
        combat_.set_camera_fov(state.scope_fov);
}


// Concrete native ownership boundary. Models/poses/VM remain Game-owned.
ActorRuntime::ActorRuntime(AssetStore& assets, EventSink events, CombatHooks combat)
    : assets_(assets), events_(std::move(events)), combat_(std::move(combat)),
      previous_input_(std::make_unique<GameInput>()) {}
ActorRuntime::~ActorRuntime() = default;

void ActorRuntime::clear() {
    actors_.clear();
    player_entity_ = 0;
    motion_actor_ = nullptr;
    ai_cursor_ = ai_iteration_ = 0;
    ai_path_budget_ = 500;
    ai_path_query_hit_ = ai_path_block_hit_ = {};
    // 0042f260 level cleanup does not reseed process-global CRT rand 00468568.
}

ActorState& ActorRuntime::create_bot(EntityHandle entity, std::string name, const Object&) {
    if (!entity || find(entity)) throw std::runtime_error("invalid or duplicate actor entity");
    const auto sentinel = actors_.size();
    auto& actor = actors_.emplace_back();
    actor.entity = entity;
    actor.kind = ActorKind::bot;
    actor.name = std::move(name);
    // 00436620 -> 004430b0 installs this nonidentity basis before the frame1
    // correction is initialized; 0041c800/00411b70 mode1 only copies/invalidates.
    actor.orientation = original_actor_basis;
    if (combat_.frame_changed) combat_.frame_changed(entity, true);
    if (ai_cursor_ == sentinel) ai_cursor_ = actors_.size();
    if (ai_iteration_ == sentinel) ai_iteration_ = actors_.size();
    set_health(entity, 1); // 00417ed0 -> 00412760, never PO.health.
    read_ai(actor);
    actor.ai.registered = true; // 0043adc0 appends after AI construction.
    return actor;
}

ActorState& ActorRuntime::create_player(EntityHandle entity, std::string name, const Object&) {
    if (!entity || find(entity)) throw std::runtime_error("invalid or duplicate actor entity");
    const auto sentinel = actors_.size();
    auto& actor = actors_.emplace_back();
    actor.entity = entity;
    actor.kind = ActorKind::player;
    actor.name = std::move(name);
    actor.orientation = original_actor_basis; // Same base 00417ed0 frame0 as bots.
    if (combat_.frame_changed) combat_.frame_changed(entity, true);
    if (ai_cursor_ == sentinel) ai_cursor_ = actors_.size();
    if (ai_iteration_ == sentinel) ai_iteration_ = actors_.size();
    player_entity_ = entity; // 0041ea60's newest player owns the input controller.
    set_health(entity, 1);
    // 0041ea60 grants registry ID2 with total30 through 00415fb0.
    // The factory's authored player model owns the Desert attachment.
    grant_weapon(entity, WeaponKind::magnum, 30);
    return actor;
}

Matrix ActorRuntime::frame(EntityHandle entity, int selector) const {
    const auto* actor = find(entity);
    if (!actor) throw std::runtime_error("frame target is not an actor");
    return original_actor_frame(*actor, selector);
}

void ActorRuntime::prepare_render(std::uint32_t render_tick, std::uint32_t interval) {
    // 00435ed0: after PKA sampling, raw frame0 uses 00444990/reset0.
    const auto displaced = [](float previous, float current) {
        return std::abs(static_cast<long double>(previous) - current) >
               static_cast<long double>(0.0001); // Original DOUBLE477640.
    };
    for (std::size_t remaining = actors_.size(); remaining != 0;) {
        auto& actor = actors_[--remaining];
        if (actor.retired) continue;
        const auto& previous = actor.previous_position;
        const auto& current = actor.position;
        if (displaced(previous.x, current.x) || displaced(previous.y, current.y) ||
            displaced(previous.z, current.z)) {
            const auto rendered = interpolate_actor_position(previous, current,
                actor.position_time_tick, render_tick, interval);
            if (actor.orientation[12] != rendered.x ||
                actor.orientation[13] != rendered.y || actor.orientation[14] != rendered.z) {
                actor.orientation[12] = rendered.x;
                actor.orientation[13] = rendered.y;
                actor.orientation[14] = rendered.z;
                if (combat_.frame_changed) combat_.frame_changed(actor.entity, true);
            }
        }
        dispatch_animation(actor, render_tick);
    }
}

CharacterMotionHooks ActorRuntime::motion_hooks(ActorState& actor) {
    motion_actor_ = &actor;
    return {this,
        [](void* context, Vec3 start, Vec3 direction, float distance, Bounds hull,
           std::uint32_t, CharacterTraceKind kind) -> Trace {
            auto& runtime = *static_cast<ActorRuntime*>(context);
            const auto* current = runtime.motion_actor_;
            if (!current) throw std::runtime_error("motion query has no current actor");
            if (!runtime.combat_.trace_motion)
                throw std::runtime_error("actor motion requires Game posed scene queries");
            // All mover/gravity hull sweeps call 00424790 (scene7, pickups1,
            // excluded0). 0041bf00 instead uses a point query excluding its owner.
            const bool box = kind == CharacterTraceKind::actor_hull;
            const auto hit = runtime.combat_.trace_motion(start, direction, distance, hull,
                box ? 0 : current->entity, 0x200, 7, box, current->entity, box);
            Trace trace;
            trace.hit = hit.hit;
            trace.distance = hit.distance;
            trace.plane_distance = hit.plane_distance;
            trace.end = hit.position;
            trace.normal = hit.normal;
            trace.shader = hit.material;
            trace.material_name = trace.shader_name = hit.shader;
            trace.fraction = distance != 0 ? hit.distance / distance : 1;
            return trace;
        },
        [](void* context, Vec3 start, const Trace& hit) {
            auto& runtime = *static_cast<ActorRuntime*>(context);
            auto* current = runtime.motion_actor_;
            if (!current) throw std::runtime_error("landing has no current actor");
            runtime.land(*current, start, hit);
        }};
}

void ActorRuntime::jump(ActorState& actor, CollisionWorld* collision) {
    if (actor.posture_state != 0) return; // 004119b0 checks actor+3e0.
    if (!collision) throw std::runtime_error("actor jump requires loaded collision world");
    const auto hooks = motion_hooks(actor);
    struct Release {
        ActorState*& context;
        ~Release() { context = nullptr; }
    } release{motion_actor_};
    jump_character(actor, actor.jump_velocity, *collision, hooks);
}

void ActorRuntime::move(ActorState& actor, float, CollisionWorld* collision) {
    if (!collision) throw std::runtime_error("actor motion requires loaded collision world");
    actor.position_time_tick = static_cast<std::uint32_t>(time_);
    // 004179f0 restores current+22c to raw frame0 BEFORE controllers.
    if (actor.orientation[12] != actor.position.x ||
        actor.orientation[13] != actor.position.y || actor.orientation[14] != actor.position.z) {
        actor.orientation[12] = actor.position.x;
        actor.orientation[13] = actor.position.y;
        actor.orientation[14] = actor.position.z;
        if (combat_.frame_changed) combat_.frame_changed(actor.entity, true);
    }
    actor.previous_position = actor.position;
    const auto& before = actor.previous_position;
    bool moved;
    {
        const auto hooks = motion_hooks(actor);
        struct Release {
            ActorState*& context;
            ~Release() { context = nullptr; }
        } release{motion_actor_};
        moved = advance_character(actor,
            {actor.kind == ActorKind::player,
             actor.kind == ActorKind::player && combat_.camera_placement_bypass(),
             static_cast<std::uint32_t>(static_cast<std::uint64_t>(time_)), 1000},
            *collision, hooks);
    }
    actor.grounded = !actor.airborne;
    actor.crouched = actor.fully_crouched;
    actor.moving = actor.movement_speed != 0;
    // 004179f0: exact OR of both controllers, once, before weapon/animation work.
    if (moved && actor.kind == ActorKind::player) {
        if (!combat_.triggers) throw std::runtime_error("player motion requires Game trigger queries");
        combat_.triggers(before, actor.position, actor.hull);
    }
}

void ActorRuntime::update(float seconds, const GameInput& input, CollisionWorld* collision) {
    // 00432ec0 consumes Game's synchronized integral clock. Physics advances
    // fixed1/30 below; seconds must never advance the shared clock a second time.
    if (auto* selected = player()) update_player(*selected, seconds, input, collision);
    if (!cinematic_) update_ai(collision);
    // Registry insertion is push-front; deque storage stays append-only so VM
    // position aliases survive creation/removal. Reverse traversal is newest-first.
    for (std::size_t remaining = actors_.size(); remaining != 0;) {
        auto& actor = actors_[--remaining];
        if (actor.retired) continue;
        move(actor, seconds, collision);
        update_combat(actor);
        // 00432ec0 -> 0041e220(0), then 0041e0e0, AFTER the complete actor tick.
        actor.input_gas = false;
        actor.movement_commands = {};
    }
    *previous_input_ = input;
}


// Native actor checkpoint format: explicit little-endian fields, never object bytes.
// Included inside namespace pusu after the concrete ActorRuntime definitions.
struct ActorSaveWriter {
    std::ostream& stream;
    std::uint64_t remaining{64u * 1024u * 1024u};
    void bytes(const char* data, std::size_t size) {
        if (size > remaining) throw std::runtime_error("actor checkpoint exceeds size limit");
        remaining -= size;
        stream.write(data, static_cast<std::streamsize>(size));
        if (!stream) throw std::runtime_error("cannot write actor checkpoint");
    }
    void u8(std::uint8_t value) { const char byte = static_cast<char>(value); bytes(&byte, 1); }
    void u32(std::uint32_t value) {
        char data[4];
        for (unsigned i = 0; i != 4; ++i) data[i] = static_cast<char>(value >> (i * 8));
        bytes(data, sizeof(data));
    }
    void u64(std::uint64_t value) {
        char data[8];
        for (unsigned i = 0; i != 8; ++i) data[i] = static_cast<char>(value >> (i * 8));
        bytes(data, sizeof(data));
    }
    void integer(int value) { u32(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(value))); }
    void real(float value) {
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite actor checkpoint field");
        u32(std::bit_cast<std::uint32_t>(value));
    }
    void real(double value) {
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite actor checkpoint field");
        u64(std::bit_cast<std::uint64_t>(value));
    }
    void flag(bool value) { u8(value ? 1 : 0); }
    void count(std::size_t value, std::uint32_t maximum = 65536) {
        if (value > maximum) throw std::runtime_error("actor checkpoint collection exceeds limit");
        u32(static_cast<std::uint32_t>(value));
    }
    void text(const std::string& value) { count(value.size()); bytes(value.data(), value.size()); }
    void vector(Vec3 value) { real(value.x); real(value.y); real(value.z); }
};
struct ActorSaveReader {
    std::istream& stream;
    std::uint64_t remaining{64u * 1024u * 1024u};
    void bytes(char* data, std::size_t size) {
        if (size > remaining) throw std::runtime_error("actor checkpoint exceeds size limit");
        remaining -= size;
        stream.read(data, static_cast<std::streamsize>(size));
        if (!stream) throw std::runtime_error("truncated actor checkpoint");
    }
    std::uint8_t u8() { char byte{}; bytes(&byte, 1); return static_cast<std::uint8_t>(byte); }
    std::uint32_t u32() {
        char data[4]; bytes(data, sizeof(data));
        std::uint32_t value{};
        for (unsigned i = 0; i != 4; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << (i * 8);
        return value;
    }
    std::uint64_t u64() {
        char data[8]; bytes(data, sizeof(data));
        std::uint64_t value{};
        for (unsigned i = 0; i != 8; ++i) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])) << (i * 8);
        return value;
    }
    int integer() { return std::bit_cast<std::int32_t>(u32()); }
    float real() {
        const float value = std::bit_cast<float>(u32());
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite actor checkpoint field");
        return value;
    }
    double real64() {
        const double value = std::bit_cast<double>(u64());
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite actor checkpoint field");
        return value;
    }
    bool flag() {
        const auto value = u8();
        if (value > 1) throw std::runtime_error("invalid actor checkpoint boolean");
        return value != 0;
    }
    std::uint32_t count(std::uint32_t maximum = 65536) {
        const auto value = u32();
        if (value > maximum) throw std::runtime_error("actor checkpoint collection exceeds limit");
        return value;
    }
    std::string text() {
        const auto size = count();
        if (size > remaining) throw std::runtime_error("actor checkpoint string exceeds remaining budget");
        std::string value(size, '\0'); bytes(value.data(), size); return value;
    }
    Vec3 vector() { return {real(), real(), real()}; }
    WeaponKind weapon() {
        const auto value = u8();
        if (value > static_cast<unsigned>(WeaponKind::baba)) throw std::runtime_error("invalid actor checkpoint weapon");
        return static_cast<WeaponKind>(value);
    }
};

static void actor_save_input(ActorSaveWriter& w, const GameInput& input) {
    w.real(input.mouse_x); w.real(input.mouse_y);
    w.flag(input.forward_down); w.flag(input.backward_down); w.flag(input.left_down); w.flag(input.right_down);
    w.flag(input.fire); w.flag(input.use); w.flag(input.jump); w.flag(input.crouch);
    w.flag(input.walk); w.flag(input.reload); w.flag(input.change_weapon); w.flag(input.drop_weapon);
    w.flag(input.aim); w.flag(input.pause); w.flag(input.pistol); w.flag(input.rifle);
}
static GameInput actor_load_input(ActorSaveReader& r) {
    GameInput input;
    input.mouse_x = r.real(); input.mouse_y = r.real();
    input.forward_down = r.flag(); input.backward_down = r.flag(); input.left_down = r.flag(); input.right_down = r.flag();
    input.fire = r.flag(); input.use = r.flag(); input.jump = r.flag(); input.crouch = r.flag();
    input.walk = r.flag(); input.reload = r.flag(); input.change_weapon = r.flag(); input.drop_weapon = r.flag();
    input.aim = r.flag(); input.pause = r.flag(); input.pistol = r.flag(); input.rifle = r.flag();
    return input;
}

// These two methods deliberately have no native checkpoint prefix: this is the
// original playerstat block byte order (0042b220/0042d620), not a world checkpoint.
void ActorRuntime::save_player_stats(std::ostream& output, EntityHandle entity) const {
    const auto* actor = find(entity);
    if (!actor || actor->kind != ActorKind::player)
        throw std::runtime_error("playerstat target is not the player");
    const auto& a = *actor;
    if (a.selected_slot > 1 || a.ammunition_order.size() > 4 || a.inventory.size() > 255)
        throw std::runtime_error("playerstat inventory exceeds original format");
    std::array<bool, 5> seen{};
    for (const auto type : a.ammunition_order) {
        if (type == 0 || type > 4 || seen[type] || a.ammunition[type] <= 0)
            throw std::runtime_error("invalid playerstat reserve ordering");
        seen[type] = true;
    }
    for (unsigned type = 0; type != a.ammunition.size(); ++type)
        if (a.ammunition[type] < 0 || (a.ammunition[type] != 0 && !seen[type]))
            throw std::runtime_error("playerstat reserve missing from ordering");
    ActorSaveWriter w{output};
    w.real(a.health);
    w.u8(a.selected_slot);
    for (const auto kind : a.weapon_slots) {
        if (static_cast<unsigned>(kind) > static_cast<unsigned>(WeaponKind::baba))
            throw std::runtime_error("invalid playerstat weapon");
        const auto magazine = kind == WeaponKind::none ? 0 : a.weapons[static_cast<unsigned>(kind)].magazine;
        if (magazine < 0) throw std::runtime_error("invalid playerstat magazine");
        w.u8(static_cast<std::uint8_t>(kind));
        w.u32(static_cast<std::uint32_t>(magazine));
    }
    w.u8(static_cast<std::uint8_t>(a.ammunition_order.size()));
    for (const auto type : a.ammunition_order) {
        w.u8(type); w.u32(static_cast<std::uint32_t>(a.ammunition[type]));
    }
    w.u8(static_cast<std::uint8_t>(a.inventory.size()));
    for (const auto& item : a.inventory) w.text(item);
}

void ActorRuntime::load_player_stats(std::istream& input, EntityHandle entity) {
    auto* actor = find(entity);
    if (!actor || actor->kind != ActorKind::player)
        throw std::runtime_error("playerstat target is not the player");
    ActorSaveReader r{input};
    const float health = r.real();
    const auto selected = r.u8();
    if (selected > 1) throw std::runtime_error("invalid playerstat selected slot");
    auto weapons = actor->weapons;
    for (auto& weapon : weapons) { weapon.magazine = 0; weapon.owned = false; }
    std::array<WeaponKind, 2> slots{};
    constexpr std::array<int, 7> capacities{0, 10, 8, 20, 30, 25, 7};
    for (unsigned slot = 0; slot != slots.size(); ++slot) {
        const auto kind = r.weapon();
        const auto rounds = r.u32();
        const auto id = static_cast<unsigned>(kind);
        const unsigned original_slot = kind == WeaponKind::cz75 || kind == WeaponKind::magnum ? 0 : 1;
        if ((kind != WeaponKind::none && original_slot != slot) || rounds > static_cast<unsigned>(capacities[id]))
            throw std::runtime_error("invalid playerstat weapon slot or magazine");
        slots[slot] = kind;
        weapons[id].kind = kind;
        weapons[id].magazine = static_cast<int>(rounds);
        weapons[id].owned = kind != WeaponKind::none;
    }
    std::array<int, 5> ammunition{};
    std::vector<std::uint8_t> order;
    const auto reserve_count = r.u8();
    if (reserve_count > 4) throw std::runtime_error("invalid playerstat reserve count");
    order.reserve(reserve_count);
    for (unsigned i = 0; i != reserve_count; ++i) {
        const auto type = r.u8();
        const auto rounds = r.u32();
        if (type == 0 || type > 4 || ammunition[type] != 0 || rounds == 0 ||
            rounds > static_cast<unsigned>(std::numeric_limits<int>::max()))
            throw std::runtime_error("invalid playerstat reserve entry");
        ammunition[type] = static_cast<int>(rounds);
        order.push_back(type);
    }
    std::vector<std::string> inventory;
    const auto item_count = r.u8();
    inventory.reserve(item_count);
    for (unsigned i = 0; i != item_count; ++i) inventory.push_back(r.text());
    // The stats API consumes a standalone original .psv block, not a section
    // embedded in the native checkpoint. Reject trailing bytes before commit.
    if (input.peek() != std::char_traits<char>::eof() || input.bad())
        throw std::runtime_error("invalid trailing playerstat data");
    // No select()/grant_weapon(): restoring stats must not dispatch sounds,
    // particles, callbacks, or one-shot animation commands.
    actor->health = health;
    actor->weapon_slots = slots;
    actor->weapons = weapons;
    actor->selected_slot = selected;
    actor->weapon = slots[selected];
    actor->ammunition = ammunition;
    actor->ammunition_order.swap(order);
    actor->inventory.swap(inventory);
}

static void actor_save_motion(ActorSaveWriter& w, const CharacterMotionState& m) {
    w.vector(m.position); w.vector(m.movement_direction); w.vector(m.gravity_direction);
    w.vector(m.hull.minimum); w.vector(m.hull.maximum);
    w.real(m.walk_speed); w.real(m.run_speed); w.real(m.acceleration); w.real(m.gravity);
    w.real(m.movement_speed); w.real(m.vertical_speed);
    w.integer(m.movement_step); w.integer(m.gravity_step);
    for (const auto command : m.movement_commands) w.vector(command);
    for (const auto flag : m.movement_command_flags) w.flag(flag);
    w.flag(m.movement_walk); w.flag(m.input_gas); w.u8(m.crouch_phase);
    w.flag(m.airborne); w.flag(m.jumping); w.flag(m.crouching); w.flag(m.fully_crouched);
    w.integer(m.posture_state); w.u32(m.fall_time_tick);
}
static void actor_load_motion(ActorSaveReader& r, CharacterMotionState& m) {
    m.position = r.vector(); m.movement_direction = r.vector(); m.gravity_direction = r.vector();
    m.hull.minimum = r.vector(); m.hull.maximum = r.vector();
    m.walk_speed = r.real(); m.run_speed = r.real(); m.acceleration = r.real(); m.gravity = r.real();
    m.movement_speed = r.real(); m.vertical_speed = r.real();
    m.movement_step = r.integer(); m.gravity_step = r.integer();
    for (auto& command : m.movement_commands) command = r.vector();
    for (auto& flag : m.movement_command_flags) flag = r.flag();
    m.movement_walk = r.flag(); m.input_gas = r.flag(); m.crouch_phase = r.u8();
    m.airborne = r.flag(); m.jumping = r.flag(); m.crouching = r.flag(); m.fully_crouched = r.flag();
    m.posture_state = r.integer(); m.fall_time_tick = r.u32();
    if (m.hull.minimum.x > m.hull.maximum.x || m.hull.minimum.y > m.hull.maximum.y ||
        m.hull.minimum.z > m.hull.maximum.z)
        throw std::runtime_error("invalid actor checkpoint motion state");
}

static void actor_save_camera(ActorSaveWriter& w, const PlayerCameraState& c) {
    w.u8(c.special); w.flag(c.reverse); w.flag(c.near);
    w.real(c.desired_distance); w.real(c.special_rotation);
    w.real(c.pitch_anchor); w.real(c.pitch); w.real(c.distance_anchor); w.real(c.distance);
    w.vector(c.ray_origin);
    w.real(c.half_width); w.real(c.half_height); w.real(c.recovery_acceleration);
    w.real(c.recovery_time); w.real(c.death_roll_time);
    w.flag(c.recovering); w.flag(c.death_captured);
    w.flag(c.scope_visible); w.flag(c.scope_entry_completed);
    w.real(c.scope_fov); w.real(c.scope_rotation_anchor);
    w.flag(c.pitch_moved);
    w.flag(c.hit_effect_active); w.real(c.hit_effect_time); w.real(c.hit_effect_duration);
    for (const auto value : c.death_orientation) w.real(value);
    for (const auto value : c.world) w.real(value);
    w.real(c.fov);
}
static void actor_load_camera(ActorSaveReader& r, PlayerCameraState& c) {
    c.special = r.u8(); c.reverse = r.flag(); c.near = r.flag();
    c.desired_distance = r.real(); c.special_rotation = r.real();
    c.pitch_anchor = r.real(); c.pitch = r.real(); c.distance_anchor = r.real(); c.distance = r.real();
    c.ray_origin = r.vector();
    c.half_width = r.real(); c.half_height = r.real(); c.recovery_acceleration = r.real();
    c.recovery_time = r.real64(); c.death_roll_time = r.real64();
    c.recovering = r.flag(); c.death_captured = r.flag();
    c.scope_visible = r.flag(); c.scope_entry_completed = r.flag();
    c.scope_fov = r.real(); c.scope_rotation_anchor = r.real();
    c.pitch_moved = r.flag();
    c.hit_effect_active = r.flag(); c.hit_effect_time = r.real64(); c.hit_effect_duration = r.real();
    for (auto& value : c.death_orientation) value = r.real();
    for (auto& value : c.world) value = r.real();
    c.fov = r.real();
}
static void actor_save_mouse(ActorSaveWriter& w, const PlayerMouseState& m) {
    for (const auto sample : m.samples) { w.real(sample.x); w.real(sample.y); }
    w.u32(m.cursor); w.u32(m.idle_samples);
}
static PlayerMouseState actor_load_mouse(ActorSaveReader& r) {
    PlayerMouseState m;
    for (auto& sample : m.samples) { sample.x = r.real(); sample.y = r.real(); }
    m.cursor = r.u32(); m.idle_samples = r.u32();
    // Original unsigned counters wrap naturally; only array indexing uses % 10.
    return m;
}
static void actor_save_hit(ActorSaveWriter& w, const ActorHit& h) {
    // Borrowed mesh/shader views are synchronous query data, not the original
    // AI's retained geometry; keep the existing numeric checkpoint layout.
    w.flag(h.hit); w.u32(h.entity); w.vector(h.position); w.vector(h.normal);
    w.u32(h.owner); w.integer(h.actor_type);
    w.real(h.distance); w.real(h.plane_distance);
    w.integer(h.part); w.integer(h.material); w.integer(h.region);
}
static ActorHit actor_load_hit(ActorSaveReader& r) {
    ActorHit h;
    h.hit = r.flag(); h.entity = r.u32(); h.position = r.vector(); h.normal = r.vector();
    h.owner = r.u32(); h.actor_type = r.integer();
    h.distance = r.real(); h.plane_distance = r.real();
    h.part = r.integer(); h.material = r.integer(); h.region = r.integer();
    // AI's original stale trace caches consume only geometry, never a physical
    // instance or borrowed mesh/shader storage from the scene being replaced.
    h.mesh = {}; h.shader = {};
    return h;
}

static void actor_save_ai(ActorSaveWriter& w, const AiState& a) {
    w.real(a.reaction_min); w.real(a.reaction_max); w.real(a.radius);
    w.real(a.movement_min); w.real(a.movement_max); w.real(a.run_probability);
    w.real(a.last_attack_time); w.real(a.last_attack_probability); w.real(a.last_move_probability);
    w.real(a.dexterity); w.real(a.attack_ability);
    for (const auto value : a.attack_weights) w.real(value);
    w.flag(a.enabled); w.flag(a.registered); w.flag(a.movement_pending); w.flag(a.turned);
    w.flag(a.sound_played); w.flag(a.autostart); w.flag(a.last_seen_valid); w.flag(a.movement_decision); w.flag(a.acquisition);
    w.text(a.see_player_sound);
    w.vector(a.initial_position); w.vector(a.initial_forward); w.vector(a.last_seen_position); w.vector(a.target);
    for (const auto value : a.saved_orientation) w.real(value);
    for (const auto value : a.working_orientation) w.real(value);
    w.vector(a.obstacle_contact);
    w.flag(a.obstacle_following); w.flag(a.obstacle_side); w.flag(a.obstacle_side_valid);
    w.flag(a.obstacle_previous_side);
    w.integer(a.combat_state); w.integer(a.navigation_state); w.integer(a.route_cursor); w.integer(a.move_mode);
    w.real(a.last_seen_time); w.real(a.move_segment_time); w.real(a.move_decision_time); w.real(a.reaction_time);
    w.real(a.movement_auxiliary_time);
    w.real(a.dwell_time); w.real(a.search_time); w.real(a.move_duration); w.real(a.reaction_delay); w.flag(a.run);
    w.count(a.waypoints.size());
    for (const auto& waypoint : a.waypoints) {
        w.vector(waypoint.position); w.vector(waypoint.direction);
        w.real(waypoint.dwell_seconds); w.flag(waypoint.crouch); w.flag(waypoint.run);
    }
    w.count(a.route.size());
    for (const auto index : a.route) w.integer(index);
    w.count(a.path.size());
    for (const auto position : a.path) w.vector(position);
}
static void actor_load_ai(ActorSaveReader& r, AiState& a) {
    a.reaction_min = r.real(); a.reaction_max = r.real(); a.radius = r.real();
    a.movement_min = r.real(); a.movement_max = r.real(); a.run_probability = r.real();
    a.last_attack_time = r.real(); a.last_attack_probability = r.real(); a.last_move_probability = r.real();
    a.dexterity = r.real(); a.attack_ability = r.real();
    for (auto& value : a.attack_weights) value = r.real();
    a.enabled = r.flag(); a.registered = r.flag(); a.movement_pending = r.flag(); a.turned = r.flag();
    a.sound_played = r.flag(); a.autostart = r.flag(); a.last_seen_valid = r.flag(); a.movement_decision = r.flag(); a.acquisition = r.flag();
    a.see_player_sound = r.text();
    a.initial_position = r.vector(); a.initial_forward = r.vector(); a.last_seen_position = r.vector(); a.target = r.vector();
    for (auto& value : a.saved_orientation) value = r.real();
    for (auto& value : a.working_orientation) value = r.real();
    a.obstacle_contact = r.vector();
    a.obstacle_following = r.flag(); a.obstacle_side = r.flag(); a.obstacle_side_valid = r.flag();
    a.obstacle_previous_side = r.flag();
    a.combat_state = r.integer(); a.navigation_state = r.integer(); a.route_cursor = r.integer(); a.move_mode = r.integer();
    a.last_seen_time = r.real64(); a.move_segment_time = r.real64(); a.move_decision_time = r.real64(); a.reaction_time = r.real64();
    a.movement_auxiliary_time = r.real64();
    a.dwell_time = r.real64(); a.search_time = r.real64(); a.move_duration = r.real(); a.reaction_delay = r.real(); a.run = r.flag();
    const auto waypoint_count = r.count();
    a.waypoints.reserve(waypoint_count);
    for (unsigned i = 0; i != waypoint_count; ++i) {
        auto& waypoint = a.waypoints.emplace_back();
        waypoint.position = r.vector(); waypoint.direction = r.vector();
        waypoint.dwell_seconds = r.real(); waypoint.crouch = r.flag(); waypoint.run = r.flag();
    }
    const auto route_count = r.count();
    a.route.reserve(route_count);
    for (unsigned i = 0; i != route_count; ++i) {
        const auto index = r.integer();
        if (index < 0 || static_cast<unsigned>(index) >= waypoint_count)
            throw std::runtime_error("invalid actor checkpoint route index");
        a.route.push_back(index);
    }
    const auto path_count = r.count();
    for (unsigned i = 0; i != path_count; ++i) a.path.push_back(r.vector());
    if (a.combat_state < 0 || a.combat_state > 1 ||
        a.navigation_state < 0 || a.navigation_state > 11 ||
        a.move_mode < 0 || a.move_mode > 5 || a.route_cursor < 0)
        throw std::runtime_error("invalid actor checkpoint AI state");
}

static void actor_save_state(ActorSaveWriter& w, const ActorState& a) {
    actor_save_motion(w, a);
    w.u32(a.entity); w.u8(static_cast<std::uint8_t>(a.kind));
    w.vector(a.angles); w.vector(a.velocity);
    w.vector(a.previous_position); w.u32(a.position_time_tick);
    for (const float value : a.orientation) w.real(value);
    w.real(a.health);
    w.real(a.jump_velocity);
    for (const auto& weapon : a.weapons) {
        w.u8(static_cast<std::uint8_t>(weapon.kind)); w.integer(weapon.magazine);
        w.real(weapon.cooldown); w.real(weapon.reload_remaining); w.flag(weapon.owned);
    }
    for (const auto kind : a.weapon_slots) w.u8(static_cast<std::uint8_t>(kind));
    for (const int count : a.ammunition) w.integer(count);
    w.count(a.ammunition_order.size(), 4);
    for (const auto type : a.ammunition_order) w.u8(type);
    w.u8(a.selected_slot);
    w.count(a.inventory.size());
    for (const auto& item : a.inventory) w.text(item);
    w.u8(static_cast<std::uint8_t>(a.weapon));
    actor_save_ai(w, a.ai);
    w.integer(a.combat_state); w.integer(a.previous_combat_state); w.integer(a.shoot_flag);
    w.real(a.combat_time); w.real(a.shot_time); w.real(a.movement_action_time);
    w.real(a.shot_visual_time); w.integer(a.weapon_phase); w.flag(a.shot_light);
    w.u32(a.footstep_variant); w.flag(a.footstep_first); w.flag(a.footstep_second);
    w.flag(a.death_surface_sound);
    w.flag(a.no_ammo_latch); w.flag(a.reload_blocked); w.flag(a.movement_reload_blocked);
    w.integer(a.drop_ammunition); w.flag(a.drop_health);
    w.real(a.hit_pitch); w.real(a.hit_yaw); w.vector(a.hit_direction); w.integer(a.hit_region);
    w.real(a.hit_time);
    w.u8(static_cast<std::uint8_t>(a.hit_weapon)); w.flag(a.blood_enabled); w.real(a.shake_time);
    w.real(a.shake_duration); w.real(a.shake_yaw); w.real(a.shake_pitch);
    actor_save_camera(w, a.player_camera); w.flag(a.shake_active);
    w.text(a.name); w.text(a.animation); w.text(a.ai_name); w.text(a.on_death);
    w.flag(a.enabled); w.flag(a.alive); w.flag(a.grounded); w.flag(a.crouched);
    w.flag(a.attacking); w.flag(a.moving); w.flag(a.no_weapon); w.flag(a.retired);
    w.flag(a.trigger_held);
}
static void actor_load_state(ActorSaveReader& r, ActorState& a) {
    actor_load_motion(r, a);
    a.entity = r.u32();
    const auto kind = r.u8();
    if (kind > static_cast<unsigned>(ActorKind::bot)) throw std::runtime_error("invalid actor checkpoint kind");
    a.kind = static_cast<ActorKind>(kind);
    a.angles = r.vector(); a.velocity = r.vector();
    a.previous_position = r.vector(); a.position_time_tick = r.u32();
    for (float& value : a.orientation) value = r.real();
    a.health = r.real();
    a.jump_velocity = r.real();
    constexpr std::array<int, 7> capacities{0, 10, 8, 20, 30, 25, 7};
    for (unsigned id = 0; id != a.weapons.size(); ++id) {
        auto& weapon = a.weapons[id];
        weapon.kind = r.weapon(); weapon.magazine = r.integer();
        weapon.cooldown = r.real(); weapon.reload_remaining = r.real(); weapon.owned = r.flag();
        if (weapon.magazine < 0 || weapon.magazine > capacities[id])
            throw std::runtime_error("invalid actor checkpoint magazine");
    }
    for (unsigned index = 0; index != a.weapon_slots.size(); ++index) {
        auto& slot = a.weapon_slots[index];
        slot = r.weapon();
        const unsigned weapon_slot = slot == WeaponKind::cz75 || slot == WeaponKind::magnum ? 0 : 1;
        if (slot != WeaponKind::none && weapon_slot != index)
            throw std::runtime_error("invalid actor checkpoint weapon slot");
    }
    for (int& count : a.ammunition) {
        count = r.integer();
        if (count < 0) throw std::runtime_error("negative actor checkpoint reserve");
    }
    const auto reserve_count = r.count(4);
    a.ammunition_order.reserve(reserve_count);
    std::array<bool, 5> seen{};
    for (unsigned i = 0; i != reserve_count; ++i) {
        const auto type = r.u8();
        if (type == 0 || type > 4 || seen[type] || a.ammunition[type] == 0)
            throw std::runtime_error("invalid actor checkpoint reserve order");
        seen[type] = true; a.ammunition_order.push_back(type);
    }
    for (unsigned type = 0; type != a.ammunition.size(); ++type)
        if (a.ammunition[type] != 0 && !seen[type]) throw std::runtime_error("actor checkpoint unordered reserve");
    a.selected_slot = r.u8();
    if (a.selected_slot > 1) throw std::runtime_error("invalid actor checkpoint selected slot");
    const auto item_count = r.count();
    a.inventory.reserve(item_count);
    for (unsigned i = 0; i != item_count; ++i) a.inventory.push_back(r.text());
    a.weapon = r.weapon();
    actor_load_ai(r, a.ai);
    a.combat_state = r.integer(); a.previous_combat_state = r.integer(); a.shoot_flag = r.integer();
    a.combat_time = r.real64(); a.shot_time = r.real64(); a.movement_action_time = r.real64();
    a.shot_visual_time = r.real64(); a.weapon_phase = r.integer(); a.shot_light = r.flag();
    a.footstep_variant = r.u32(); a.footstep_first = r.flag(); a.footstep_second = r.flag();
    a.death_surface_sound = r.flag();
    a.no_ammo_latch = r.flag(); a.reload_blocked = r.flag(); a.movement_reload_blocked = r.flag();
    a.drop_ammunition = r.integer(); a.drop_health = r.flag();
    a.hit_pitch = r.real(); a.hit_yaw = r.real(); a.hit_direction = r.vector(); a.hit_region = r.integer();
    a.hit_time = r.real64();
    a.hit_weapon = r.weapon(); a.blood_enabled = r.flag(); a.shake_time = r.real64();
    a.shake_duration = r.real(); a.shake_yaw = r.real(); a.shake_pitch = r.real();
    actor_load_camera(r, a.player_camera); a.shake_active = r.flag();
    a.name = r.text(); a.animation = r.text(); a.ai_name = r.text(); a.on_death = r.text();
    a.enabled = r.flag(); a.alive = r.flag(); a.grounded = r.flag(); a.crouched = r.flag();
    a.attacking = r.flag(); a.moving = r.flag(); a.no_weapon = r.flag(); a.retired = r.flag();
    a.trigger_held = r.flag();
    if (a.entity == 0 || a.combat_state < 0 || a.combat_state > 7 ||
        a.previous_combat_state < 0 || a.previous_combat_state > 7 || a.shoot_flag < 0 || a.shoot_flag > 2 ||
        a.weapon_phase < 0 || a.weapon_phase > 2)
        throw std::runtime_error("invalid actor checkpoint state");
}

void ActorRuntime::save(std::ostream& output) const {
    ActorSaveWriter w{output};
    w.bytes("PUSUACTR", 8);
    w.u32(2);
    w.real(time_);
    w.u32(random_state_);
    w.u32(player_entity_);
    w.integer(ai_path_budget_); w.count(ai_cursor_);
    w.count(ai_iteration_);
    actor_save_mouse(w, mouse_state_);
    w.real(mouse_base_weight_); w.real(mouse_active_weight_);
    w.flag(cinematic_);
    w.flag(invert_mouse_);
    w.flag(player_processing_); w.flag(use_only_); w.flag(menu_visible_);
    w.flag(jump_latched_);
    actor_save_hit(w, ai_path_query_hit_); actor_save_hit(w, ai_path_block_hit_);
    w.flag(previous_input_ != nullptr);
    if (previous_input_) actor_save_input(w, *previous_input_);
    w.count(actors_.size());
    for (const auto& actor : actors_) actor_save_state(w, actor);
}

void ActorRuntime::load(std::istream& input) {
    ActorSaveReader r{input};
    char magic[8];
    r.bytes(magic, sizeof(magic));
    if (std::string_view(magic, sizeof(magic)) != "PUSUACTR" || r.u32() != 2)
        throw std::runtime_error("unsupported native actor checkpoint");
    const double time = r.real64();
    if (time < 0) throw std::runtime_error("negative actor checkpoint clock");
    const auto random_state = r.u32();
    const auto player_entity = r.u32();
    const auto path_budget = r.integer();
    const auto cursor = r.count();
    const auto iteration = r.count();
    if (path_budget < 0 || path_budget > 500) throw std::runtime_error("invalid actor checkpoint AI path budget");
    auto mouse = actor_load_mouse(r);
    const float mouse_base_weight = r.real(), mouse_active_weight = r.real();
    const bool cinematic = r.flag();
    const bool invert_mouse = r.flag();
    const bool player_processing = r.flag(), use_only = r.flag(), menu_visible = r.flag();
    const bool jump_latched = r.flag();
    const auto query_hit = actor_load_hit(r), block_hit = actor_load_hit(r);
    if (!r.flag()) throw std::runtime_error("actor checkpoint has no input history");
    const GameInput previous_input = actor_load_input(r);
    const auto count = r.count();
    if (cursor > count || iteration > count)
        throw std::runtime_error("invalid actor checkpoint AI iterator");
    std::deque<ActorState> actors;
    std::vector<EntityHandle> live;
    live.reserve(count);
    bool player_valid = player_entity == 0;
    for (unsigned i = 0; i != count; ++i) {
        actors.emplace_back();
        actor_load_state(r, actors.back());
        const auto& actor = actors.back();
        if (!actor.retired) live.push_back(actor.entity);
        if (actor.entity == player_entity && !actor.retired && actor.kind == ActorKind::player) player_valid = true;
    }
    if (!player_valid) throw std::runtime_error("invalid actor checkpoint player controller");
    if ((cursor < count && !actors[cursor].ai.registered) ||
        (iteration < count && !actors[iteration].ai.registered))
        throw std::runtime_error("actor checkpoint AI iterator is not a registered bot");
    std::sort(live.begin(), live.end());
    if (std::adjacent_find(live.begin(), live.end()) != live.end())
        throw std::runtime_error("duplicate active entity in actor checkpoint");
    auto previous = std::make_unique<GameInput>(previous_input);
    // Game releases all VM position bindings before full actor restore. Ordinary
    // actor append/removal still retains deque element addresses and tombstones.
    // All parsing/allocation above precedes this no-throw commit; no events fire.
    actors_.swap(actors);
    previous_input_.swap(previous);
    time_ = time;
    random_state_ = random_state;
    player_entity_ = player_entity;
    ai_path_budget_ = path_budget;
    ai_cursor_ = cursor;
    ai_iteration_ = static_cast<std::size_t>(iteration);
    mouse_state_ = mouse;
    mouse_base_weight_ = mouse_base_weight; mouse_active_weight_ = mouse_active_weight;
    cinematic_ = cinematic;
    invert_mouse_ = invert_mouse;
    player_processing_ = player_processing; use_only_ = use_only; menu_visible_ = menu_visible;
    jump_latched_ = jump_latched;
    ai_path_query_hit_ = query_hit; ai_path_block_hit_ = block_hit;
}

void ActorRuntime::swap_state(ActorRuntime& other) noexcept {
    actors_.swap(other.actors_);
    previous_input_.swap(other.previous_input_);
    std::swap(time_, other.time_);
    std::swap(random_state_, other.random_state_);
    std::swap(player_entity_, other.player_entity_);
    std::swap(ai_path_budget_, other.ai_path_budget_);
    std::swap(ai_cursor_, other.ai_cursor_);
    std::swap(ai_iteration_, other.ai_iteration_);
    std::swap(mouse_state_, other.mouse_state_);
    std::swap(mouse_base_weight_, other.mouse_base_weight_);
    std::swap(mouse_active_weight_, other.mouse_active_weight_);
    std::swap(cinematic_, other.cinematic_);
    std::swap(invert_mouse_, other.invert_mouse_);
    std::swap(player_processing_, other.player_processing_);
    std::swap(use_only_, other.use_only_);
    std::swap(menu_visible_, other.menu_visible_);
    std::swap(jump_latched_, other.jump_latched_);
    std::swap(ai_path_query_hit_, other.ai_path_query_hit_);
    std::swap(ai_path_block_hit_, other.ai_path_block_hit_);
}

} // namespace pusu
