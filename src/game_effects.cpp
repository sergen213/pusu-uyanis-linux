#include "game_effects.hpp"
#include "resources.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <iostream>
#include <istream>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>

namespace pusu {
namespace {
std::string identity(std::string_view value) {
    std::string result(value);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
        if (c == '\\') c = '/';
    }
    return result;
}
std::string asset_name(std::string_view value, std::string_view prefix,
                       std::string_view suffix) {
    std::string name = identity(value);
    if (name.starts_with(prefix)) name.erase(0, prefix.size());
    if (name.ends_with(suffix)) name.resize(name.size() - suffix.size());
    return std::string(prefix) + name + std::string(suffix);
}

// Original line reader004641e0 and word splitter00463c60 retain quoted words.
// Views are used only while the owning definition text is alive.
template<class Function>
void authored_lines(std::string_view text, Function consume) {
    std::vector<std::string_view> words;
    words.reserve(8);
    while (!text.empty()) {
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        text = newline == text.npos ? std::string_view{} : text.substr(newline + 1);
        words.clear();
        while (!line.empty()) {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == line.npos) break;
            line.remove_prefix(first);
            if (line.starts_with("//") || line.starts_with(';')) break;
            if (line.front() == '"') {
                line.remove_prefix(1);
                const auto end = line.find('"');
                if (end == line.npos) throw std::runtime_error("Unterminated particle definition quote");
                words.push_back(line.substr(0, end));
                line.remove_prefix(end + 1);
            } else {
                const auto end = line.find_first_of(" \t\r");
                words.push_back(line.substr(0, end));
                if (end == line.npos) break;
                line.remove_prefix(end);
            }
        }
        if (!words.empty()) consume(std::span<const std::string_view>(words));
    }
}
float scalar(std::string_view word) {
    if (word.starts_with('+')) word.remove_prefix(1);
    float value{};
    const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
    if (parsed.ec != std::errc{} || !std::isfinite(value))
        throw std::runtime_error("Invalid particle numeric value: " + std::string(word));
    return value;
}
std::uint32_t original_integer(std::string_view word) {
    bool negative = false;
    if (!word.empty() && (word.front() == '-' || word.front() == '+')) {
        negative = word.front() == '-';
        word.remove_prefix(1);
    }
    std::uint32_t value{};
    //004682df is atoi: decimal accumulation, suffix ignored, DWORD wrap.
    for (char c : word) {
        if (c < '0' || c > '9') break;
        value = value * 10 + std::uint32_t(c - '0');
    }
    return negative ? 0u - value : value;
}
std::uint8_t byte_value(std::string_view word) {
    return static_cast<std::uint8_t>(original_integer(word));
}
Vec3 vector_value(std::span<const std::string_view> words) {
    return {scalar(words[1]), scalar(words[2]), scalar(words[3])};
}
struct ParticleConstruction {
    std::string name;
    bool object{};
};
struct ParticleMaterialConstruction {
    std::string name;
    std::uint32_t contents{};
};
struct ParticleDefinition {
    std::string material{"no_shader"}, object;
    std::vector<ParticleConstruction> constructions{{"no_shader", false}};
    Vec3 emitter{}, gravity{}, velocity{}, angular_velocity{};
    std::array<std::uint8_t, 4> start_color{255,0,0,255}, end_color{255,0,0,255};
    float elasticity{1}, size_modifier{1}, rate{15}, life{30}, life_variety{};
    float explosion{160}, color_duration{}, size_duration{}, size_variety{}, lookback{};
    std::int32_t count{250};
    std::uint8_t color_variety{}, start_size{5}, end_size{5};
    bool collide{}, reflect{}, particle_visibility{}, no_autosprite{}, sort{};
};
ParticleDefinition parse_definition(std::string_view text) {
    ParticleDefinition definition;
    authored_lines(text, [&](std::span<const std::string_view> words) {
        const auto key = identity(words[0]);
        if (key == "particle_vis") definition.particle_visibility = true;
        else if (key == "no_autosprite") definition.no_autosprite = true;
        else if (key == "sort") definition.sort = true;
        else if (key == "collision_options") {
            for (const auto option : words.subspan(1)) {
                const auto flag = identity(option);
                if (flag == "ps_collide_testcollide") definition.collide = true;
                else if (flag == "ps_collide_reflect") definition.reflect = true;
            }
        } else if (words.size() == 4) {
            if (key == "emitter_size") definition.emitter = vector_value(words);
            else if (key == "gravity") definition.gravity = vector_value(words);
            else if (key == "velocity") definition.velocity = vector_value(words);
            else if (key == "start_color" || key == "end_color") {
                auto& color = key == "start_color" ? definition.start_color : definition.end_color;
                for (std::size_t i = 0; i != 3; ++i) color[i] = byte_value(words[i + 1]);
            }
        } else if (words.size() == 2) {
            if (key == "shader") {
                definition.material = words[1];
                definition.constructions.push_back({definition.material, false});
            } else if (key == "object") {
                definition.object = words[1];
                definition.constructions.push_back({definition.object, true});
            } else if (key == "start_alpha") definition.start_color[3] = byte_value(words[1]);
            else if (key == "end_alpha") definition.end_color[3] = byte_value(words[1]);
            else if (key == "color_variety") definition.color_variety = byte_value(words[1]);
            else if (key == "start_size") definition.start_size = byte_value(words[1]);
            else if (key == "end_size") definition.end_size = byte_value(words[1]);
            else if (key == "particle_count") {
                definition.count = scalar(words[1]) > 2147483647.0
                    ? std::numeric_limits<std::int32_t>::max()
                    : static_cast<std::int32_t>(original_integer(words[1]));
            } else {
                float* target = nullptr;
                if (key == "elasticity") target = &definition.elasticity;
                else if (key == "size_modifier") target = &definition.size_modifier;
                else if (key == "birth_rate") target = &definition.rate;
                else if (key == "life") target = &definition.life;
                else if (key == "life_variety") target = &definition.life_variety;
                else if (key == "explosion") target = &definition.explosion;
                else if (key == "color_fade_speed") target = &definition.color_duration;
                else if (key == "size_fade_speed") target = &definition.size_duration;
                else if (key == "size_variety") target = &definition.size_variety;
                else if (key == "look_back_limit") target = &definition.lookback;
                else if (key == "rotx") target = &definition.angular_velocity.x;
                else if (key == "roty") target = &definition.angular_velocity.y;
                else if (key == "rotz") target = &definition.angular_velocity.z;
                if (target) *target = scalar(words[1]);
            }
        }
        //0044ca80 ignores unknown directives and wrong arities. In particular,
        //time_increment_amount is present in assets but has no original handler.
    });
    if (definition.count < 0 || definition.rate <= 0 || definition.life <= 0 ||
        definition.life_variety < 0 || definition.color_duration < 0 ||
        definition.size_duration < 0 || definition.lookback < 0)
        throw std::runtime_error("Invalid particle population or fade duration");
    if (definition.color_duration == 0) {
        definition.end_color = definition.start_color;
        definition.color_duration = 1;
    }
    if (definition.size_duration == 0) {
        definition.end_size = definition.start_size;
        definition.size_duration = 1;
    }
    if (!std::isfinite(1.0f / definition.rate) ||
        !std::isfinite(definition.life + definition.life_variety) ||
        !std::isfinite(3.0f / definition.color_duration) ||
        !std::isfinite((255.0f + std::abs(definition.size_variety)) / definition.size_duration) ||
        !std::isfinite(length(definition.velocity)) ||
        !std::isfinite(dot(definition.gravity, definition.gravity)))
        throw std::runtime_error("Particle definition arithmetic exceeds original finite range");
    if (definition.material.empty() || definition.material.size() >= 256 ||
        definition.object.size() >= 256 ||
        definition.material.find('\0') != definition.material.npos ||
        definition.object.find('\0') != definition.object.npos)
        throw std::runtime_error("Invalid particle material or object name");
    return definition;
}
std::size_t particle_capacity(const ParticleDefinition& definition) {
    //0044b930 uses single precision arithmetic followed by truncation.
    const float capacity = std::min(float(definition.count),
        (definition.life + definition.life_variety) * definition.rate + 5.0f);
    if (!std::isfinite(capacity) || capacity < 0 ||
        double(capacity) > double(std::numeric_limits<std::int32_t>::max() / 0x70))
        throw std::runtime_error("Particle pool exceeds original addressable allocation");
    return static_cast<std::size_t>(capacity);
}
struct ObjectParticlePart {
    const Mesh* mesh{};
    std::string material;
    Matrix local{render_identity};
};
struct ParticleTemplate {
    ParticleDefinition definition;
    std::vector<ObjectParticlePart> parts;
    std::string name;
    std::vector<ParticleMaterialConstruction> material_constructions;
};
float particle_dot(Vec3 a, Vec3 b) noexcept {
    //00444420 accumulates z,y,x in x87 before the caller rounds to binary32.
    return static_cast<float>((static_cast<long double>(a.z)*b.z +
        static_cast<long double>(a.y)*b.y) + static_cast<long double>(a.x)*b.x);
}
Matrix rotation_degrees(Vec3 angles) {
    Matrix result{render_identity};
    //0044db90 deliberately maps authored rotx/roty/rotz to Z/X/Y.
    rotate_local(result,{0,0,angles.x});
    rotate_local(result,{angles.y,0,0});
    rotate_local(result,{0,angles.z,0});
    return result;
}
std::string_view dropped_weapon_object(std::int32_t weapon_id) {
    //0044bf10 uses registry004776e0[weaponID], not the particle's authored object.
    static constexpr std::array<std::string_view,6> objects{
        "gun_pistol_cz75_dropped", "gun_pistol_desert_dropped",
        "gun_rifle_uzi_dropped", "gun_rifle_ak101_dropped",
        "gun_rifle_m4_dropped", "gun_shotgun_baba_dropped"};
    if (weapon_id < 1 || weapon_id > 6)
        throw std::runtime_error("Invalid original dropped-weapon ID");
    return objects[std::size_t(weapon_id - 1)];
}
struct Particle {
    Vec3 position{}, velocity{};
    float life{}, birth{}, updated{};
    std::array<float,4> color{}, end_color{}, color_rate{};
    float size{}, end_size{}, size_rate{};
    std::uint8_t collisions_left{3};
    Vec3 initial_angles{};
};
struct ParticleSystem {
    const ParticleTemplate* source{};
    std::string template_name;
    Vec3 position{}, offset{}, emitter{}, velocity{}, angular_velocity{};
    std::optional<std::array<Vec3,2>> plane_axes;
    std::optional<std::array<std::int32_t,5>> pickup;
    std::vector<Particle> live;
    std::int32_t remaining{};
    double next_birth{}, started{};
    float simulated{};
    std::uint8_t active{};
};
struct ParticleGroup {
    std::string name, definition, entity, attachment;
    std::vector<ParticleSystem> systems;
    bool visible{true};
};
float advance_toward(float value, float target, float rate, float seconds) {
    const float result = value + rate * seconds;
    return rate > 0 ? std::min(result, target) : std::max(result, target);
}
void advance_particle(Particle& particle, Vec3 gravity, float seconds) {
    for (std::size_t channel = 0; channel != 4; ++channel)
        particle.color[channel] = advance_toward(particle.color[channel],
            particle.end_color[channel], particle.color_rate[channel], seconds);
    particle.size = advance_toward(particle.size, particle.end_size, particle.size_rate, seconds);
    if (particle.collisions_left != 0) {
        particle.position = particle.position + particle.velocity * seconds;
        particle.velocity = particle.velocity + gravity * seconds;
        particle.position = particle.position + gravity * (seconds * seconds * .5f);
    }
}
std::uint32_t random_integer(std::uint32_t& state) {
    state = state * 214013u + 2531011u;
    return (state >> 16) & 32767u;
}
std::uint32_t random_integer(const ParticleRandomSource& source) {
    if (!source.word) throw std::runtime_error("Particle runtime requires the shared original random source");
    const auto value = source.word(source.context);
    if (value > 32767) throw std::runtime_error("Particle random source is not original MSVCRT rand");
    return value;
}
float variety(const ParticleRandomSource& state, float amount) {
    return amount == 0 ? 0 : (float(random_integer(state)) * 0.000030518509447574615f - .5f) * amount;
}
float color_variety(const ParticleRandomSource& state, std::uint8_t base, std::uint8_t amount) {
    const auto offset = amount == 0 ? 0 : static_cast<std::int8_t>(
        std::uint8_t(random_integer(state) % (std::uint32_t(amount) * 2) - amount));
    return float(int(base) + int(offset)) / 256.0f;
}
Particle spawn_particle(ParticleSystem& system, float birth, const ParticleRandomSource& random) {
    const auto& definition = system.source->definition;
    Particle particle;
    particle.birth = particle.updated = birth;
    particle.life = definition.life + variety(random, definition.life_variety);
    particle.position = {system.position.x + variety(random, system.emitter.x),
                         system.position.y + variety(random, system.emitter.y),
                         system.position.z + variety(random, system.emitter.z)};
    particle.velocity = {system.velocity.x + variety(random, definition.explosion),
                         system.velocity.y + variety(random, definition.explosion),
                         system.velocity.z + variety(random, definition.explosion)};
    //0044b310 randomizes all start channels before all end channels.
    for (std::size_t i = 0; i != 4; ++i)
        particle.color[i] = color_variety(random, definition.start_color[i], definition.color_variety);
    for (std::size_t i = 0; i != 4; ++i)
        particle.end_color[i] = color_variety(random, definition.end_color[i], definition.color_variety);
    for (std::size_t i = 0; i != 4; ++i)
        particle.color_rate[i] = (particle.end_color[i] - particle.color[i]) / definition.color_duration;
    particle.size = definition.start_size + variety(random, definition.size_variety);
    particle.end_size = definition.end_size + variety(random, definition.size_variety);
    particle.size_rate = (particle.end_size - particle.size) / definition.size_duration;
    particle.initial_angles = {variety(random, system.angular_velocity.x),
                               variety(random, system.angular_velocity.y),
                               variety(random, system.angular_velocity.z)};
    --system.remaining;
    return particle;
}
bool known_missing_template(std::string_view name) {
    return name == "particle/system/sys_ventilation_particle.txt" ||
           name == "particle/system/falling_water_01_particle_system.txt" ||
           name == "particle/system/sys_chimneys_01.txt";
}
void check_name(std::string_view name) {
    if (name.empty() || name.size() >= 256 || name.find('\0') != name.npos)
        throw std::runtime_error("Invalid original particle resource or instance name");
}
constexpr std::size_t snapshot_limit = 256u * 1024u * 1024u;
void save_integer(std::ostream& output, std::uint64_t value, std::size_t bytes = 4) {
    std::array<char,8> data{};
    for (std::size_t i = 0; i != bytes; ++i) data[i] = char(value >> (8 * i));
    output.write(data.data(), std::streamsize(bytes));
    if (!output) throw std::runtime_error("Cannot write particle checkpoint");
}
void save_float(std::ostream& output, float value) { save_integer(output, std::bit_cast<std::uint32_t>(value)); }
void save_double(std::ostream& output, double value) { save_integer(output, std::bit_cast<std::uint64_t>(value), 8); }
void save_vector(std::ostream& output, Vec3 value) {
    save_float(output, value.x); save_float(output, value.y); save_float(output, value.z);
}
void save_string(std::ostream& output, std::string_view value) {
    if (value.size() >= 256 || value.find('\0') != value.npos)
        throw std::runtime_error("Particle checkpoint name exceeds original buffer");
    save_integer(output, value.size());
    output.write(value.data(), std::streamsize(value.size()));
    if (!output) throw std::runtime_error("Cannot write particle checkpoint name");
}
struct ParticleCheckpointReader {
    std::istream& input;
    std::size_t bytes{}, allocated{};
    void read(char* destination, std::size_t amount) {
        if (amount > snapshot_limit - bytes) throw std::runtime_error("Particle checkpoint exceeds byte limit");
        input.read(destination, std::streamsize(amount));
        if (!input) throw std::runtime_error("Truncated particle checkpoint");
        bytes += amount;
    }
    void allocation(std::size_t amount) {
        if (amount > snapshot_limit - allocated)
            throw std::runtime_error("Particle checkpoint exceeds state memory limit");
        allocated += amount;
    }
    std::uint64_t integer(std::size_t size = 4) {
        std::array<char,8> data{};
        read(data.data(), size);
        std::uint64_t result{};
        for (std::size_t i = 0; i != size; ++i)
            result |= std::uint64_t(static_cast<unsigned char>(data[i])) << (8 * i);
        return result;
    }
    bool flag() {
        const auto value = integer();
        if (value > 1) throw std::runtime_error("Invalid particle checkpoint flag");
        return value != 0;
    }
    float scalar() {
        const float value = std::bit_cast<float>(std::uint32_t(integer()));
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite particle checkpoint scalar");
        return value;
    }
    double real() {
        const double value = std::bit_cast<double>(integer(8));
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite particle checkpoint clock");
        return value;
    }
    Vec3 vector() {
        const float x = std::bit_cast<float>(std::uint32_t(integer()));
        const float y = std::bit_cast<float>(std::uint32_t(integer()));
        const float z = std::bit_cast<float>(std::uint32_t(integer()));
        return {x,y,z};
    }
    std::string string() {
        const auto size = integer();
        if (size >= 256) throw std::runtime_error("Oversized particle checkpoint name");
        std::string result(std::size_t(size), '\0');
        read(result.data(), result.size());
        if (result.find('\0') != result.npos) throw std::runtime_error("Embedded NUL in particle checkpoint name");
        return result;
    }
};
void save_particle(std::ostream& output, const Particle& particle) {
    save_vector(output, particle.position); save_vector(output, particle.velocity);
    save_float(output, particle.life); save_float(output, particle.birth); save_float(output, particle.updated);
    for (float value : particle.color) save_float(output, value);
    for (float value : particle.end_color) save_float(output, value);
    for (float value : particle.color_rate) save_float(output, value);
    save_float(output, particle.size); save_float(output, particle.end_size); save_float(output, particle.size_rate);
    save_integer(output, particle.collisions_left);
    save_vector(output, particle.initial_angles);
}
Particle load_particle(ParticleCheckpointReader& input, const ParticleDefinition& definition, double now) {
    Particle particle;
    particle.position = input.vector(); particle.velocity = input.vector();
    particle.life = input.scalar(); particle.birth = input.scalar(); particle.updated = input.scalar();
    for (auto& value : particle.color) value = input.scalar();
    for (auto& value : particle.end_color) value = input.scalar();
    for (auto& value : particle.color_rate) value = input.scalar();
    particle.size = input.scalar(); particle.end_size = input.scalar(); particle.size_rate = input.scalar();
    const auto budget = input.integer();
    if (budget > 3) throw std::runtime_error("Invalid particle checkpoint collision budget");
    particle.collisions_left = std::uint8_t(budget);
    particle.initial_angles = input.vector();
    const float minimum_life = definition.life - definition.life_variety * .5f;
    const float maximum_life = definition.life + definition.life_variety * .5f;
    if (particle.life < minimum_life || particle.life > maximum_life ||
        particle.updated < particle.birth || double(particle.updated) > double(float(now)) ||
        double(particle.birth) > double(float(now)))
        throw std::runtime_error("Invalid particle checkpoint lifetime or update clock");
    return particle;
}
} // namespace

