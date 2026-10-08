#include "game_materials.hpp"
#include "game_effects.hpp"
#include "resources.hpp"
#include "scene_math.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <iomanip>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pusu {
namespace {
[[noreturn]] void material_error(std::string_view reason) {
    throw std::runtime_error("PhysicalMaterials: " + std::string(reason));
}
bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i != a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
float number(std::string_view token) {
    float value{};
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value))
        material_error("invalid numeric directive");
    return value;
}
// Original 00462fc0 is line-oriented; legacy Turkish comment bytes are not
// resource identifiers. AssetStore supplies protected text decoding when needed.
std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> result;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && static_cast<unsigned char>(line[i]) <= 32) ++i;
        if (i == line.size() || line.substr(i, 2) == "//") break;
        if (line[i] == '"') {
            const auto first = ++i;
            while (i < line.size() && line[i] != '"') ++i;
            if (i == line.size()) material_error("unterminated quoted directive");
            result.push_back(line.substr(first, i++ - first));
        } else {
            const auto first = i;
            while (i < line.size() && static_cast<unsigned char>(line[i]) > 32 && line.substr(i, 2) != "//") ++i;
            result.push_back(line.substr(first, i - first));
        }
    }
    return result;
}
enum class EffectKind : std::uint8_t { decal, particle, sound, footstep, jump, jump_up, die_fall };
struct Effect {
    EffectKind kind{};
    std::string name;
    float width{5}, height{5}, angle{1};
    bool reverse{}, normal{};
};
struct Block {
    unsigned kind{};
    std::string name;
    std::vector<Effect> always;
    std::array<std::vector<Effect>, 7> alternatives;
};
struct Definition { std::string name; std::vector<Block> blocks; };
std::vector<Definition> parse_materials(std::string_view text) {
    std::vector<Definition> definitions;
    Definition* definition = nullptr;
    Block* block = nullptr;
    unsigned depth = 0;
    bool inner_directive = false;
    for (std::size_t first = 0; first < text.size();) {
        const auto newline = text.find('\n', first);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        const auto line = tokens(text.substr(first, end - first));
        first = newline == std::string_view::npos ? text.size() : newline + 1;
        if (line.empty()) continue;
        const auto command = line.front();
        if (command == "{") {
            if (!definition || depth >= 2) material_error("unexpected material opening brace");
            ++depth;
            definition->blocks.emplace_back();
            block = &definition->blocks.back();
            block->kind = depth == 1 ? 2 : 0;
            continue;
        }
        if (command == "}") {
            if (!depth) material_error("unexpected material closing brace");
            --depth;
            block = depth ? &definition->blocks.front() : nullptr;
            if (!depth) definition = nullptr;
            continue;
        }
        if (depth == 0) {
            if (definition || line.size() != 1) material_error("invalid material name declaration");
            definitions.push_back({std::string(command), {}});
            definition = &definitions.back();
            inner_directive = false;
            continue;
        }
        if (line.size() < 2) material_error("missing material directive argument");
        if (depth == 1) {
            if (inner_directive) continue; // Original outer directives stop after inner content.
            EffectKind kind;
            if (same(command, "footstepsound")) kind = EffectKind::footstep;
            else if (same(command, "jumpsound")) kind = EffectKind::jump;
            else if (same(command, "jumpupsound")) kind = EffectKind::jump_up;
            else if (same(command, "diefallsound")) kind = EffectKind::die_fall;
            else material_error("unknown outer material directive: " + std::string(command));
            block->alternatives[static_cast<unsigned>(kind)].push_back({kind, std::string(line[1])});
            continue;
        }
        inner_directive = true;
        if (same(command, "type")) {
            if (same(line[1], "ammo")) block->kind = 0;
            else if (same(line[1], "particle")) block->kind = 1;
            else material_error("unknown material event category");
            continue;
        }
        if (same(command, "name")) { block->name = line[1]; continue; }
        Effect effect;
        effect.name = line[1];
        bool always = false;
        if (same(command, "hitdecal")) {
            effect.kind = EffectKind::decal;
            if (line.size() >= 4) {
                effect.width = number(line[2]); effect.height = number(line[3]);
                if (effect.width <= 0 || effect.height <= 0) material_error("nonpositive decal dimensions");
                for (std::size_t i = 4; i < line.size(); ++i) {
                    if (same(line[i], "always")) always = true;
                    else if (const float angle = number(line[i]); angle != 0) effect.angle = angle;
                }
            }
            if (effect.angle < .5f || effect.angle > float(std::numeric_limits<int>::max() / 2))
                material_error("invalid decal random-angle range");
        } else if (same(command, "hitparticle")) {
            effect.kind = EffectKind::particle;
            // 00462fc0 leaves its normal byte uninitialized when absent. The
            // native representation defines absent flags as false, not heap junk.
            for (std::size_t i = 2; i < line.size(); ++i) {
                if (same(line[i], "always")) always = true;
                else if (same(line[i], "reverse")) effect.reverse = true;
                else if (same(line[i], "normal")) effect.normal = true;
                else material_error("unknown particle impact flag");
            }
        } else if (same(command, "hitsound")) {
            effect.kind = EffectKind::sound;
            always = line.size() == 3 && same(line[2], "always");
        } else material_error("unknown inner material directive: " + std::string(command));
        if (always) block->always.push_back(std::move(effect));
        else block->alternatives[static_cast<unsigned>(effect.kind)].push_back(std::move(effect));
    }
    if (depth || definition) material_error("unterminated material definition");
    return definitions;
}
template<class Random,class Apply>
void dispatch(const Definition& definition, const PhysicalImpact& event, Random random, Apply apply) {
    for (const auto& block : definition.blocks) {
        // 004680a0 is strstr, not a case-insensitive registry lookup.
        if (block.kind != static_cast<unsigned>(event.kind) || event.name.find(block.name) == std::string_view::npos) continue;
        for (const auto& effect : block.always) apply(effect);
        for (unsigned kind = 0; kind != 3; ++kind) {
            const auto& choices = block.alternatives[kind];
            if (!choices.empty()) apply(choices[choices.size() - 1 - random() % choices.size()]);
        }
    }
}
struct ClipVertex { Vec3 position; std::array<float, 4> color{255,255,255,255}; };
struct Projection { Vec3 center, normal, u, v; float width{}, height{}, depth{2}; };
// 004249d0/00424f40: bounded convex clipping; a triangle clipped against six
// planes has at most nine vertices. No allocation during the clipping pass.
std::size_t clipped_triangle(std::array<ClipVertex, 12>& vertices, const Projection& p) {
    std::array<ClipVertex, 12> scratch;
    std::size_t count = 3;
    const std::array<Vec3,6> axes{p.u, p.u * -1, p.v, p.v * -1, p.normal, p.normal * -1};
    const std::array<float,6> distances{p.width*.5f,p.width*.5f,p.height*.5f,p.height*.5f,p.depth,p.depth};
    for (std::size_t plane = 0; plane != axes.size() && count; ++plane) {
        std::size_t next = 0;
        for (std::size_t i = 0; i != count; ++i) {
            const auto& a = vertices[i]; const auto& b = vertices[(i + 1) % count];
            const float da = dot(a.position - p.center, axes[plane]) - distances[plane];
            const float db = dot(b.position - p.center, axes[plane]) - distances[plane];
            const bool inside_a = da <= .001f, inside_b = db <= .001f;
            if (inside_a) scratch[next++] = a;
            if (inside_a != inside_b) {
                const float fraction = da / (da - db);
                auto& out = scratch[next++];
                out.position = a.position + (b.position - a.position) * fraction;
                for (unsigned c = 0; c != 3; ++c) out.color[c] = a.color[c] + (b.color[c] - a.color[c]) * fraction;
                out.color[3] = 255;
            }
        }
        count = next;
        std::copy_n(scratch.begin(), count, vertices.begin());
    }
    return count;
}
std::size_t clipped_facing_triangle(std::array<ClipVertex,12>& vertices, const Projection& projection,
                                  Vec3& normal) {
    normal=normalized(cross(vertices[2].position-vertices[0].position,vertices[1].position-vertices[0].position));
    return dot(normal,projection.normal)<.5f?0:clipped_triangle(vertices,projection);
}
struct Decal : ProjectedGeometry {
    std::string shader;
    std::uint32_t surface{};
};
void expand(Bounds& bounds,Vec3 position,bool first) {
    if (first) { bounds={position,position}; return; }
    auto& minimum=bounds.minimum; auto& maximum=bounds.maximum;
    minimum={std::min(minimum.x,position.x),std::min(minimum.y,position.y),std::min(minimum.z,position.z)};
    maximum={std::max(maximum.x,position.x),std::max(maximum.y,position.y),std::max(maximum.z,position.z)};
}
std::uint64_t geometry_generation() {
    // Single game/render thread; process-wide identity also covers restored
    // meshes and allocator address reuse after scene clear.
    static std::uint64_t generation{};
    return ++generation;
}
void append_polygon(ProjectedGeometry& decal, const std::array<ClipVertex,12>& vertices, std::size_t count,
                    const Projection& p, Vec3 face_normal) {
    const auto first = static_cast<std::uint16_t>(decal.mesh.positions.size());
    for (std::size_t i = 0; i != count; ++i) {
        expand(decal.mesh.bounds,vertices[i].position,decal.mesh.positions.empty());
        decal.mesh.positions.push_back(vertices[i].position);
        const auto offset = vertices[i].position - p.center;
        decal.mesh.texcoords.push_back({dot(offset, p.u) / p.width + .5f, dot(offset, p.v) / p.height + .5f});
        decal.mesh.normals.push_back(face_normal);
        std::array<std::uint8_t,4> color;
        for (unsigned c = 0; c != 4; ++c) color[c] = static_cast<std::uint8_t>(std::clamp(vertices[i].color[c],0.f,255.f));
        decal.colors.push_back(color);
    }
    for (std::uint16_t i = 2; i < count; ++i) {
        decal.mesh.indices.push_back(first); decal.mesh.indices.push_back(first + i - 1); decal.mesh.indices.push_back(first + i);
    }
    decal.generation=geometry_generation();
}
void clear_projection(WorldProjection& output) {
    for (std::size_t i=0;i<output.active_batches;++i) {
        auto& batch=output.batches[i];
        batch.mesh.positions.clear(); batch.mesh.texcoords.clear(); batch.mesh.normals.clear();
        batch.mesh.indices.clear(); batch.colors.clear();
        batch.mesh.bounds={};
        batch.generation=geometry_generation();
    }
    output.active_batches=0;
}
void append_transient_polygon(WorldProjection& output, const std::array<ClipVertex,12>& vertices,
                              std::size_t count, const Projection& projection, Vec3 normal) {
    if (!output.active_batches || output.batches[output.active_batches-1].mesh.positions.size()+count>65535) {
        if (output.active_batches==output.batches.size()) output.batches.emplace_back();
        output.batches[output.active_batches++].mesh.primitive=Primitive::triangles;
    }
    append_polygon(output.batches[output.active_batches-1],vertices,count,projection,normal);
}
} // namespace

