#include "game.hpp"
#include "interface.hpp"
#include "media.hpp"
#include "resources.hpp"

#include <algorithm>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pusu {
namespace {
char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a'-'A')) : c; }
bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}
std::string key(std::string_view name) {
    std::string result(name);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}
Vec3 matrix_position(const Matrix& m) { return {m[12],m[13],m[14]}; }
void matrix_position(Matrix& m, Vec3 p) { m[12]=p.x; m[13]=p.y; m[14]=p.z; }
std::vector<std::string> quoted_strings(std::string_view text) {
    std::vector<std::string> values;
    for(std::size_t start=0; (start=text.find('"',start))!=text.npos;) {
        const auto end=text.find('"',start+1);
        if(end==text.npos) throw std::runtime_error("Unterminated original language string");
        values.emplace_back(text.substr(start+1,end-start-1));start=end+1;
    }
    return values;
}
constexpr std::uint32_t native_interval_ms=33; // 0042da63: trunc(1000 * float(1/30)).
constexpr bool native_tick_due(std::uint32_t deadline,std::uint32_t now) { return deadline<now; }
constexpr std::array<std::string_view,7> native_guns{
    "no_weapon","gun_pistol_cz75","gun_pistol_desert","gun_rifle_uzi",
    "gun_rifle_ak101","gun_rifle_m4","gun_shotgun_baba"};
}