struct ParticleRuntime::State {
    State(AssetStore& store, const MaterialLibrary& library) : assets(store), materials(library) {}
    AssetStore& assets;
    const MaterialLibrary& materials;
    std::map<std::string, std::optional<Mesh>, std::less<>> meshes;
    std::map<std::string, ParticleTemplate, std::less<>> templates;
    std::map<std::string, std::vector<std::string>, std::less<>> definitions;
    std::vector<ParticleGroup> groups;
    std::vector<RenderParticle> particles;
    std::vector<RenderObject> objects;
    std::vector<ParticleImpact> impacts;
    std::vector<ParticleSettlement> settlements;
    std::map<std::string,bool,std::less<>> published_materials;
    std::map<const Mesh*,bool> published_meshes;
    std::uint64_t resource_revision{};
    std::map<std::string, std::string, std::less<>> restored_impact_names;
    double time{};
    ParticleRandomSource random;
    Vec3 camera_position{}, camera_right{1,0,0}, camera_up{0,1,0}, camera_forward{0,0,1};

    void load_object(std::string_view name, Matrix local,
                     std::vector<ObjectParticlePart>& parts, std::vector<std::string>& ancestors,
                     std::vector<ParticleMaterialConstruction>& constructions) {
        const auto path = asset_name(name, "object/po/", ".po");
        check_name(path);
        if (ancestors.size() >= 64) throw std::runtime_error("Object particle attachment depth exceeds limit");
        if (std::find(ancestors.begin(), ancestors.end(), path) != ancestors.end())
            throw std::runtime_error("Cyclic object particle attachment");
        if (!assets.contains_optional_game_file(path)) return;
        ancestors.push_back(path);
        const auto object = read_object(assets.path(path));
        const auto first_part = parts.size();
        for (const auto& part : object.parts) {
            materials.construct(part.shader, 0x10000);
            constructions.push_back({part.shader, 0x10000});
            const auto mesh_name = asset_name(part.mesh, "object/pm/", ".pm");
            auto found = meshes.find(mesh_name);
            if (found == meshes.end()) {
                found = meshes.emplace(mesh_name, read_runtime_mesh(assets, mesh_name)).first;
                if (!found->second) std::cerr << "PM yok: " << mesh_name << '\n';
            }
            const auto* mesh = found->second ? &*found->second : nullptr;
            parts.push_back({mesh, part.shader, local});
            if (mesh) published_meshes.try_emplace(mesh, false);
            published_materials.try_emplace(part.shader, false);
        }
        const auto end_part = parts.size();
        for (const auto& attachment : object.attachments) {
            Matrix child = attachment.transform.value_or(render_identity);
            if (attachment.bone) {
                bool matched = false;
                for (std::size_t i = first_part; i != end_part && !matched; ++i) {
                    if (!parts[i].mesh) continue;
                    for (const auto& bone : parts[i].mesh->bones) {
                        if (identity(bone.name) == identity(*attachment.bone)) {
                            child = multiply(bone.global_bind, child);
                            matched = true;
                            break;
                        }
                    }
                }
                if (!matched) throw std::runtime_error("Unknown object particle attachment bone");
            }
            load_object(attachment.object, multiply(local, child), parts, ancestors, constructions);
        }
        ancestors.pop_back();
    }
    const ParticleTemplate* load_template(const std::string& path) {
        check_name(path);
        if (!path.starts_with("particle/system/") || !path.ends_with(".txt"))
            throw std::runtime_error("Invalid particle template reference");
        auto found = templates.find(path);
        if (found != templates.end()) {
            for (const auto& construction : found->second.material_constructions)
                materials.construct(construction.name, construction.contents, 0, construction.contents != 0);
            return &found->second;
        }
        if (!assets.contains(path) && known_missing_template(path)) {
            std::cerr << "Particle system not found: " << path << '\n';
            return nullptr;
        }
        ParticleTemplate value{parse_definition(assets.text(path)), {}, path, {}};
        published_materials.try_emplace(value.definition.material, false);
        //0044c780 constructs no_shader before0044ca80 processes each directive,
        // including inline PO loads. Keep only concrete shader seeds for reuse.
        auto constructions = std::move(value.definition.constructions);
        for (auto& construction : constructions) {
            if (construction.object) {
                value.parts.clear();
                std::vector<std::string> ancestors;
                load_object(construction.name, render_identity, value.parts, ancestors,
                            value.material_constructions);
            } else {
                materials.construct(construction.name, 0, 0, false);
                value.material_constructions.push_back({std::move(construction.name), 0});
            }
        }
        return &templates.emplace(path, std::move(value)).first->second;
    }
    const std::vector<std::string>& load_definition(std::string_view name) {
        const auto path = asset_name(name, "particle/group/", ".txt");
        check_name(path);
        auto found = definitions.find(path);
        if (found != definitions.end()) return found->second;
        //0044d4f0 leaves genuinely absent groups empty; no template constructors run.
        if (!assets.contains_optional_game_file(path)) return definitions.try_emplace(path).first->second;
        std::vector<std::string> members;
        authored_lines(assets.text(path), [&](std::span<const std::string_view> words) {
            check_name(words[0]);
            auto member = identity(words[0]);
            if (const auto extension = member.find(".txt"); extension != member.npos)
                member.resize(extension);
            members.push_back(asset_name(member, "particle/system/", ".txt"));
            if (members.size() > 10) throw std::runtime_error("Particle group exceeds original ten systems");
        });
        return definitions.emplace(path, std::move(members)).first->second;
    }
    void reserve_outputs() {
        std::size_t sprites{}, models{};
        for (const auto& group : groups) for (const auto& system : group.systems) {
            if (system.source->definition.object.empty()) sprites += system.live.capacity();
            else models += system.live.capacity() * system.source->parts.size();
        }
        particles.reserve(sprites);
        objects.reserve(models);
        settlements.reserve(groups.size());
    }
    void create(std::string_view name, std::string_view definition, bool active,
                Vec3 position, std::optional<Vec3> emitter, std::optional<Vec3> direction,
                std::optional<std::array<Vec3,2>> axes) {
        check_name(name); check_name(definition);
        const auto& members = load_definition(definition);
        if (members.empty()) return;
        ParticleGroup group{identity(name), identity(definition), {}, {}, {}};
        for (const auto& member : members) {
            const auto* source = load_template(member);
            if (!source) continue;
            ParticleSystem system;
            system.source = source;
            system.template_name = member;
            system.position = system.offset = position;
            system.emitter = emitter.value_or(source->definition.emitter);
            system.velocity = direction
                ? *direction * original_sqrt(particle_dot(source->definition.velocity,source->definition.velocity))
                : source->definition.velocity;
            system.angular_velocity = source->definition.angular_velocity;
            if (source->definition.no_autosprite) system.plane_axes = axes;
            system.remaining = source->definition.count;
            system.next_birth = system.started = time;
            system.simulated = float(time);
            system.active = active ? 1 : 0;
            system.live.reserve(particle_capacity(source->definition));
            group.systems.push_back(std::move(system));
        }
        if (!group.systems.empty()) groups.push_back(std::move(group));
        reserve_outputs();
    }
    void rebuild();
    void simulate(ParticleSystem& system, float now, CollisionWorld& collision);
};
void ParticleRuntime::State::simulate(ParticleSystem& system, float now, CollisionWorld& collision) {
    const auto& definition = system.source->definition;
    const float interval = 1.0f / definition.rate;
    float origin = float(system.next_birth);
    const float window = std::min(definition.life + definition.life_variety, definition.lookback);
    system.simulated = now;
    if (system.next_birth < double(now - window) && system.remaining > 0) {
        const double missed = std::trunc((double(now - window) - system.next_birth) / interval + 1);
        const auto skipped = static_cast<std::int32_t>(std::min(missed, double(system.remaining)));
        system.remaining -= skipped;
        system.next_birth += double(skipped) * interval;
        origin = now - window;
    }
    for (std::size_t i = 0; i < system.live.size();) {
        if (system.live[i].life <= now - system.live[i].birth) {
            system.live[i] = system.live.back();
            system.live.pop_back();
        } else ++i;
    }
    if (system.next_birth < now && system.remaining > 0) {
        double relative = system.next_birth - origin;
        const double duration = double(now) - origin;
        while (relative < duration && system.remaining > 0) {
            const float birth = float(double(origin) + relative -
                float(random_integer(random)) * 0.000030518509447574615f * interval);
            if (system.live.size() == system.live.capacity())
                throw std::runtime_error("Original particle capacity exhausted");
            system.live.push_back(spawn_particle(system, birth, random));
            relative += interval;
        }
        system.next_birth = double(origin) + relative;
    }
    for (std::size_t i = 0; i < system.live.size();) {
        auto& particle = system.live[i];
        if (particle.life <= now - particle.birth) {
            particle = system.live.back();
            system.live.pop_back();
            continue;
        }
        const float elapsed = now - particle.updated;
        Vec3 start = particle.position;
        advance_particle(particle, definition.gravity, elapsed);
        if (definition.collide && particle.collisions_left != 0) {
            while (particle.collisions_left != 0) {
                const auto trace = collision.trace(start, particle.position, Bounds{}, 0x400);
                if (!trace.hit) break;
                const Vec3 movement = particle.position - start;
                const Vec3 direction = original_normalized(movement);
                if (!trace.material_name.empty())
                    impacts.push_back({system.source->name, trace.material_name, direction,
                        trace.end, trace.normal, particle.collisions_left});
                if (system.pickup) {
                    Vec3 position = particle.position;
                    position.z = trace.end.z + 50;
                    const auto floor = collision.trace(position, position + Vec3{0,0,-10000}, Bounds{}, 0x400);
                    position.z = (floor.hit ? floor.end.z : trace.end.z) + 49.084f - 32.7226676940918f;
                    //0044bf10 first reflects/decrements below, then replaces the
                    //throwing particle by a persistent dropped-weapon entity.
                    settlements.push_back({dropped_weapon_object((*system.pickup)[0]), position, *system.pickup});
                }
                float travelled_time{};
                //0044bf10 chooses the first nonzero displacement component,
                //using the already gravity-adjusted velocity for time recovery.
                if (particle.position.x != trace.end.x && particle.velocity.x != 0)
                    travelled_time = (trace.end.x - particle.position.x) / particle.velocity.x;
                else if (particle.position.y != trace.end.y && particle.velocity.y != 0)
                    travelled_time = (trace.end.y - particle.position.y) / particle.velocity.y;
                else if (particle.velocity.z != 0)
                    travelled_time = (trace.end.z - particle.position.z) / particle.velocity.z;
                const float speed_squared = particle_dot(particle.velocity, particle.velocity);
                if (speed_squared >= (particle_dot(definition.gravity, definition.gravity) + 1) * elapsed &&
                    trace.reflection > .01f && speed_squared > 0) {
                    Vec3 direction_after = original_normalized(particle.velocity);
                    direction_after = direction_after - trace.normal * (2 * particle_dot(trace.normal, direction_after));
                    direction_after = original_normalized(direction_after);
                    const float damping = .1f + (trace.reflection * 1.5f - .1f) *
                        float(random_integer(random)) * 0.000030518509447574615f;
                    particle.velocity = direction_after * (original_sqrt(speed_squared) * damping);
                } else {
                    particle.collisions_left = 1;
                    particle.velocity = {};
                }
                system.angular_velocity = {};
                --particle.collisions_left;
                start = trace.end;
                if (system.pickup) {
                    system.active = 2;
                    particle.position = trace.end;
                    break;
                }
                if (particle.collisions_left == 0) { particle.position = trace.end; break; }
                // The original helper recovers remaining travel backwards from
                // the endpoint; no second gravity/fade integration is applied.
                particle.position = start + particle.velocity * -travelled_time;
                // PS_COLLIDE_REFLECT is stored by0044ca80, but the actual
                // collision/update helpers never test it; both modes reflect.
            }
        }
        particle.updated = now;
        ++i;
    }
    if (system.live.empty() && system.remaining <= 0) system.active = 2;
}