struct PhysicalMaterials::State {
    AssetStore& assets;
    const MaterialLibrary& materials;
    Media& media;
    ParticleRuntime& particles;
    std::vector<Definition> definitions;
    const Level* level{};
    std::vector<std::uint32_t> visited_surfaces;
    std::vector<std::array<std::uint8_t,4>> world_colors;
    std::uint32_t projection_generation{};
    std::uint64_t resource_revision{};
    std::deque<Decal> decals;
    std::vector<RenderObject> objects;
    ParticleRandomSource random;
    bool environment{true}, dirty{};
    State(AssetStore& a, const MaterialLibrary& m, Media& s, ParticleRuntime& p) : assets(a), materials(m), media(s), particles(p) {
        for (const auto& name : assets.names("material")) {
            if (name.size() < 9 || !same(std::string_view(name).substr(name.size() - 9), ".material")) continue;
            auto parsed = parse_materials(assets.text(name));
            for (auto& definition : parsed) definitions.push_back(std::move(definition));
        }
    }
    std::uint32_t random_word() {
        if (!random.word) material_error("material effect requires the canonical game random source");
        return random.word(random.context);
    }
    const Definition* find(std::string_view name) const {
        if (const auto* shader = materials.find(name)) name = shader->physical_material_name;
        if (name.empty()) return nullptr;
        // 00446c20 prepends physical-name registrations: newest definition wins.
        for (auto it = definitions.rbegin(); it != definitions.rend(); ++it) if (same(it->name, name)) return &*it;
        return nullptr;
    }
    void views() {
        if (!dirty) return;
        objects.clear(); objects.reserve(decals.size());
        for (const auto& decal : decals) if (!decal.mesh.indices.empty()) {
            RenderObject object{&decal.mesh,decal.shader};
            object.vertex_colors=decal.colors; object.geometry_generation=decal.generation;
            objects.push_back(object);
        }
        dirty = false;
    }
    void triangle(std::array<ClipVertex,12>& vertices, const Projection& projection,
                  std::string_view shader, std::uint32_t surface, WorldProjection* output=nullptr) {
        Vec3 normal;
        const auto count=clipped_facing_triangle(vertices,projection,normal);
        if (count < 3) return;
        if (output) {
            append_transient_polygon(*output,vertices,count,projection,normal);
            return;
        }
        auto found = std::find_if(decals.rbegin(), decals.rend(), [&](const Decal& decal) {
            return decal.surface == surface && same(decal.shader, shader) && decal.mesh.positions.size() + count <= 65535;
        });
        if (found == decals.rend()) {
            decals.emplace_back();
            ++resource_revision;
            auto& decal = decals.back(); decal.shader = shader; decal.surface = surface; decal.mesh.primitive = Primitive::triangles;
            append_polygon(decal, vertices, count, projection, normal);
        } else append_polygon(*found, vertices, count, projection, normal);
        dirty = true;
    }
    void world_surface(std::uint32_t s,const Projection& projection,std::string_view shader,WorldProjection* output) {
        if (s>=level->surfaces.size()) material_error("decal leaf surface outside level");
        if (visited_surfaces[s]==projection_generation) return;
        visited_surfaces[s]=projection_generation;
        const auto& surface=level->surfaces[s];
        if (surface.type==SurfaceType::patch || static_cast<std::uint32_t>(surface.type)==4) return;
        for (std::size_t i=0;i+2<surface.index_count;i+=3) {
            std::array<ClipVertex,12> vertices;
            for (unsigned v=0;v!=3;++v) {
                const auto index=std::size_t(surface.first_index)+i+v;
                if (index>=level->indices.size()) material_error("world decal index outside level");
                const auto vertex=std::size_t(surface.first_vertex)+level->indices[index];
                if (vertex>=level->vertices.size()) material_error("world decal vertex outside level");
                vertices[v].position=level->vertices[vertex].position;
                const auto& color=world_colors[vertex];
                for (unsigned c=0;c!=4;++c) vertices[v].color[c]=color[c];
            }
            triangle(vertices,projection,shader,s,output);
        }
    }
    void world_node(std::int32_t index,const Bounds& query,const Projection& projection,
                    std::string_view shader,WorldProjection* output,std::size_t depth=0) {
        if (depth>level->nodes.size()) material_error("cyclic decal BSP");
        const auto overlaps=[&](const Bounds& b) {
            return b.maximum.x>=query.minimum.x && b.minimum.x<=query.maximum.x &&
                   b.maximum.y>=query.minimum.y && b.minimum.y<=query.maximum.y &&
                   b.maximum.z>=query.minimum.z && b.minimum.z<=query.maximum.z;
        };
        if (index<0) {
            const auto leaf_index=static_cast<std::uint32_t>(~index);
            if (leaf_index>=level->leaves.size()) material_error("decal BSP leaf outside level");
            const auto& leaf=level->leaves[leaf_index];
            if (!overlaps(leaf.bounds.bounds)) return;
            for (std::size_t i=0;i<leaf.surface_count;++i) {
                const auto ref=std::size_t(leaf.first_surface)+i;
                if (ref>=level->leaf_surfaces.size()) material_error("decal leaf range outside level");
                world_surface(level->leaf_surfaces[ref],projection,shader,output);
            }
        } else {
            if (std::size_t(index)>=level->nodes.size()) material_error("decal BSP node outside level");
            const auto& node=level->nodes[index];
            if (!overlaps(node.bounds.bounds)) return;
            world_node(node.children[0],query,projection,shader,output,depth+1);
            world_node(node.children[1],query,projection,shader,output,depth+1);
        }
    }
    void world_projection(const Projection& projection,std::string_view shader,WorldProjection* output=nullptr) {
        if (++projection_generation==0) {
            std::fill(visited_surfaces.begin(),visited_surfaces.end(),0);
            ++projection_generation;
        }
        // 004259a0 deliberately uses this same cube for depths 2 and 20.
        const float extent=std::max(projection.width,projection.height);
        const Vec3 radius{extent,extent,extent};
        const Bounds query{projection.center-radius,projection.center+radius};
        if (!level->nodes.empty()) world_node(0,query,projection,shader,output);
        else for (std::size_t leaf=0;leaf<level->leaves.size();++leaf)
            world_node(~static_cast<std::int32_t>(leaf),query,projection,shader,output);
    }
    void decal(const Effect& effect, const PhysicalImpact& event, Vec3 tangent) {
        if (!materials.find(effect.name)) material_error("decal graphical shader missing: " + effect.name);
        if (!level) material_error("decal impact requires loaded scene geometry");
        materials.construct(effect.name);
        const auto range = static_cast<std::uint32_t>(effect.angle * 2);
        const float angle = (float(random_word() % range) - effect.angle) * .017453292519943295f;
        tangent = tangent * std::cos(angle) + cross(event.normal, tangent) * std::sin(angle);
        const Projection projection{event.position + event.normal * 2, event.normal, tangent,
                                    cross(event.normal,tangent),effect.width,effect.height};
        world_projection(projection,effect.name);
    }
    void impact(const PhysicalImpact& event) {
        if (!finite(event.direction) || !finite(event.position) || !finite(event.normal)) material_error("nonfinite impact vectors");
        const auto* definition=find(event.material);
        if (!definition) return; // Original missing/empty physical binding has no effects.
        const auto first=normalized(cross(event.normal,normalized(Vec3{.1f,0,1})));
        const auto tangent=normalized(cross(first,event.normal));
        const std::array<Vec3,2> axes{tangent,normalized(cross(event.normal,tangent))};
        dispatch(*definition,event,[this] { return random_word(); },[&](const Effect& effect) {
            switch (effect.kind) {
            case EffectKind::decal: decal(effect,event,axes[0]); break;
            case EffectKind::particle: {
                const auto reflected=normalized(event.direction-event.normal*(2*dot(event.normal,event.direction)));
                const auto direction=effect.reverse?event.direction:effect.normal?event.normal:reflected;
                particles.create("hitparticle",effect.name,true,event.position+event.normal*2,std::nullopt,direction,axes);
                break;
            }
            case EffectKind::sound: {
                const auto volume=event.kind==PhysicalImpactKind::particle?
                    (event.collisions_left==3?160.f:event.collisions_left==2?80.f:15.f)/255.f:1.f;
                media.environment_sound("material",effect.name,"hit",volume,event.position+event.normal*2,true,environment,0,
                    event.kind==PhysicalImpactKind::particle?nullptr:event.previous_variant);
                break;
            }
            default: material_error("non-impact effect selected by impact dispatcher");
            }
        });
    }
    void sound(std::string_view material,Vec3 position,EffectKind kind,std::uint32_t* previous_variant=nullptr) {
        const auto* definition=find(material);
        if (!definition) return;
        for (const auto& block:definition->blocks) if (block.kind==2) {
            const auto& sounds=block.alternatives[static_cast<unsigned>(kind)];
            if (sounds.empty()) continue;
            const auto event=kind==EffectKind::footstep?"footstep":kind==EffectKind::jump?"jump":kind==EffectKind::jump_up?"jump_up":"die_fall";
            media.environment_sound(kind==EffectKind::die_fall?"bot":"material",sounds.back().name,event,1,position,true,environment,0,previous_variant);
        }
    }
};