Game::Game(AssetStore& assets, const MaterialLibrary& materials, Settings& settings,
           Interface& interface, Media& media)
    : materials_(materials), assets_(assets), settings_(settings), interface_(interface),
      media_(media), actors_(assets,[this](const ActorEvent& event){on_actor_event(event);},
        [this] {
            CombatHooks hooks;
            hooks.attachment=[this](std::uint32_t id,std::string_view name){return attachment_world(id,name);};
            hooks.trace=[this](Vec3 a,Vec3 b,std::uint32_t id,std::uint32_t mask,std::uint32_t flags,bool pickups){
                return trace_scene(a,b,id,mask,flags,pickups);
            };
            hooks.has_attachment=[this](std::uint32_t id,std::string_view name){return has_attachment(id,name);};
            hooks.animation_exists=[this](std::uint32_t id,std::string_view name){
                return character_animation(entity(id).model,name)!=nullptr;
            };
            hooks.animation_controller=[this](std::uint32_t id,int channel){
                return animation_controller(id,channel);
            };
            hooks.animation_surface_sound=[this](std::uint32_t,bool death,bool positional,Vec3 position,
                                                 const ActorHit& hit,std::uint32_t* previous){
                const auto origin=positional?position:Vec3{};
                if(death) physical_materials_.death(hit.shader,origin,{},hit.normal);
                else physical_materials_.footstep(hit.shader,origin,{},hit.normal,previous);
            };
            hooks.trace_motion=[this](Vec3 start,Vec3 direction,float distance,Bounds hull,
                    std::uint32_t source,std::uint32_t mask,std::uint32_t flags,
                    bool pickups,std::uint32_t recipient,bool box){
                return trace_motion(start,direction,distance,hull,source,mask,flags,pickups,recipient,box);
            };
            hooks.camera=[this]{return camera_.frame0;};
            hooks.camera_fov=[this]{return camera_.reference_fov_degrees>0?
                camera_.reference_fov_degrees:settings_.reference_fov;};
            hooks.set_camera_fov=[this](float value){
                camera_fov_=value;camera_.reference_fov_degrees=value;
                const auto id=selected_camera_?selected_camera_:main_camera_;
                if(id) entity(id).camera_fov=value;
            };
            // 00440330 is camera+148: near4, not the separate culling distance.
            hooks.camera_view_distance=[this]{return camera_.near_plane;};
            hooks.viewport_width=[this]{return viewport_width_;};
            hooks.bot_damage_blocked=[]{return false;}; // Original console ordinal46 defaults off.
            hooks.camera_placement_bypass=[]{return false;}; // Ordinal43 defaults off.
            hooks.camera_update_blocked=[this]{return camera_blocked_;};
            hooks.damage=[this](const ActorHit& hit,float damage,std::uint32_t source){
                if(hit.entity) damage_entity(hit.entity,damage,source,hit);
            };
            hooks.corpse=[this](std::uint32_t id){configure_corpse(entity(id));};
            hooks.impact=[this](const ActorHit& hit,Vec3 incoming,int){
                // 004174e2 supplies an empty event name for every ammunition type.
                const PhysicalImpact impact{PhysicalImpactKind::ammunition,hit.shader,"",
                    incoming,hit.position,hit.normal,0,nullptr};
                // 004174e2 forwards +4c shader identity;004259a0 projects BSP world surfaces only.
                physical_materials_.impact(impact);
            };
            hooks.land_sound=[this](std::uint32_t,bool fallen,Vec3 position,const Trace& hit){
                const auto material=hit.shader_name.empty()?hit.material_name:hit.shader_name;
                if(fallen) physical_materials_.fall(material,position,{},hit.normal);
                else physical_materials_.jump(material,position,{},hit.normal);
            };
            hooks.triggers=[this](Vec3 previous,Vec3 current,Bounds hull){trace_triggers(previous,current,hull);};
            hooks.material_action=[this](std::uint32_t,Vec3 position,const ActorHit& hit,std::string_view event){
                if(event=="jump_up") physical_materials_.jump_up(hit.shader,position);
                else physical_materials_.impact(hit.shader,event,position,{},hit.normal);
            };
            hooks.frame_changed=[this](std::uint32_t id,bool relink){
                // Native mode0 dirties a lazy model matrix; ours is evaluated
                // at use. Only mode1 notifies the BSP membership list.
                if(!relink) return;
                if(const auto* actor=actors_.find(id)) {
                    auto& e=entity(id);e.orientation=actor->orientation;
                    e.position=matrix_position(e.orientation);refresh_membership(e);
                }
            };
            return hooks;
        }()), effects_(assets,materials), physical_materials_(assets,materials,media,effects_) {
    const ParticleRandomSource random{this,[](void* context){
        return static_cast<Game*>(context)->actors_.random_word();
    }};
    effects_.set_random_source(random);physical_materials_.set_random_source(random);
    camera_fov_=settings_.reference_fov;
    viewport_width_=static_cast<unsigned>(settings_.width);
    viewport_height_=static_cast<unsigned>(settings_.height);
    camera_transition_tick_=game_tick_-1000u;
}
Game::~Game() { release_position_bindings(); }
void Game::warn_missing_call(std::string_view name) noexcept {
    // The original 004776a8 literal and console font use CP1254, not UTF-8.
    warn("WARNING: Fonksiyon bulunamad\xfd: ",name);
}
void Game::warn(std::string_view prefix,std::string_view name) noexcept {
    try {
        // 00438ce0 -> 00447520: severity2, console+file, unchanged VM operands.
        // Resource diagnostics use the same severity2 console+file pipeline.
        std::string line;
        line.reserve(prefix.size()+name.size());
        line.append(prefix).append(name);
        interface_.console_warning(line,game_tick_);
        if(!warning_log_.is_open()) {
            const auto path=user_data_directory()/"log.txt";
            std::filesystem::create_directories(path.parent_path());
            warning_log_.clear();
            warning_log_.open(path,std::ios::app|std::ios::binary);
        }
        warning_log_<<line<<'\n';
        warning_log_.flush();
        if(!warning_log_) {
            std::fputs("Pusu: cannot append or flush user-data log.txt\n",stderr);
            warning_log_.close();
        }
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Pusu: warning output failed: %s\n",error.what());
    } catch(...) {
        std::fputs("Pusu: warning output failed\n",stderr);
    }
}
void Game::release_position_bindings() {
    for(auto& e:entities_) if(e.position_address) {
        runtime_.unbind_words(e.position_address); e.position_address=0;
    }
}
Game::Entity& Game::entity(std::uint32_t handle) {
    if(!handle || handle>entities_.size() || !entities_[handle-1].alive)
        throw script::Error("Invalid native entity handle "+std::to_string(handle));
    return entities_[handle-1];
}
const Game::Entity& Game::entity(std::uint32_t handle) const {
    if(!handle || handle>entities_.size() || !entities_[handle-1].alive)
        throw script::Error("Invalid native entity handle "+std::to_string(handle));
    return entities_[handle-1];
}
std::uint32_t Game::create_entity(std::string name,int kind) {
    if(entities_.size()>=std::numeric_limits<std::uint32_t>::max()-1)
        throw std::runtime_error("Native entity address space exhausted");
    entities_.emplace_back();auto& e=entities_.back();
    e.handle=static_cast<std::uint32_t>(entities_.size());e.name=std::move(name);e.kind=kind;
    // 0040c1c0 -> 00440160 seeds camera FRAME0 via00436620, not identity.
    if(kind==3) e.orientation=original_actor_basis;
    next_entity_=e.handle+1;
    prepare_membership();
    return e.handle;
}
std::uint32_t Game::host_create_entity(std::string name,int kind,std::string definition) {
    const auto handle=create_entity(std::move(name),kind);
    auto& e=entity(handle);
    if(!definition.empty()) set_mesh(e,definition);
    if(kind==1 || kind==2) {
        if(!e.model.definition) throw script::Error("Actor requires an original PO definition");
        auto& actor=kind==1?actors_.create_bot(handle,e.name,*e.model.definition):
                            actors_.create_player(handle,e.name,*e.model.definition);
        e.orientation=actor.orientation;e.position=matrix_position(e.orientation);
        if(kind==2) player_=handle;
        configure_actor_material(e);refresh_membership(e);
    }
    return handle;
}
void Game::set_mesh(Entity& e,std::string_view name) {
    e.object_name=std::string(name);
    build_model(e.model,name);
    if(e.kind==1 || e.kind==2) configure_actor_model(e);
    if(e.kind==0) configure_prop(e);
    rebind_character_model(e);
    refresh_membership(e);
    scene_changed_=true;
}
void Game::build_model(Entity::Model& model,std::string_view name) {
    std::vector<std::string> loading;
    const auto build=[&](auto&& self,Entity::Model& model,std::string_view object_name)->void {
        model.character_clips.clear();
        model.linked.clear();
        const auto identity=key(object_name);
        if(std::find(loading.begin(),loading.end(),identity)!=loading.end())
            throw std::runtime_error("Cyclic original object attachment: "+identity);
        loading.push_back(identity);
        const auto object_path="object/po/"+std::string(object_name)+".po";
        if(!assets_.contains_optional_game_file(object_path)) {
            // 00450ff0 returns its allocated empty model when the PO open fails.
            model.name=std::string(object_name);model.definition=nullptr;
            model.parts.clear();model.linked.clear();model.has_bounds=false;model.bounds={};
            loading.pop_back();return;
        }
        auto object=objects_.find(identity);
        if(object==objects_.end()) object=objects_.emplace(identity,
            read_object(assets_.path("object/po/"+std::string(object_name)+".po"))).first;
        model.name=std::string(object_name);model.definition=&object->second;
        model.parts.clear();model.linked.clear();
        model.has_bounds=false;model.bounds={};
        model.parts.reserve(model.definition->parts.size());
        for(const auto& part:model.definition->parts) {
            model.parts.emplace_back();auto& draw=model.parts.back();
            draw.mesh_name=part.mesh;draw.shader=part.shader;
            draw.lighting_id=++next_lighting_id_;
            draw.visible=part.mesh!="fire_01"&&part.mesh!="fire_02";
            // Original PO construction claims the shader before opening the PM.
            resolve_part_material(draw,0x10000);
            const auto mesh_key=key(part.mesh);auto mesh=meshes_.find(mesh_key);
            if(mesh==meshes_.end()) {
                mesh=meshes_.emplace(mesh_key,read_runtime_mesh(assets_,"object/pm/"+part.mesh+".pm")).first;
                if(!mesh->second)warn("PM yok: ",part.mesh);
            }
            draw.mesh=mesh->second?&*mesh->second:nullptr;
            // Missing original PM bounds are uninitialized: retain identity/state,
            // but contribute no invented geometry, bounds, collision or alignment.
            if(!draw.mesh)continue;
            if(draw.mesh->bones.empty()) draw.collision_bounds=draw.mesh->bounds;
            else {
                const float height=std::bit_cast<float>(std::uint32_t{0x42445604});
                draw.collision_bounds={{-11,-height,-11},{11,height,11}};
            }
            if(!model.has_bounds) {model.bounds=draw.collision_bounds;model.has_bounds=true;}
            else {
                const auto& b=draw.collision_bounds;
                model.bounds.minimum={std::min(model.bounds.minimum.x,b.minimum.x),
                    std::min(model.bounds.minimum.y,b.minimum.y),std::min(model.bounds.minimum.z,b.minimum.z)};
                model.bounds.maximum={std::max(model.bounds.maximum.x,b.maximum.x),
                    std::max(model.bounds.maximum.y,b.maximum.y),std::max(model.bounds.maximum.z,b.maximum.z)};
            }
            draw.pose.resize(draw.mesh->bones.size());
            draw.animation_locals.resize(draw.mesh->bones.size());
            draw.animation_touched.resize(draw.mesh->bones.size());
            draw.evaluator.emplace(*draw.mesh,nullptr);draw.evaluator->evaluate(0,false,draw.pose);
        }
        model.linked.reserve(model.definition->attachments.size());
        for(const auto& attachment:model.definition->attachments) {
            model.linked.emplace_back();auto& child=model.linked.back();
            self(self,child,attachment.object);child.attachment=attachment;
            if(attachment.bone) {
                bool found=false;
                for(std::size_t i=0;i<model.parts.size();++i) {
                    if(!model.parts[i].mesh)continue;
                    const auto& bones=model.parts[i].mesh->bones;
                    if(std::any_of(bones.begin(),bones.end(),[&](const Bone& b){return same_name(b.name,*attachment.bone);})) {
                        child.attachment_part=i;found=true;break;
                    }
                }
                if(!found) throw std::runtime_error("Original attachment bone not found: "+*attachment.bone);
            }
        }
        loading.pop_back();
    };
    build(build,model,name);
}
void Game::resolve_part_material(Entity::Part& part,std::uint32_t constructor_contents) {
    const auto& material=materials_.construct(part.shader,constructor_contents,0,true);
    part.trace_mask=materials_.runtime_trace_mask(part.shader);
    part.material=-1;
    const auto& definitions=materials_.definitions();
    for(std::size_t i=0;i<definitions.size();++i) if(&definitions[i]==&material) {
        part.material=static_cast<int>(i);break;
    }
}
void Game::host_set_mesh(std::uint32_t handle,std::string_view mesh) { set_mesh(entity(handle),mesh); }
std::uint32_t Game::host_find_bot(std::string_view name) const {
    for(auto e=entities_.rbegin();e!=entities_.rend();++e)
        if(e->alive && e->kind==1 && same_name(e->name,name)) return e->handle;
    return 0;
}
std::uint32_t Game::host_player() const { return player_; }
Vec3 Game::host_position(std::uint32_t handle) {
    auto& e=entity(handle);
    const auto* actor=(e.kind==1||e.kind==2)?actors_.find(handle):nullptr;
    return matrix_position(actor?actor->orientation:e.orientation);
}
std::uint32_t Game::host_position_address(std::uint32_t handle) {
    auto& e=entity(handle);
    if(!e.position_address) {
        auto* actor=actors_.find(handle);
        auto& frame=actor?actor->orientation:e.orientation;
        e.position_address=runtime_.bind_words(std::as_writable_bytes(std::span(frame.data()+12,3)));
    }
    return e.position_address;
}
void Game::host_set_position(std::uint32_t handle,Vec3 position) {
    auto& e=entity(handle);
    if(actors_.find(handle)) {
        actors_.set_position(handle,position);
        e.orientation=actors_.find(handle)->orientation;e.position=matrix_position(e.orientation);
    } else {e.position=position;matrix_position(e.orientation,position);refresh_membership(e);}
}
void Game::host_rotate(std::uint32_t handle,int axis,float degrees,bool world) {
    auto& e=entity(handle);const auto position=host_position(handle);
    if(const auto* actor=actors_.find(handle)) e.orientation=actor->orientation;
    Vec3 angles{};
    if(axis==0) angles.y=degrees;
    else if(axis==1) angles.x=degrees;
    else angles.z=degrees;
    if(world) rotate_world(e.orientation,angles);
    else rotate_local(e.orientation,angles);
    matrix_position(e.orientation,position);
    if(auto* actor=actors_.find(handle)) actor->orientation=e.orientation;
}
void Game::host_delete_entity(std::uint32_t handle) {
    auto& e=entity(handle);
    e.alive=false;refresh_membership(e);
    std::erase_if(explosions_,[handle](const auto& explosion){return explosion.entity==handle;});
    if(e.position_address) { runtime_.unbind_words(e.position_address);e.position_address=0; }
    actors_.remove(handle);e.alive=false;e.visible=false;scene_changed_=true;
    if(player_==handle) player_=0;
    if(selected_camera_==handle) selected_camera_=0;
    for(auto& animation:character_playbacks_) if(animation.entity==handle)
        reset_animation_controller(animation);
}
void Game::host_entity_visible(std::string_view name,bool visible) {
    for(auto& e:entities_) if(e.alive && same_name(e.name,name)) e.visible=visible;
}
void Game::host_linked_visible(std::string_view name,std::string_view linked,bool visible) {
    const auto apply=[&](auto&& self,Entity::Model& model)->void {
        for(auto& child:model.linked) { if(child.name.find(linked)!=std::string::npos) child.visible=visible;self(self,child); }
    };
    for(auto& e:entities_) if(e.alive && same_name(e.name,name)) apply(apply,e.model);
}
void Game::host_shader(std::string_view name,std::string_view part_name,std::string_view shader,bool all) {
    for(auto e=entities_.rbegin();e!=entities_.rend();++e) if(e->alive && same_name(e->name,name)) {
        for(auto& part:e->model.parts) if(same_name(part.mesh_name,part_name)) {
            part.shader=std::string(shader);part.runtime_material.reset();
            resolve_part_material(part,0);
            if(e->kind==1||e->kind==2) configure_actor_material(*e);
            scene_changed_=true;break;
        }
        if(!all) break;
    }
}
void Game::host_bot_weapon(std::uint32_t actor,std::uint32_t,int weapon) {
    entity(actor);actors_.grant_weapon(actor,static_cast<WeaponKind>(weapon),99999999);
}
void Game::host_bot_enabled(std::string_view name,bool enabled) {
    if(const auto handle=host_find_bot(name)) if(auto* actor=actors_.find(handle);actor&&actor->ai.registered)
        actor->ai.enabled=enabled;
}
void Game::host_inventory_remove(std::string_view name) { actors_.remove_inventory(player_,name); }
bool Game::host_inventory_contains(std::string_view name) const { return actors_.inventory_contains(player_,name); }
void Game::host_no_weapon(bool enabled) { no_weapon_=enabled;actors_.no_weapon(player_,enabled); }
void Game::host_hurt(float damage) { if(player_) actors_.hurt(player_,damage); }
void Game::host_init_orientation() {
    if(!player_) return;
    actors_.init_orientation(player_);
    auto& e=entity(player_);e.orientation=actors_.find(player_)->orientation;
    e.position=matrix_position(e.orientation);refresh_membership(e);
}
void Game::host_shake(float duration,float yaw,float pitch) { if(player_) actors_.camera_shake(player_,duration,yaw,pitch); }
void Game::host_waypoint(std::string_view name,int operation,int index) {
    const auto handle=host_find_bot(name);if(!handle) return;
    if(operation==0) actors_.waypoint_index(handle,index);
    else if(operation==1) actors_.waypoint_clear_indices(handle);
    else if(operation==2) actors_.waypoint_start(handle);
    else actors_.waypoint_end(handle);
}
void Game::host_camera_fov(std::string_view name,float degrees) {
    for(auto& e:entities_) if(e.alive && same_name(e.name,name)) {
        e.camera_fov=degrees;e.camera_fov_explicit=true;
        if(e.handle==(selected_camera_?selected_camera_:main_camera_)) {
            camera_fov_=degrees;camera_.reference_fov_degrees=degrees;
        }
    }
}
void Game::host_timed_event(std::string name,float delay) {
    // 0042d370 prepends; all due events keep newest-first order, including duplicates.
    const auto milliseconds=static_cast<std::uint32_t>(static_cast<std::int64_t>(
        std::trunc(static_cast<double>(delay)*1000)));
    const auto current=static_cast<std::uint32_t>(static_cast<std::uint64_t>(clock_*1000));
    const std::uint32_t deadline=current+milliseconds;
    events_.push_back({std::move(name),static_cast<double>(deadline)*.001,++next_event_serial_});
}
std::uint32_t Game::host_random() {
    return actors_.random_word();
}
bool Game::host_svar_exists(std::string_view name) const { return variables_.contains(key(name)); }
std::uint32_t Game::host_svar_value(std::string_view name) const {
    const auto variable=variables_.find(key(name));return variable==variables_.end()?0:variable->second.value;
}
void Game::host_svar_word(std::string name,std::uint32_t value,int type) {
    auto [at,inserted]=variables_.try_emplace(key(name));
    if(inserted) at->second.type=type;
    if(at->second.owns_string) runtime_.release_string(at->second.value);
    at->second.text.clear();at->second.owns_string=false;
    at->second.value=value;
}
void Game::host_svar_string(std::string name,std::string value) {
    auto [at,inserted]=variables_.try_emplace(key(name));
    if(inserted) at->second.type=2;
    if(at->second.owns_string) runtime_.release_string(at->second.value);
    at->second.text=std::move(value);
    at->second.value=runtime_.allocate_string(at->second.text);at->second.owns_string=true;
}
void Game::host_particle(std::string_view name,std::string_view definition,Vec3 position,Vec3 velocity,bool active) {
    effects_.create(name,definition,active,position,velocity);scene_changed_=true;
}
void Game::host_particle_active(std::string_view name,bool active) { if(active) effects_.start(name);else effects_.stop(name); }
void Game::host_environment(int type) { environment_=type;media_.environment(type); }
void Game::host_fog_enabled(bool value) { fog_enabled_=value; }
void Game::host_fog_type(int type) { fog_type_=type; }
void Game::host_fog_color(std::array<float,4> color) { fog_color_=color; }
void Game::host_fog_range(std::uint32_t start,std::uint32_t end) { fog_start_=start;fog_end_=end; }
void Game::host_fog_density(float density) { fog_density_=density; }
void Game::host_rendering(bool enabled) {
    if(std::exchange(rendering_,enabled)!=enabled) scene_changed_=true;
}
void Game::host_cinematic(bool enabled) {
    cinematic_=enabled;camera_blocked_=enabled;camera_transition_tick_=game_tick_;
    actors_.set_cinematic(enabled);
    // 00428220/00428260 explicitly write HUD and AI, not a saved prior value.
    hud_visible_=!enabled;interface_.show_hud(hud_visible_);
}
void Game::host_fade(bool fade_in) { fade_in_=fade_in;interface_.set_fade(fade_in,fade_seconds_); }
void Game::host_fade_time(float seconds) {
    fade_seconds_=seconds;interface_.set_fade_time(seconds);
}
void Game::host_fullscreen(std::string_view shader,bool enabled) {
    fullscreen_=enabled;
    if(enabled) {
        fullscreen_shader_=std::string(shader);
        (void)materials_.construct(shader,0,0,false);
    }
    interface_.set_fullscreen_quad(fullscreen_shader_,enabled);
}
void Game::host_subtitle(std::string_view shader,bool enabled) {
    subtitle_=enabled;
    if(enabled) {
        subtitle_shader_=std::string(shader);
        (void)materials_.construct(shader,0,0,false); // Eager original constructor, not a drawn quad.
    }
    interface_.set_subtitle_quad(subtitle_shader_,enabled);
}
void Game::host_subtitle_text(int line,std::string_view id) {
    const auto text=language_.find(key(id));
    if(text==language_.end()) throw std::runtime_error("Unknown original language identifier: "+std::string(id));
    subtitles_.at(static_cast<std::size_t>(line))=text->second;
    interface_.set_subtitle_text(line,text->second);
}
void Game::load_language() {
    const auto names=quoted_strings(assets_.text("langs.txt"));
    const auto values=quoted_strings(assets_.text("langt.txt"));
    if(names.size()!=values.size()) throw std::runtime_error("Original language name/text counts disagree");
    for(std::size_t i=0;i<names.size();++i) language_.insert_or_assign(key(names[i]),values[i]);
}
void Game::queue_level(std::string name,bool image,bool checkpoint) {
    pending_level_=std::move(name);pending_image_=image;pending_checkpoint_=checkpoint;
}
bool Game::scene_changed() noexcept { return std::exchange(scene_changed_,false); }
RenderScene Game::frame() const noexcept {
    RenderScene scene;scene.level=rendering_ && level_?&*level_:nullptr;
    scene.generation=scene_generation_;
    scene.camera=camera_;scene.objects=rendering_?std::span<const RenderObject>(draws_):std::span<const RenderObject>{};
    scene.particles=rendering_?std::span<const RenderParticle>(particles_):std::span<const RenderParticle>{};
    scene.time_seconds=time_;scene.player_effect=player_effect_;
    scene.tick_milliseconds=game_tick_;scene.player_effect_serial=player_effect_serial_;
    scene.gameplay_frame=level_&&!menu_;
    scene.fog.enabled=fog_enabled_;scene.fog.mode=fog_type_;scene.fog.color=fog_color_;
    scene.fog.start=static_cast<float>(fog_start_);scene.fog.end=static_cast<float>(fog_end_);scene.fog.density=fog_density_;
    return scene;
}
} // namespace pusu