void ParticleRuntime::update(float seconds, CollisionWorld& collision,
                             ParticleAttachmentLookup attachments) {
    if (!std::isfinite(seconds) || seconds < 0)
        throw std::runtime_error("Invalid particle update duration");
    state_->impacts.clear();
    state_->settlements.clear();
    state_->restored_impact_names.clear();
    std::erase_if(state_->groups, [](const ParticleGroup& group) {
        return std::all_of(group.systems.begin(), group.systems.end(),
            [](const ParticleSystem& system) { return system.active == 2; });
    });
    std::size_t impact_bound{};
    for (const auto& group : state_->groups) for (const auto& system : group.systems) {
        if (!system.source->definition.collide || system.active != 1) continue;
        const double elapsed = std::max(double(seconds), state_->time + seconds - system.simulated);
        const double births = std::min(double(system.remaining),
            std::ceil(elapsed * system.source->definition.rate) + 1);
        const double bound = (births + double(system.live.size())) * 3;
        if (bound > double(std::numeric_limits<std::size_t>::max() - impact_bound))
            throw std::runtime_error("Particle impact queue size overflow");
        impact_bound += static_cast<std::size_t>(bound);
    }
    state_->impacts.reserve(impact_bound);
    if (state_->time + seconds > std::numeric_limits<float>::max())
        throw std::runtime_error("Particle clock exceeds original floating-point range");
    state_->time += seconds;
    for (auto& group : state_->groups) {
        if (!group.entity.empty() && attachments.transform) {
            if (const auto matrix = attachments.transform(attachments.context, group.entity, group.attachment)) {
                const Vec3 translation{(*matrix)[12],(*matrix)[13],(*matrix)[14]};
                for (auto& system : group.systems) system.position = system.offset + translation;
            }
        }
        group.visible = std::any_of(group.systems.begin(), group.systems.end(), [&](const ParticleSystem& system) {
            return system.source->definition.particle_visibility ||
                   collision.visible(state_->camera_position, system.position);
        });
        if (!group.visible) continue;
        for (auto& system : group.systems) {
            if (system.active != 1) continue;
            const float now = float(state_->time);
            if (system.source->definition.collide) {
                constexpr float step = .025f; //0044ef30 global00477d90, NOT the ignored directive.
                const float begin = system.simulated;
                for (float increment = step; system.active == 1 && increment < now - begin; increment += step)
                    state_->simulate(system, begin + increment, collision);
            } else state_->simulate(system, now, collision);
        }
    }
    state_->rebuild();
}
void ParticleRuntime::State::rebuild() {
    particles.clear();
    std::size_t object_count{};
    bool object_topology_changed{};
    // Retain prior slot resources: fallback draw keys need preparation on
    // topology changes, while the renderer handles transform-only motion.
    // No original system directive creates dynamic lights. The authored
    // sys_gun_light is a sprite using particle_light_01, not a light source.
    for (auto& group : groups) for (auto& system : group.systems) {
        if (!group.visible || system.active == 2) continue;
        if (!system.live.empty()) {
            if (system.source->definition.object.empty()) {
                auto& published = published_materials.find(system.source->definition.material)->second;
                if (!published) { published = true; ++resource_revision; }
            } else for (const auto& part : system.source->parts) {
                if (!part.mesh) continue;
                auto& mesh = published_meshes.find(part.mesh)->second;
                auto& material = published_materials.find(part.material)->second;
                if (!mesh) { mesh = true; ++resource_revision; }
                if (!material) { material = true; ++resource_revision; }
            }
        }
        const auto& definition = system.source->definition;
        if (definition.sort) {
            std::sort(system.live.begin(), system.live.end(), [&](const Particle& a, const Particle& b) {
                const float a_depth = particle_dot(a.position - camera_position, camera_forward);
                const float b_depth = particle_dot(b.position - camera_position, camera_forward);
                if (std::isnan(a_depth) || std::isnan(b_depth))
                    return !std::isnan(a_depth) && std::isnan(b_depth);
                return a_depth > b_depth;
            });
        }
        for (std::size_t i = 0; i != system.live.size(); ++i) {
            const auto& particle = system.live[definition.sort ? i : system.live.size() - i - 1];
            if (!definition.object.empty()) {
                const float age = float(time) - particle.birth;
                Vec3 angles = particle.initial_angles;
                //0044db90 checks the initial angle, not the angular velocity.
                if (angles.x != 0) angles.x += age * system.angular_velocity.x;
                if (angles.y != 0) angles.y += age * system.angular_velocity.y;
                if (angles.z != 0) angles.z += age * system.angular_velocity.z;
                Matrix world = rotation_degrees(angles);
                world[12] = particle.position.x;
                world[13] = particle.position.y;
                world[14] = particle.position.z;
                for (const auto& part : system.source->parts) if (part.mesh) {
                    const RenderObject draw{part.mesh, part.material, multiply(world, part.local), {}, {1,1,1,1}, true};
                    if (object_count < objects.size()) {
                        auto& previous = objects[object_count];
                        if (previous.mesh != draw.mesh || previous.material != draw.material)
                            object_topology_changed = true;
                        previous = draw;
                    } else {
                        objects.push_back(draw);
                        object_topology_changed = true;
                    }
                    ++object_count;
                }
                continue;
            }
            Vec3 up = camera_up, right = camera_right;
            float stretch{};
            if (system.plane_axes) {
                up = (*system.plane_axes)[0];
                right = (*system.plane_axes)[1];
            } else if (definition.elasticity != 0) {
                const float speed = original_sqrt(particle_dot(particle.velocity,particle.velocity));
                if (speed != 0) {
                    up = particle.velocity * (1.f / speed);
                    const Vec3 transverse = cross(up, camera_forward * -1);
                    right = original_normalized(transverse);
                    stretch = speed * definition.elasticity;
                } else {
                    // Original zero-speed division produces an unrenderable
                    // triangle; keep it degenerate without generating NaNs.
                    up = right = {};
                }
            }
            const float radius = definition.size_modifier * particle.size * 1.414212942123413f;
            const float half = radius * .5f;
            const float along = half * 1.7320499420166016f + stretch;
            const Vec3 bias = right * (radius * .1f);
            RenderParticle draw;
            draw.position = particle.position;
            draw.size = {particle.size * definition.size_modifier, particle.size * definition.size_modifier};
            draw.color = particle.color;
            draw.material = definition.material;
            draw.autosprite = !system.plane_axes.has_value();
            draw.triangle = true;
            draw.triangle_positions = {
                particle.position - right * radius - bias,
                particle.position + up * along + right * half - bias,
                particle.position - up * along + right * half - bias};
            draw.triangle_uv = {Vec2{.5f,-.8660249710083008f},
                Vec2{-.5773500204086304f,1}, Vec2{1.5773500204086304f,1}};
            particles.push_back(draw);
        }
    }
    if (object_count != objects.size()) object_topology_changed = true;
    objects.resize(object_count);
    if (object_topology_changed) ++resource_revision;
}