PhysicalMaterials::PhysicalMaterials(AssetStore& a,const MaterialLibrary& m,Media& media,ParticleRuntime& p):state_(std::make_unique<State>(a,m,media,p)) {}
PhysicalMaterials::~PhysicalMaterials()=default;
void PhysicalMaterials::impact(std::string_view material,std::string_view event,Vec3 position,Vec3 direction,Vec3 normal,std::uint8_t budget) {
    impact(PhysicalImpact{budget?PhysicalImpactKind::particle:PhysicalImpactKind::ammunition,material,event,direction,position,normal,budget});
}
void PhysicalMaterials::impact(const ParticleImpact& event) {
    impact(PhysicalImpact{PhysicalImpactKind::particle,event.material,event.definition,event.direction,event.position,event.normal,event.collisions_left});
}
void PhysicalMaterials::impact(const PhysicalImpact& event) { state_->impact(event); }
void PhysicalMaterials::set_level(const Level* level) {
    clear(); state_->level=level; state_->projection_generation=0;
    state_->visited_surfaces.assign(level?level->surfaces.size():0,0);
    state_->world_colors.resize(level?level->vertices.size():0);
    if (level) for (std::size_t i=0;i<level->vertices.size();++i)
        state_->world_colors[i]=original_world_color(level->vertices[i].color);
}
void PhysicalMaterials::project_world(WorldProjection& output,Vec3 center,Vec3 normal,Vec3 u,
                                      float width,float height) {
    const auto batch_count=output.batches.size();
    const auto* batch_data=output.batches.data();
    clear_projection(output);
    if (!state_->level) material_error("world projection requires loaded scene geometry");
    if (!finite(center) || !finite(normal) || !finite(u) || !std::isfinite(width) ||
        !std::isfinite(height) || width<0 || height<0) material_error("invalid world projection");
    if (!width || !height) return;
    state_->world_projection(Projection{center,normal,u,cross(normal,u),width,height,20},{},&output);
    if (output.batches.size()!=batch_count || output.batches.data()!=batch_data) ++state_->resource_revision;
}
void PhysicalMaterials::set_random_source(ParticleRandomSource random) { state_->random=random; }
void PhysicalMaterials::footstep(std::string_view m,Vec3 p,Vec3,Vec3,std::uint32_t* previous) { state_->sound(m,p,EffectKind::footstep,previous); }
void PhysicalMaterials::jump(std::string_view m,Vec3 p,Vec3,Vec3,std::uint32_t* previous) { state_->sound(m,p,EffectKind::jump,previous); }
void PhysicalMaterials::jump_up(std::string_view m,Vec3 p) { state_->sound(m,p,EffectKind::jump_up); }
void PhysicalMaterials::fall(std::string_view m,Vec3 p,Vec3,Vec3,std::uint32_t* previous) { state_->sound(m,p,EffectKind::jump,previous); }
void PhysicalMaterials::death(std::string_view m,Vec3 p,Vec3,Vec3) { state_->sound(m,p,EffectKind::die_fall); }
void PhysicalMaterials::sound_environment(bool enabled) { state_->environment=enabled; }
void PhysicalMaterials::update(float seconds) {
    if (!std::isfinite(seconds) || seconds<0) material_error("invalid update duration");
    state_->views(); // Persistent original world decals do not have invented expiration.
}
void PhysicalMaterials::clear() {
    if (!state_->decals.empty()) ++state_->resource_revision;
    state_->decals.clear(); state_->objects.clear(); state_->dirty=false;
}
std::span<const RenderObject> PhysicalMaterials::objects() const noexcept { return state_->objects; }
std::uint64_t PhysicalMaterials::resource_revision() const noexcept { return state_->resource_revision; }
void PhysicalMaterials::save(std::ostream& output) const {
    output << "PUSUMATERIALS 2 " << unsigned(state_->environment) << ' ' << state_->decals.size() << '\n';
    const auto precision=output.precision(); output << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (const auto& decal:state_->decals) {
        output << std::quoted(decal.shader) << ' ' << decal.surface << ' ' << decal.mesh.positions.size() << ' ' << decal.mesh.indices.size() << '\n';
        for (std::size_t i=0;i<decal.mesh.positions.size();++i) {
            const auto p=decal.mesh.positions[i],n=decal.mesh.normals[i]; const auto uv=decal.mesh.texcoords[i];
            output << p.x << ' ' << p.y << ' ' << p.z << ' ' << n.x << ' ' << n.y << ' ' << n.z << ' ' << uv.x << ' ' << uv.y;
            for (auto c:decal.colors[i]) output << ' ' << unsigned(c);
            output << '\n';
        }
        for (auto index:decal.mesh.indices) output << index << ' ';
        output << '\n';
    }
    output.precision(precision);
    if (!output) material_error("saving decal state failed");
}
void PhysicalMaterials::load(std::istream& input) {
    std::string magic; unsigned version,environment; std::size_t count;
    if (!(input>>magic>>version>>environment>>count) || magic!="PUSUMATERIALS" || version!=2 || environment>1 || count>65536)
        material_error("invalid material save header");
    std::deque<Decal> decals;
    std::size_t total=0;
    for (std::size_t d=0;d<count;++d) {
        decals.emplace_back(); auto& decal=decals.back(); std::size_t vertices,indices;
        if (!(input>>std::quoted(decal.shader)>>decal.surface>>vertices>>indices) || !state_->materials.find(decal.shader) || vertices>65535 || indices>vertices*9 || indices%3 ||
            (decal.surface!=std::numeric_limits<std::uint32_t>::max() && (!state_->level || decal.surface>=state_->level->surfaces.size())))
            material_error("invalid saved decal geometry");
        total+=vertices; if (total>16777216) material_error("material save exceeds geometry limit");
        decal.mesh.primitive=Primitive::triangles;
        decal.mesh.positions.resize(vertices); decal.mesh.normals.resize(vertices); decal.mesh.texcoords.resize(vertices); decal.colors.resize(vertices); decal.mesh.indices.resize(indices);
        for (std::size_t i=0;i<vertices;++i) {
            auto& p=decal.mesh.positions[i]; auto& n=decal.mesh.normals[i]; auto& uv=decal.mesh.texcoords[i];
            if (!(input>>p.x>>p.y>>p.z>>n.x>>n.y>>n.z>>uv.x>>uv.y) || !finite(p) || !finite(n) || !std::isfinite(uv.x) || !std::isfinite(uv.y)) material_error("invalid saved decal vertex");
            expand(decal.mesh.bounds,p,i==0);
            for (auto& c:decal.colors[i]) { unsigned value; if (!(input>>value) || value>255) material_error("invalid saved decal color"); c=static_cast<std::uint8_t>(value); }
        }
        for (auto& index:decal.mesh.indices) { unsigned value; if (!(input>>value) || value>=vertices) material_error("invalid saved decal index"); index=static_cast<std::uint16_t>(value); }
        decal.generation=geometry_generation();
    }
    for (const auto& decal:decals) state_->materials.construct(decal.shader);
    if (!state_->decals.empty() || !decals.empty()) ++state_->resource_revision;
    state_->decals.swap(decals); state_->environment=environment!=0; state_->dirty=true; state_->views();
}
void PhysicalMaterials::swap_state(PhysicalMaterials& other) noexcept {
    using std::swap;
    if (!state_->decals.empty() || !other.state_->decals.empty()) {
        ++state_->resource_revision;
        ++other.state_->resource_revision;
    }
    state_->decals.swap(other.state_->decals); state_->objects.swap(other.state_->objects);
    swap(state_->environment,other.state_->environment);
    swap(state_->level,other.state_->level); swap(state_->dirty,other.state_->dirty);
    state_->visited_surfaces.swap(other.state_->visited_surfaces);
    state_->world_colors.swap(other.state_->world_colors);
    swap(state_->projection_generation,other.state_->projection_generation);
}