namespace pusu {
void Game::load_level(std::string_view name,bool) {
    const std::string level_name(name);
    const bool ending=level_name.find("end_game")!=std::string::npos;
    if(!ending) {
        // 0042ee00 re-enables the image even for load_level_without_image.
        loading_shader_=level_name+"_"+std::to_string(actors_.random_word()%20);
        if(loading_presenter_) loading_presenter_(loading_context_,loading_shader_,0);
    }
    media_.clear_game_sounds();release_position_bindings();
    for(auto& [unused,value]:variables_) if(value.owns_string) runtime_.release_string(value.value);
    variables_.clear();runtime_.clear_callables();runtime_.reset_stack();
    actor_events_.clear();actors_.clear();effects_.clear();physical_materials_.clear();
    physical_materials_.set_level(nullptr);
    entities_.clear();explosions_.clear();explosion_candidates_.clear();
    character_playbacks_.clear();keyframe_playbacks_.clear();events_.clear();sounds_.clear();
    draws_.clear();particles_.clear();clear_membership();
    collision_.reset();level_.reset();meshes_.clear();objects_.clear();animations_.clear();keyframes_.clear();
    materials_.clear_runtime_masks(); // 0042f260 ->0040ea60 ends the old constructor epoch.
    player_=main_camera_=selected_camera_=0;next_entity_=1;bsp_path_.clear();
    level_name_=level_name;pending_mouse_={};
    next_tick_=0;scheduler_ready_=false;prerender_pending_=false;
    ++scene_generation_;scene_changed_=true;
    processing_=input_processing_=rendering_=true;use_only_=no_weapon_=false;
    cinematic_=camera_blocked_=player_effect_=false;camera_transition_tick_=game_tick_-1000u;
    ++player_effect_serial_;
    hud_visible_=true;fog_enabled_=false;fullscreen_=subtitle_=false;
    sound_set_.clear();music_.clear();fullscreen_shader_.clear();subtitle_shader_.clear();subtitles_={};
    camera_={};camera_fov_=settings_.reference_fov;
    interface_.show_hud(true);interface_.set_fullscreen_quad({},false);interface_.set_subtitle_quad({},false);
    // 0042edf0 resets the independent fade before loading authored callbacks.
    fade_in_=false;fade_seconds_=.5f;interface_.set_fade(false,fade_seconds_);
    for(int line=0;line!=4;++line) interface_.set_subtitle_text(line,{});
    actors_.set_cinematic(false);host_environment(1);
    if(ending) {
        // 00431cd9..00431ced: unload, pause, authored credits at0047676c.
        menu_=true;interface_.set_game_active(false);interface_.show_menu("emegi_gecenler");
        rebuild_frame();return;
    }
    main_camera_=create_entity("main_camera_nosave",3);
    register_hosts();actors_.synchronize_clock_tick(game_tick_);
    const auto pcs="level/pcs/"+level_name+".pcs";
    runtime_.load(script::Program::decode(assets_.bytes(pcs),pcs),[this](const std::string& included){
        const auto path="level/pcs/"+included+".pcs";
        return script::Program::decode(assets_.bytes(path),path);
    });
    host_timed_event("explosion",static_cast<float>(host_random())*
        std::bit_cast<float>(std::uint32_t{0x38000100}));
    menu_=false;interface_.set_game_active(true);interface_.hide_menu();
    runtime_.invoke("on_start");runtime_.remove_callable("on_start");
    prerender_pending_=true;
    // 00435e31..00435e3f activates main, then0041f5a0 binds it without placement.
    // Zero selected_camera_ denotes the bound main camera for the normal publisher.
    selected_camera_=0;
    if(auto* player=actors_.player())
        player->player_camera.world=entity(main_camera_).orientation;
    camera_.reference_fov_degrees=0;update_camera_scene();
    if(loading_presenter_) loading_presenter_(loading_context_,loading_shader_,.9f);
    // 0042edf0 prepares cubemaps after on_start and main-camera activation.
    if(scene_preparer_ && level_) {
        rebuild_frame();
        scene_preparer_(loading_context_,frame(),bsp_path_);
    }
    if(loading_presenter_) loading_presenter_(loading_context_,loading_shader_,1);
    loading_shader_.clear();
}

void Game::update_player(float seconds,const GameInput& input) {
    actors_.synchronize_clock_tick(game_tick_);
    actors_.set_player_input_mode(input_processing_,use_only_,menu_);
    actors_.set_mouse_settings(settings_.mouse_sensitivity,interface_.invert_mouse());
    const bool blood=interface_.blood_enabled();
    for(auto& actor:actors_.actors()) if(!actor.retired) actor.blood_enabled=blood;
    actors_.update(seconds,input,collision_.get());
    dispatch_actor_events();
}
void Game::update_events() {
    // Native events prepend. Only the initial traversal is eligible; callbacks
    // may append events without invalidating any borrowed callback argument.
    for(std::size_t remaining=events_.size();remaining!=0;--remaining) {
        const auto index=remaining-1;
        const auto deadline=static_cast<std::uint32_t>(std::llround(events_[index].deadline*1000));
        if(deadline>game_tick_) continue;
        auto event=std::move(events_[index]);
        events_.erase(events_.begin()+static_cast<std::ptrdiff_t>(index));
        runtime_.invoke(event.name);
    }
}
void Game::update_particles(float seconds) {
    if(!collision_) return;
    effects_.set_camera(camera_);
    effects_.update(seconds,*collision_,{this,[](void* context,std::string_view name,std::string_view attachment)
            ->std::optional<Matrix>{
        auto& game=*static_cast<Game*>(context);
        for(auto e=game.entities_.rbegin();e!=game.entities_.rend();++e) if(e->alive&&same_name(e->name,name)) {
            if(!attachment.empty()) return game.attachment_world(e->handle,attachment);
            return (e->kind==1||e->kind==2)?game.actors_.frame(e->handle,1):e->orientation;
        }
        return std::nullopt;
    }});
    for(const auto& impact:effects_.impacts()) physical_materials_.impact(impact);
    for(const auto& settled:effects_.settlements()) {
        const auto id=host_create_entity("dropped_weapon",0,std::string(settled.object));
        auto& dropped=entity(id);dropped.pickup_kind=1;dropped.pickup_weapon=settled.pickup;
        host_set_position(id,settled.position);
    }
}
void Game::update(float seconds,const GameInput& supplied) {
    if(!std::isfinite(seconds)||seconds<0) throw std::invalid_argument("Invalid native frame elapsed time");
    ++frame_stamp_;
    if(!pending_level_.empty()) {
        const auto name=std::exchange(pending_level_,{});
        const bool checkpoint=std::exchange(pending_checkpoint_,false);
        const bool image=std::exchange(pending_image_,false);
        if(checkpoint) load_checkpoint(user_data_directory()/"save"/(name+".psv"));
        else load_level(name,image);
    }
    if(supplied.pause&&level_&&!menu_&&processing_) {
        const auto* player=actors_.find(player_);
        if(input_processing_||(player&&player->combat_state==5))
            interface_.show_menu("ana_sayfa");
    }
    if(!menu_) clock_+=seconds;
    const auto now=static_cast<std::uint32_t>(static_cast<std::uint64_t>(clock_*1000));
    game_tick_=now;time_=static_cast<double>(now)*.001;
    if(!menu_&&level_) {
        pending_mouse_.x+=supplied.mouse_x;pending_mouse_.y+=supplied.mouse_y;
        if(next_tick_==0) {next_tick_=now;scheduler_ready_=true;}
        GameInput input=supplied;input.mouse_x=pending_mouse_.x;input.mouse_y=pending_mouse_.y;
        while(native_tick_due(next_tick_,now)) {
            game_tick_=next_tick_;time_=static_cast<double>(game_tick_)*.001;
            if(processing_) update_player(.033f,input);
            pending_mouse_={};input.mouse_x=input.mouse_y=0;
            next_tick_+=native_interval_ms;
        }
        game_tick_=now;time_=static_cast<double>(now)*.001;actors_.synchronize_clock_tick(now);
        // 00431db6: player frame0 listener precedes render interpolation.
        if(player_) {
            const auto frame=actors_.frame(player_,0);
            media_.listener(matrix_position(frame),{-frame[8],-frame[9],-frame[10]},
                {-frame[4],-frame[5],-frame[6]});
        }
        update_events();dispatch_actor_events();update_triggers();
        if(prerender_pending_) {
            prerender_pending_=false;
            runtime_.invoke("before_first_rendered_frame");
            runtime_.remove_callable("before_first_rendered_frame");
        }
        update_animations(seconds);
        if(player_&&!selected_camera_) {
            camera_=actors_.camera(collision_.get());
            entity(main_camera_).orientation=camera_.frame0;
            entity(main_camera_).position=camera_.position;
        } else update_camera_scene();
        update_particles(seconds);physical_materials_.update(seconds);
        rebuild_frame();
    }
    update_hud(seconds);
}
void Game::on_actor_event(const ActorEvent& event) { apply_actor_event(event); }
void Game::dispatch_actor_events() {
    for(std::size_t i=0;i<actor_events_.size();++i) {
        const auto event=std::move(actor_events_[i]);apply_actor_event(event);
    }
    actor_events_.clear();
}
void Game::apply_actor_event(const ActorEvent& event) {
    switch(event.kind) {
    case ActorEvent::Kind::script:
        // 004164d0 ->00438e30: synchronous zero-operand event before drop RNG.
        if(runtime_.contains(event.name)) {
            runtime_.invoke(event.name);runtime_.remove_callable(event.name);
        }
        break;
    case ActorEvent::Kind::animation:character_animation_event(event);break;
    case ActorEvent::Kind::use:use_entities(event.position);break;
    case ActorEvent::Kind::sound: {
        std::string_view path=event.name;
        if(event.category=="see_player") {
            media_.environment_sound("bot","see_player",path,1,event.position,true,true,event.entity);
            break;
        }
        if(path.starts_with("sound/")) path.remove_prefix(6);
        const auto first=path.find('/');
        const std::string_view category=event.category.empty()?
            (first==path.npos?std::string_view{}:path.substr(0,first)):std::string_view(event.category);
        if(category.empty()) throw std::runtime_error("Original actor sound lacks category: "+event.name);
        if(event.category.empty()) path.remove_prefix(first+1);
        const auto last=path.rfind('/');
        const auto object=last==path.npos?std::string_view{}:path.substr(0,last);
        const auto name=last==path.npos?path:path.substr(last+1);
        const bool spatial=event.entity&&!(category=="effect"&&object=="menu");
        const float volume=category=="gun"&&(name=="shoot"||name=="reload")?130.f/255.f:1.f;
        const auto owner=category=="gun"&&name=="shoot"?event.entity:0;
        media_.environment_sound(category,name,object,volume,event.position,spatial,event.environment_sound,owner);
        break;
    }
    case ActorEvent::Kind::particle:
        effects_.create(event.name,event.name,true,event.position,std::nullopt,event.direction,
            std::array<Vec3,2>{event.basis_right,event.basis_up});
        break;
    case ActorEvent::Kind::drop_weapon:
        if(event.name.ends_with("_gun")) {
            effects_.create_dropped_weapon(event.name,event.name,event.position,event.direction,event.pickup);
        } else {
            const auto weapon=static_cast<std::size_t>(event.weapon);
            if(weapon>=native_guns.size()) throw std::runtime_error("Invalid original dropped weapon ID");
            const auto id=host_create_entity("dropped_weapon",0,std::string(native_guns[weapon])+"_dropped");
            auto& dropped=entity(id);dropped.pickup_kind=1;dropped.pickup_weapon=event.pickup;
            host_set_position(id,event.position);
        }
        break;
    case ActorEvent::Kind::drop_health: {
        const auto id=host_create_entity(event.name,0,event.name);
        auto& dropped=entity(id);dropped.pickup_kind=3;dropped.pickup_health=event.value;
        host_set_position(id,event.position);break;
    }
    case ActorEvent::Kind::weapon_visibility: {
        const auto* actor=actors_.find(event.entity);if(!actor) break;
        const auto weapon=static_cast<std::size_t>(actor->no_weapon?WeaponKind::none:actor->weapon);
        const auto& name=entity(event.entity).name;
        host_linked_visible(name,"gun_",false);
        if(event.value&&weapon&&weapon<native_guns.size()) host_linked_visible(name,native_guns[weapon],true);
        break;
    }
    case ActorEvent::Kind::damage_visual: {
        auto& e=entity(event.entity);
        if(!e.model.parts.empty()) {
            auto& part=e.model.parts.front();
            const auto& current=part.runtime_material?*part.runtime_material:materials_.construct(part.shader,0,0,true);
            // Early164d0 checks CURRENT shader pass-count==3;12760 only
            // requires pass1. No health-derived reconstruction is involved.
            if(current.passes.size()>1&&(event.phase!=1||current.passes.size()==3)) {
                if(!part.runtime_material) part.runtime_material=current;
                part.runtime_material->passes[1].color[3]=static_cast<std::uint8_t>(event.value);
            }
        }
        break;
    }
    case ActorEvent::Kind::weapon_phase:host_weapon_phase(event.entity,event.phase);break;
    case ActorEvent::Kind::weapon_light:host_weapon_light(event);break;
    case ActorEvent::Kind::shot:update_actor_projection(entity(event.entity));break;
    case ActorEvent::Kind::player_death:
        camera_blocked_=true;camera_transition_tick_=game_tick_;
        hud_visible_=false;interface_.show_hud(false);
        break;
    case ActorEvent::Kind::player_effect:
        player_effect_=event.phase!=0&&interface_.graphics_options().motion_blur;
        ++player_effect_serial_; // 00452280 resets capture counters on EVERY write, even unchanged.
        break;
    }
}
void Game::update_hud(float seconds) {
    HudState hud;hud.camera_blocked=camera_blocked_;hud.game_tick=game_tick_;
    hud.camera_transition_tick=camera_transition_tick_;
    if(auto* actor=player_?actors_.find(player_):nullptr) {
        constexpr std::array<int,7> ammunition_types{0,2,3,2,1,1,4};
        constexpr std::array<std::string_view,7> shaders{"","weapon_panel_gun_pistol_cz75",
            "weapon_panel_gun_pistol_desert","weapon_panel_gun_rifle_uzi",
            "weapon_panel_gun_rifle_ak101","weapon_panel_gun_rifle_m4","weapon_panel_gun_shotgun_baba"};
        constexpr std::array<std::string_view,7> names{"","CZ-75","MAGNUM","UZI","AK-101","M4","BABA"};
        const auto kind=static_cast<std::size_t>(actor->no_weapon?WeaponKind::none:actor->weapon);
        if(kind>=shaders.size()) throw std::runtime_error("Invalid original HUD weapon ID");
        hud.health=actor->health;hud.weapon_shader=shaders[kind];hud.weapon_name=names[kind];
        hud.magazine=actor->weapons[kind].magazine;hud.ammunition=actor->ammunition[ammunition_types[kind]];
        hud.damage_elapsed=static_cast<float>(game_tick_-static_cast<std::uint32_t>(actor->hit_time))*.001f;
        if(!menu_&&hud_visible_&&hud.damage_elapsed>.5588235259056091f) actor->hit_region=10;
        hud.damage_direction=actor->hit_region;
        hud.scope_visible=actor->player_camera.scope_visible;
        hud.scope_angle_degrees=actor->player_camera.scope_rotation_anchor+actor->player_camera.special_rotation;
        hud.crosshair=kind!=0&&!camera_blocked_&&actor->player_camera.special==0;
    }
    interface_.update(seconds,hud);
}
void game_clock_check() {
    std::uint32_t deadline=16,ticks=0;
    const auto advance=[&](std::uint32_t now){
        while(native_tick_due(deadline,now)) {deadline+=native_interval_ms;++ticks;}
    };
    advance(16);assert(ticks==0);
    advance(17);assert(ticks==1&&deadline==49);
    advance(49);assert(ticks==1);
    advance(116);assert(ticks==4&&deadline==148);
    assert(!native_tick_due(0xfffffff0u,15));
}
} // namespace pusu

namespace pusu {
void Game::configure_corpse(Entity& e) {
    // 004164d0 late mutation: FIRST ROOT shader only, never linked col_ masks.
    if(!e.model.parts.empty()) {
        auto& first=e.model.parts.front();first.trace_mask&=~0x600u;
        if(first.runtime_material) {
            first.runtime_material->collision_enabled=false;
            first.runtime_material->impact_enabled=false;
        }
    }
    // 0041a3a0 changes the cached model box, not part boxes or physical hull.
    if(e.model.has_bounds)e.model.bounds={{-100,-100,-100},{100,100,100}};
    e.model.frustum_cull=false;refresh_membership(e); // 0041b010: BSP relink.
}
} // namespace pusu