ParticleRuntime::ParticleRuntime(AssetStore& assets, const MaterialLibrary& materials)
    : state_(std::make_unique<State>(assets, materials)) {}
ParticleRuntime::~ParticleRuntime() = default;
void ParticleRuntime::create(std::string_view name, std::string_view definition, bool active,
                             Vec3 position, Vec3 emitter_size) {
    create(name, definition, active, position, emitter_size, {}, {});
}
void ParticleRuntime::create(std::string_view name, std::string_view definition, bool active,
                             Vec3 position, std::optional<Vec3> emitter_size, std::optional<Vec3> direction,
                             std::optional<std::array<Vec3,2>> plane_axes) {
    state_->create(name, definition, active, position, emitter_size, direction, plane_axes);
}
void ParticleRuntime::create_dropped_weapon(std::string_view name, std::string_view definition,
                                          Vec3 position, Vec3 direction,
                                          std::array<std::int32_t,5> pickup) {
    if (pickup[0] < 1 || pickup[0] > 6 ||
        std::any_of(pickup.begin() + 1, pickup.end(), [](std::int32_t value) { return value < 0; }))
        throw std::runtime_error("Invalid original dropped-weapon payload");
    const auto before = state_->groups.size();
    state_->create(name, definition, true, position, Vec3{}, direction, {});
    if (state_->groups.size() == before) throw std::runtime_error("Missing dropped-weapon particle group");
    auto& system = state_->groups.back().systems.back();
    if (system.source->definition.object.empty() || system.source->definition.count != 1 ||
        !system.source->definition.collide) {
        state_->groups.pop_back();
        throw std::runtime_error("Dropped weapon requires the original single colliding object particle");
    }
    system.pickup = pickup;
}
void ParticleRuntime::start(std::string_view name) {
    const auto key = identity(name);
    for (auto& group : state_->groups) if (group.name == key)
        for (auto& system : group.systems) { system.active = 1; system.started = state_->time; }
}
void ParticleRuntime::stop(std::string_view name) {
    const auto key = identity(name);
    for (auto& group : state_->groups) if (group.name == key)
        for (auto& system : group.systems) system.active = 2;
    state_->rebuild();
}
void ParticleRuntime::attach(std::string_view name, std::string_view entity, std::string_view attachment) {
    check_name(entity);
    const auto key = identity(name);
    for (auto& group : state_->groups) if (group.name == key) {
        group.entity = identity(entity);
        group.attachment = identity(attachment);
    }
}
void ParticleRuntime::detach(std::string_view name) {
    const auto key = identity(name);
    for (auto& group : state_->groups) if (group.name == key) {
        group.entity.clear();
        group.attachment.clear();
        for (auto& system : group.systems) system.offset = system.position;
    }
}
void ParticleRuntime::clear() {
    state_->groups.clear();
    state_->particles.clear();
    state_->objects.clear();
    state_->impacts.clear();
    state_->settlements.clear();
    for (auto& [name,published] : state_->published_materials) published = false;
    for (auto& [mesh,published] : state_->published_meshes) published = false;
    ++state_->resource_revision;
    state_->restored_impact_names.clear();
}
void ParticleRuntime::save(std::ostream& output) const {
    save_integer(output, 0x32535050); // PPS2; shared actor RNG is persisted exactly once by its owner.
    save_double(output, state_->time);
    save_vector(output, state_->camera_position);
    save_vector(output, state_->camera_right);
    save_vector(output, state_->camera_up);
    save_vector(output, state_->camera_forward);
    save_integer(output, state_->groups.size());
    for (const auto& group : state_->groups) {
        save_string(output, group.name); save_string(output, group.definition);
        save_string(output, group.entity); save_string(output, group.attachment);
        save_integer(output, group.visible);
        save_integer(output, group.systems.size());
        for (const auto& system : group.systems) {
            save_string(output, system.template_name);
            save_vector(output, system.position); save_vector(output, system.offset);
            save_vector(output, system.emitter); save_vector(output, system.velocity);
            save_vector(output, system.angular_velocity);
            save_integer(output, system.plane_axes.has_value());
            if (system.plane_axes) {
                save_vector(output, (*system.plane_axes)[0]);
                save_vector(output, (*system.plane_axes)[1]);
            }
            save_integer(output, system.pickup.has_value());
            if (system.pickup) for (const auto value : *system.pickup)
                save_integer(output, std::uint32_t(value));
            save_integer(output, std::uint32_t(system.remaining));
            save_double(output, system.next_birth); save_double(output, system.started);
            save_float(output, system.simulated); save_integer(output, system.active);
            save_integer(output, system.live.size());
            for (const auto& particle : system.live) save_particle(output, particle);
        }
    }
    save_integer(output, state_->impacts.size());
    for (const auto& impact : state_->impacts) {
        save_string(output, impact.definition); save_string(output, impact.material);
        save_vector(output, impact.direction); save_vector(output, impact.position); save_vector(output, impact.normal);
        save_integer(output, impact.collisions_left);
    }
    save_integer(output, state_->settlements.size());
    for (const auto& settlement : state_->settlements) {
        save_string(output, settlement.object); save_vector(output, settlement.position);
        for (const auto value : settlement.pickup) save_integer(output, std::uint32_t(value));
    }
}
void ParticleRuntime::load(std::istream& input_stream) {
    ParticleCheckpointReader input{input_stream};
    if (input.integer() != 0x32535050) throw std::runtime_error("Unknown particle checkpoint format");
    auto restored = std::make_unique<State>(state_->assets, state_->materials);
    restored->time = input.real();
    if (restored->time < 0 || restored->time > std::numeric_limits<float>::max())
        throw std::runtime_error("Invalid particle checkpoint time");
    restored->random = state_->random;
    restored->camera_position = input.vector();
    restored->camera_right = input.vector();
    restored->camera_up = input.vector();
    restored->camera_forward = input.vector();
    const auto groups = input.integer();
    if (groups > snapshot_limit / sizeof(ParticleGroup))
        throw std::runtime_error("Oversized particle checkpoint group count");
    input.allocation(std::size_t(groups) * sizeof(ParticleGroup));
    restored->groups.reserve(std::size_t(groups));
    for (std::size_t g = 0; g != groups; ++g) {
        ParticleGroup group;
        group.name = input.string(); group.definition = input.string();
        group.entity = input.string(); group.attachment = input.string();
        check_name(group.name); check_name(group.definition);
        if (!group.entity.empty()) check_name(group.entity);
        if (!group.attachment.empty()) check_name(group.attachment);
        if (identity(group.name) != group.name || identity(group.entity) != group.entity ||
            identity(group.attachment) != group.attachment)
            throw std::runtime_error("Noncanonical particle checkpoint instance name");
        group.visible = input.flag();
        std::array<const ParticleTemplate*,10> sources{};
        std::size_t expected{};
        for (const auto& member : restored->load_definition(group.definition))
            if (const auto* source = restored->load_template(member)) sources[expected++] = source;
        const auto systems = input.integer();
        if (systems == 0 || systems != expected)
            throw std::runtime_error("Particle checkpoint no longer matches authored group");
        input.allocation(std::size_t(systems) * sizeof(ParticleSystem));
        group.systems.reserve(std::size_t(systems));
        for (std::size_t s = 0; s != systems; ++s) {
            ParticleSystem system;
            system.template_name = input.string();
            if (system.template_name != sources[s]->name)
                throw std::runtime_error("Particle checkpoint template order mismatch");
            system.source = sources[s];
            system.position = input.vector(); system.offset = input.vector();
            system.emitter = input.vector(); system.velocity = input.vector();
            system.angular_velocity = input.vector();
            if (input.flag()) {
                const Vec3 first = input.vector(), second = input.vector();
                if (!system.source->definition.no_autosprite)
                    throw std::runtime_error("Unexpected particle checkpoint plane axes");
                system.plane_axes = std::array<Vec3,2>{first,second};
            }
            if (input.flag()) {
                std::array<std::int32_t,5> pickup{};
                for (auto& value : pickup) {
                    const auto word = input.integer();
                    if (word > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
                        throw std::runtime_error("Invalid dropped-weapon checkpoint payload");
                    value = std::int32_t(word);
                }
                if (pickup[0] < 1 || pickup[0] > 6 ||
                    system.source->definition.object.empty() ||
                    system.source->definition.count != 1 || !system.source->definition.collide ||
                    s + 1 != systems)
                    throw std::runtime_error("Invalid dropped-weapon checkpoint system");
                system.pickup = pickup;
            }
            const auto remaining = input.integer();
            if (remaining > std::uint32_t(system.source->definition.count))
                throw std::runtime_error("Invalid particle checkpoint remaining population");
            system.remaining = std::int32_t(remaining);
            system.next_birth = input.real(); system.started = input.real();
            system.simulated = input.scalar();
            const auto active = input.integer();
            if (active > 2 || system.simulated < 0 ||
                system.simulated > float(restored->time) || system.started < 0 ||
                system.started > restored->time || system.next_birth < 0)
                throw std::runtime_error("Invalid particle checkpoint phase or clock");
            system.active = std::uint8_t(active);
            const auto live = input.integer();
            const auto capacity = particle_capacity(system.source->definition);
            if (live > capacity) throw std::runtime_error("Particle checkpoint exceeds authored pool");
            input.allocation(capacity * sizeof(Particle));
            system.live.reserve(capacity);
            for (std::size_t p = 0; p != live; ++p)
                system.live.push_back(load_particle(input, system.source->definition, restored->time));
            group.systems.push_back(std::move(system));
        }
        restored->groups.push_back(std::move(group));
    }
    const auto impacts = input.integer();
    if (impacts > snapshot_limit / sizeof(ParticleImpact))
        throw std::runtime_error("Oversized particle checkpoint impact count");
    input.allocation(std::size_t(impacts) * sizeof(ParticleImpact));
    restored->impacts.reserve(std::size_t(impacts));
    for (std::size_t i = 0; i != impacts; ++i) {
        const auto definition = input.string(), material = input.string();
        const auto* source = restored->load_template(definition);
        if (!source || material.empty()) throw std::runtime_error("Invalid particle checkpoint impact resource");
        const auto material_key = restored->restored_impact_names.emplace(material, material).first;
        ParticleImpact impact{source->name, material_key->first, input.vector(), input.vector(), input.vector(), 0};
        const auto budget = input.integer();
        if (budget < 1 || budget > 3) throw std::runtime_error("Invalid particle checkpoint impact budget");
        impact.collisions_left = std::uint8_t(budget);
        restored->impacts.push_back(impact);
    }
    const auto settlements = input.integer();
    if (settlements > groups) throw std::runtime_error("Oversized dropped-weapon settlement checkpoint");
    input.allocation(std::size_t(settlements) * sizeof(ParticleSettlement));
    restored->settlements.reserve(std::size_t(settlements));
    for (std::size_t i = 0; i != settlements; ++i) {
        const auto object = input.string();
        check_name(object);
        if (!restored->assets.contains(asset_name(object, "object/po/", ".po")))
            throw std::runtime_error("Missing dropped-weapon checkpoint object");
        const auto name = restored->restored_impact_names.emplace(object, object).first;
        ParticleSettlement settlement{name->first, input.vector(), {}};
        for (auto& value : settlement.pickup) {
            const auto word = input.integer();
            if (word > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
                throw std::runtime_error("Invalid settled-weapon checkpoint payload");
            value = std::int32_t(word);
        }
        const bool matches = std::any_of(restored->groups.begin(), restored->groups.end(),
            [&](const ParticleGroup& group) {
                return std::any_of(group.systems.begin(), group.systems.end(),
                    [&](const ParticleSystem& system) {
                        return system.active == 2 && system.pickup &&
                            *system.pickup == settlement.pickup &&
                            dropped_weapon_object((*system.pickup)[0]) == object;
                    });
            });
        if (!matches) throw std::runtime_error("Settled weapon does not match an authored dropped particle");
        if (settlement.pickup[0] < 1 || settlement.pickup[0] > 6)
            throw std::runtime_error("Invalid settled-weapon checkpoint ID");
        restored->settlements.push_back(settlement);
    }
    restored->reserve_outputs();
    restored->rebuild();
    restored->resource_revision += state_->resource_revision + 1;
    state_.swap(restored);
}
void ParticleRuntime::swap_state(ParticleRuntime& other) noexcept {
    if (this == &other) return;
    const auto revision = state_->resource_revision, other_revision = other.state_->resource_revision;
    const auto random = state_->random, other_random = other.state_->random;
    state_.swap(other.state_);
    state_->random = random;
    other.state_->random = other_random;
    state_->resource_revision = std::max(revision, state_->resource_revision) + 1;
    other.state_->resource_revision = std::max(other_revision, other.state_->resource_revision) + 1;
}
void ParticleRuntime::set_camera(const RenderCamera& camera) {
    const auto world = inverse_rigid(camera.view);
    state_->camera_position = camera.position;
    //004363a0/004363d0 supply negated camera X/Y. The original
    //triangle's long axis uses -X and its apex axis uses -Y.
    state_->camera_up = {-world[0],-world[1],-world[2]};
    state_->camera_right = {-world[4],-world[5],-world[6]};
    state_->camera_forward = {-world[8],-world[9],-world[10]};
}
std::span<const RenderParticle> ParticleRuntime::particles() const noexcept { return state_->particles; }
std::span<const RenderObject> ParticleRuntime::objects() const noexcept { return state_->objects; }
std::span<const ParticleImpact> ParticleRuntime::impacts() const noexcept { return state_->impacts; }
std::span<const ParticleSettlement> ParticleRuntime::settlements() const noexcept { return state_->settlements; }
std::uint64_t ParticleRuntime::resource_revision() const noexcept { return state_->resource_revision; }
void ParticleRuntime::set_random_source(ParticleRandomSource random) {
    if (!random.word) throw std::runtime_error("Missing shared particle random callback");
    state_->random = random;
}
void particle_runtime_check() {
    const auto expect = [](bool condition) {
        if (!condition) throw std::runtime_error("Original particle runtime invariant failed");
    };
    const auto close = [](float left, float right) { return std::abs(left - right) < 0.00001f; };
    const auto defaults = parse_definition("");
    expect(defaults.material == "no_shader" && defaults.count == 250 &&
        defaults.rate == 15 && defaults.life == 30 && defaults.explosion == 160 &&
        defaults.start_size == 5 && defaults.end_color == defaults.start_color);
    const auto constructors = parse_definition(
        "shader first\nobject prop\nshader second\nshader first\n");
    expect(constructors.constructions.size() == 5 &&
        constructors.constructions[0].name == "no_shader" &&
        !constructors.constructions[0].object &&
        constructors.constructions[1].name == "first" &&
        !constructors.constructions[1].object &&
        constructors.constructions[2].name == "prop" &&
        constructors.constructions[2].object &&
        constructors.constructions[3].name == "second" &&
        !constructors.constructions[3].object &&
        constructors.constructions[4].name == "first" &&
        !constructors.constructions[4].object &&
        constructors.material == "first" && constructors.object == "prop");
    const auto authored = parse_definition(
        "start_color 257 -1 7\nend_color 0 0 0\nstart_alpha 255\nend_alpha 0\n"
        "start_size .1\nend_size 20\ncolor_fade_speed 0\nsize_fade_speed 0\n"
        "collision_options PS_COLLIDE_TESTCOLLIDE PS_COLLIDE_REFLECT\n"
        "particle_vis\nno_autosprite\nsort\ntime_increment_amount 999\n");
    expect(authored.start_color == std::array<std::uint8_t,4>{1,255,7,255} &&
        authored.end_color == authored.start_color && authored.start_size == 0 &&
        authored.end_size == 0 && authored.collide && authored.reflect &&
        authored.particle_visibility && authored.no_autosprite && authored.sort);
    auto pool = defaults;
    pool.life = .9f; pool.rate = 3000; pool.count = 6000;
    expect(particle_capacity(pool) == 2705); // Actual sys_fireball_02 authored bound.
    expect(original_sqrt(std::bit_cast<float>(std::uint32_t{0x3f8000ff})) == 1);
    const auto rotate_z = rotation_degrees({90,0,0});
    expect(close(rotate_z[0],0) && close(rotate_z[1],1));
    std::uint32_t seed = 1;
    const ParticleRandomSource random{&seed,[](void* context) {
        return random_integer(*static_cast<std::uint32_t*>(context));
    }};
    expect(random_integer(random) == 41);
    expect(random_integer(random) == 18467);
    const auto unchanged = seed;
    expect(variety(random, 0) == 0 && seed == unchanged);
    seed = 1;
    expect(close(color_variety(random,255,200),352.0f / 256));
    ParticleTemplate source{parse_definition(
        "particle_count 4\nlife 2\nexplosion 0\nvelocity 2 0 1\n"
        "start_color 255 0 0\nend_color 0 0 255\n"
        "start_alpha 255\nend_alpha 0\ncolor_fade_speed 1\n"
        "start_size 4\nend_size 0\nsize_fade_speed 1\n"), {}, "check", {}};
    ParticleSystem system;
    system.source = &source; system.remaining = 4;
    system.position = {1,2,3}; system.velocity = source.definition.velocity;
    seed = 1;
    auto particle = spawn_particle(system,0,random);
    expect(system.remaining == 3 && seed == 1 && particle.collisions_left == 3 &&
        close(particle.color[0],255.0f / 256) && particle.size == 4);
    advance_particle(particle,{0,0,-4},.5f);
    expect(close(particle.position.x,2) && close(particle.position.z,3) &&
        particle.velocity.z == -1 && particle.size == 2 &&
        close(particle.color[3],255.0f / 512));
    advance_particle(particle,{0,0,-4},.5f);
    expect(close(particle.position.x,3) && close(particle.position.z,2) &&
        particle.velocity.z == -3 && particle.size == 0 && particle.color[3] == 0);
    particle.collisions_left = 0;
    advance_particle(particle,{0,0,-4},1);
    expect(particle.position.x == 3 && particle.position.z == 2 && particle.velocity.z == -3);
    particle.updated = 1;
    std::ostringstream saved(std::ios::binary);
    save_particle(saved,particle);
    auto record = saved.str();
    expect(record.size() == 0x70);
    std::istringstream valid(record,std::ios::binary);
    ParticleCheckpointReader valid_reader{valid};
    const auto restored = load_particle(valid_reader,source.definition,1);
    expect(restored.color == particle.color && restored.velocity.z == -3 &&
        restored.collisions_left == 0 && restored.updated == 1);
    const Vec3 raw_geometry{
        std::bit_cast<float>(std::uint32_t{0x7fc12345}),
        std::bit_cast<float>(std::uint32_t{0x7f800000}),
        std::bit_cast<float>(std::uint32_t{0xff800000})};
    std::ostringstream saved_geometry(std::ios::binary);
    save_vector(saved_geometry,raw_geometry);
    std::istringstream geometry_record(saved_geometry.str(),std::ios::binary);
    ParticleCheckpointReader geometry_reader{geometry_record};
    const auto restored_geometry = geometry_reader.vector();
    expect(std::bit_cast<std::uint32_t>(restored_geometry.x) == std::bit_cast<std::uint32_t>(raw_geometry.x) &&
        std::bit_cast<std::uint32_t>(restored_geometry.y) == std::bit_cast<std::uint32_t>(raw_geometry.y) &&
        std::bit_cast<std::uint32_t>(restored_geometry.z) == std::bit_cast<std::uint32_t>(raw_geometry.z));
    record[0x60] = 4;
    std::istringstream corrupt(record,std::ios::binary);
    ParticleCheckpointReader corrupt_reader{corrupt};
    bool rejected = false;
    try { (void)load_particle(corrupt_reader,source.definition,1); }
    catch (const std::runtime_error&) { rejected = true; }
    expect(rejected);
}
} // namespace pusu