void physical_material_runtime_check() {
    const auto require=[](bool value) { if (!value) material_error("permanent material runtime check failed"); };
    const auto definitions=parse_materials(R"(mat_test
{
 footstepsound first
 footstepsound last
 jumpsound ground
 jumpupsound normal
 diefallsound concrete
 {
  type ammo
  hitparticle shards always normal
  hitparticle dust always reverse normal
  hitsound concrete always
  hitdecal decal_a 3 4 10
  hitdecal decal_b 5 6 0 20
 }
 {
  type particle
  name drop_shell
  hitsound shell_concrete
 }
 {
  type particle
  name drop_shell
  hitparticle extra always
 }
}
)");
    require(definitions.size()==1 && definitions[0].blocks.size()==4);
    const auto& noise=definitions[0].blocks[0];
    require(noise.alternatives[3].back().name=="last" && noise.alternatives[4].back().name=="ground");
    std::uint32_t random=1;
    std::size_t draws=0;
    const auto word=[&] {
        ++draws;
        random=random*214013u+2531011u;
        return (random>>16)&32767u;
    };
    std::vector<std::string> selected;
    dispatch(definitions[0],PhysicalImpact{PhysicalImpactKind::ammunition,"mat_test","bullet"},word,[&](const Effect& e) { selected.push_back(e.name); });
    require(draws==1 && selected.size()==4 && selected[0]=="shards" && selected[1]=="dust" && selected[2]=="concrete");
    selected.clear();
    dispatch(definitions[0],PhysicalImpact{PhysicalImpactKind::particle,"mat_test","particle/system/drop_shell_metal.txt"},word,[&](const Effect& e) { selected.push_back(e.name); });
    require(draws==2 && selected.size()==2 && selected[0]=="shell_concrete" && selected[1]=="extra");
    selected.clear();
    dispatch(definitions[0],PhysicalImpact{PhysicalImpactKind::particle,"mat_test","DROP_SHELL"},word,[&](const Effect& e) { selected.push_back(e.name); });
    require(draws==2 && selected.empty());
    require(definitions[0].blocks[1].alternatives[0][1].angle==20);
    std::array<ClipVertex,12> triangle;
    triangle[0].position={-4,-4,0}; triangle[1].position={0,4,0}; triangle[2].position={4,-4,0};
    const Projection projection{{0,0,2},{0,0,1},{1,0,0},{0,1,0},2,2};
    const auto count=clipped_triangle(triangle,projection);
    require(count>=3 && count<=9);
    Decal decal; append_polygon(decal,triangle,count,projection,{0,0,1});
    require(decal.mesh.indices.size()==(count-2)*3);
    for (std::size_t i=0;i<count;++i) require(std::abs(decal.mesh.positions[i].x)<=1.00001f && std::abs(decal.mesh.positions[i].y)<=1.00001f && decal.mesh.texcoords[i].x>=-.00001f && decal.mesh.texcoords[i].x<=1.00001f);
    const auto source=[](float z) {
        std::array<ClipVertex,12> vertices;
        vertices[0]={{-.5f,-.5f,z},{32,64,96,255}};
        vertices[1]={{0,.5f,z},{32,64,96,255}};
        vertices[2]={{.5f,-.5f,z},{32,64,96,255}};
        return vertices;
    };
    const Projection transient{{0,0,0},{0,0,1},{1,0,0},{0,1,0},2,2,20};
    auto shallow=transient; shallow.depth=2;
    Vec3 face_normal;
    auto vertices=source(10);
    require(clipped_facing_triangle(vertices,shallow,face_normal)==0);
    vertices=source(10);
    require(clipped_facing_triangle(vertices,transient,face_normal)==3);
    WorldProjection output;
    append_transient_polygon(output,vertices,3,transient,face_normal);
    require(output.active_batches==1 && output.batches[0].mesh.positions[0].z==10 &&
            output.batches[0].mesh.texcoords[0].x==.25f &&
            output.batches[0].colors[0]==std::array<std::uint8_t,4>{32,64,96,255});
    vertices=source(20.0005f);
    require(clipped_facing_triangle(vertices,transient,face_normal)==3);
    vertices=source(20.002f);
    require(clipped_facing_triangle(vertices,transient,face_normal)==0);
    vertices=source(-20.002f);
    require(clipped_facing_triangle(vertices,transient,face_normal)==0);
    vertices=source(0); std::swap(vertices[1],vertices[2]);
    require(clipped_facing_triangle(vertices,transient,face_normal)==0);
    const auto capacity=output.batches[0].mesh.positions.capacity();
    const auto generation=output.batches[0].generation;
    clear_projection(output);
    require(output.active_batches==0 && output.batches[0].mesh.positions.empty() &&
            output.batches[0].mesh.indices.empty() && output.batches[0].colors.empty() &&
            output.batches[0].mesh.positions.capacity()==capacity);
    append_transient_polygon(output,triangle,count,projection,{0,0,1});
    const auto& reused=output.batches[0];
    require(reused.generation>generation && reused.mesh.indices==decal.mesh.indices && reused.colors==decal.colors);
    for (std::size_t i=0;i<count;++i)
        require(reused.mesh.positions[i].x==decal.mesh.positions[i].x &&
                reused.mesh.positions[i].y==decal.mesh.positions[i].y &&
                reused.mesh.texcoords[i].x==decal.mesh.texcoords[i].x &&
                reused.mesh.texcoords[i].y==decal.mesh.texcoords[i].y);
    clear_projection(output);
    vertices=source(0);
    for (std::size_t i=0;i!=21846;++i) append_transient_polygon(output,vertices,3,transient,{0,0,1});
    require(output.active_batches==2 && output.batches[0].mesh.positions.size()==65535 &&
            output.batches[1].mesh.positions.size()==3 && output.batches[0].mesh.indices.back()==65534 &&
            output.batches[1].mesh.indices==std::vector<std::uint16_t>{0,1,2});
    const auto second_generation=output.batches[1].generation;
    const auto second_capacity=output.batches[1].mesh.positions.capacity();
    clear_projection(output);
    append_transient_polygon(output,vertices,3,transient,{0,0,1});
    require(output.active_batches==1 && output.batches.size()==2 && output.batches[1].mesh.positions.empty() &&
            output.batches[1].mesh.positions.capacity()==second_capacity &&
            output.batches[1].generation>=second_generation);
    bool rejected=false;
    try { (void)parse_materials("bad\n{\n{\nhitdecal missing 0 1\n}\n}\n"); } catch (const std::runtime_error&) { rejected=true; }
    require(rejected);
}
void physical_material_runtime_check(const AssetStore& assets) {
    physical_material_runtime_check();
    std::size_t files=0,definitions=0;
    for (const auto& name:assets.names("material")) {
        if (name.size()<9 || !same(std::string_view(name).substr(name.size()-9),".material")) continue;
        const auto parsed=parse_materials(assets.text(name));
        ++files; definitions+=parsed.size();
    }
    if (files!=17 || definitions!=41) material_error("original material census must contain 17 files and 41 definitions");
}
} // namespace pusu
