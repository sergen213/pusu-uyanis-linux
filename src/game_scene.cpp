#include "game.hpp"
#include "resources.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <iterator>
#include <utility>

namespace pusu {
namespace {
char scene_lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a'-'A')) : c; }
bool scene_equal(std::string_view a, std::string_view b) noexcept {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
        [](char x,char y){return scene_lower(x)==scene_lower(y);});
}
std::string scene_key(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(),result.end(),result.begin(),scene_lower);
    std::replace(result.begin(),result.end(),'\\','/');
    return result;
}
Vec3 scene_position(const Matrix& matrix) noexcept { return {matrix[12],matrix[13],matrix[14]}; }
void scene_position(Matrix& matrix,Vec3 value) noexcept { matrix[12]=value.x;matrix[13]=value.y;matrix[14]=value.z; }
float scene_component(Vec3 value,int axis) noexcept { return axis==0?value.x:axis==1?value.y:value.z; }
}

void Game::load_bsp(std::string_view name) {
    std::string path(name);
    if(path.size()<3 || !scene_equal(std::string_view(path).substr(path.size()-3),".pl")) path+=".pl";
    auto loaded=std::make_unique<Level>(read_level(assets_.path(path),this,
        loading_presenter_ ? +[](void* context,float progress) {
            auto& game=*static_cast<Game*>(context);
            game.loading_presenter_(game.loading_context_,game.loading_shader_,progress);
        } : nullptr));
    // 004200e0 draws three CRT words per parsed trigger before renderer stage 0.4.
    for(std::size_t i=0;i<loaded->triggers.size();++i) {
        (void)actors_.random_word();
        (void)actors_.random_word();
        (void)actors_.random_word();
    }
    auto collision=std::make_unique<CollisionWorld>(*loaded,materials_);
    collision_=std::move(collision);level_=std::move(loaded);
    clear_membership();
    bsp_path_=path;physical_materials_.set_level(level_.get());
    reset_trigger_state(triggers_,*level_);
    ++scene_generation_;scene_changed_=true;
    if(level_preparer_) {
        level_preparer_(loading_context_,*level_,scene_generation_,loading_shader_);
        if(loading_presenter_) loading_presenter_(loading_context_,loading_shader_,.7f);
    }
    host_create_entity("fatih",2,"fatih");
    constexpr std::array<std::string_view,7> groups{
        "fatih/common/die/leg","fatih/common/die/torso","fatih/common",
        "fatih/leg","fatih/no_weapon","fatih/pistol","fatih/rifle"};
    for(const auto group:groups) {
        for(const auto& filename:assets_.names("object/anim/pa/"+std::string(group))) {
            if(filename.size()<3 || !scene_equal(std::string_view(filename).substr(filename.size()-3),".pa")) continue;
            const auto identity=scene_key(filename);
            if(!animations_.contains(identity)) animations_.emplace(identity,read_animation(assets_.path(filename)));
        }
    }
}

void Game::create_entities(std::string_view name) {
    const auto scene=read_scene(assets_,"level/object/"+std::string(name)+".txt");
    constexpr std::array<int,7> ammo_types{0,2,3,2,1,1,4};
    constexpr std::array<int,7> capacities{0,10,8,20,30,25,7};
    for(const auto& entry:scene.entries) {
        const int kind=entry.name.find("camera_")!=std::string::npos?3:
                       entry.name.find("bot_")!=std::string::npos?1:0;
        const auto handle=create_entity(entry.name,kind);
        auto& e=entity(handle);
        if(kind!=3) set_mesh(e,entry.mesh);
        e.orientation=identity_matrix();
        ActorState* actor=nullptr;
        if(kind==1) {
            if(!e.model.definition) throw std::runtime_error("Bot scene entry has no object definition");
            actor=&actors_.create_bot(handle,e.name,*e.model.definition);
            e.orientation=actor->orientation; // Actual actor-constructor basis, not parser identity.
            actor->drop_ammunition=entry.drop_ammo;actor->drop_health=entry.drop_health;
        }
        host_set_position(handle,entry.position);
        if(actor) actor->orientation=e.orientation;
        const bool bot_mesh=kind!=3 && entry.mesh.find("bot_")!=std::string::npos;
        if(bot_mesh) {
            // 0041c850 copies all 64 bytes. Its mode1 callback does not change
            // the actor's separately cached canonical motion position.
            e.orientation=entry.orientation;
            if(actor) {
                actor->orientation=e.orientation;
                actors_.capture_home_position(handle);actors_.capture_home_forward(handle);
                for(const auto& weapon:entry.take_weapons) if(!weapon.empty())
                    actors_.grant_weapon(handle,weapon,99999999);
                if(entry.weapon) actors_.grant_weapon(handle,entry.weapon->name,99999999);
            }
        }
        rotate_local(e.orientation,entry.local_rotation);
        rotate_world(e.orientation,entry.world_rotation);
        e.position=scene_position(e.orientation);
        if(actor) actor->orientation=e.orientation;
        if(kind!=3) for(int axis=0;axis<3;++axis) {
            if(entry.align_positive[axis]) host_align(handle,axis,0);
            if(entry.align_negative[axis]) host_align(handle,axis,1);
        }
        const bool firstaid=e.name.find("obj_firstaid_kit")!=std::string::npos;
        if(firstaid) {e.pickup_kind=3;e.pickup_health=1;}
        else if(entry.weapon) {
            const int weapon=static_cast<int>(ActorRuntime::weapon_kind(entry.weapon->name));
            if(weapon>0 && weapon<7) {
                e.pickup_kind=1;e.pickup_weapon={weapon,ammo_types[weapon],entry.weapon->count,ammo_types[weapon],0};
            }
        } else if(entry.ammo) {
            e.pickup_kind=2;e.pickup_ammunition_count=entry.ammo->count;
            if(scene_equal(entry.ammo->name,"gun_ammo_556")) e.pickup_ammunition_type=1;
            else if(scene_equal(entry.ammo->name,"gun_ammo_9")) e.pickup_ammunition_type=2;
            else if(scene_equal(entry.ammo->name,"gun_ammo_005")) e.pickup_ammunition_type=3;
            else if(scene_equal(entry.ammo->name,"gun_ammo_12")) e.pickup_ammunition_type=4;
        }
        if(!firstaid && entry.name.find("gun_box")!=std::string::npos && entry.name.find("dropped")!=std::string::npos) {
            if(e.pickup_kind!=1) e.pickup_kind=2;
            int minimum=17,maximum=23;
            if(entry.name.find("gun_box_005_dropped")!=std::string::npos) {e.pickup_ammunition_type=3;minimum=12;maximum=18;}
            else if(entry.name.find("gun_box_12_dropped")!=std::string::npos) {e.pickup_ammunition_type=4;minimum=7;maximum=13;}
            else if(entry.name.find("gun_box_556_dropped")!=std::string::npos) e.pickup_ammunition_type=1;
            else if(entry.name.find("gun_box_9_dropped")!=std::string::npos) e.pickup_ammunition_type=2;
            e.pickup_ammunition_count=static_cast<int>((static_cast<long double>(maximum)-minimum)*
                host_random()*0.000030518509447574615478515625f+minimum);
        } else if(!firstaid && entry.name.find("gun_")!=std::string::npos && entry.name.find("dropped_2")!=std::string::npos) {
            const int weapon=static_cast<int>(ActorRuntime::weapon_kind(entry.name));
            if(weapon>0 && weapon<7) {
                e.pickup_kind=1;e.pickup_weapon={weapon,ammo_types[weapon],capacities[weapon],ammo_types[weapon],0};
            }
        }
        if(kind!=3) refresh_membership(e);
    }
    scene_changed_=true;
}

void Game::host_align(std::uint32_t handle,int axis,std::uint32_t side) {
    if(axis<0 || axis>2) throw script::Error("Invalid entity alignment axis");
    auto& e=entity(handle);
    if(!e.model.has_bounds) return;
    const Bounds& bounds=e.model.bounds;
    const Vec3 half=(bounds.maximum-bounds.minimum)*0.5f;
    const Vec3 center=transform_vector(e.orientation,(bounds.minimum+bounds.maximum)*0.5f);
    const float radius=std::abs(e.orientation[axis])*half.x+
                       std::abs(e.orientation[axis+4])*half.y+std::abs(e.orientation[axis+8])*half.z;
    const float shift=(radius+scene_component(center,axis)-8.005f)*(side?-1.f:1.f);
    auto position=host_position(handle);
    if(axis==0) position.x+=shift;else if(axis==1) position.y+=shift;else position.z+=shift;
    host_set_position(handle,position);
}

void Game::rebuild_frame() {
    const auto* previous_draws=draws_.data();
    draws_.clear();particles_.clear();
    const auto emit=[&](auto&& self,Entity::Model& model,const Matrix& world,const Matrix& owner,bool culling_enabled,bool visible)->void {
        model.world=world;visible=visible&&model.visible;
        if(model.renderable) for(const auto& part:model.parts) {
            draws_.push_back({part.mesh,part.shader,world,part.pose,{1,1,1,1},visible&&part.visible});
            auto& draw=draws_.back();
            draw.material_override=part.runtime_material?&*part.runtime_material:&materials_.construct(part.shader,0,0,true);
            draw.lighting_id=part.lighting_id;
            draw.lighting_origin=scene_position(owner);
            draw.frustum_cull=model.frustum_cull;
            draw.culling_transform=&owner;
            draw.root_bounds=model.has_bounds?&model.bounds:nullptr;
            draw.culling_enabled=culling_enabled;
        }
        for(auto& child:model.linked) {
            Matrix linked_world=world;
            if(child.attachment) {
                if(child.attachment->bone) {
                    const auto& part=model.parts.at(child.attachment_part);
                    if(!part.mesh)throw std::runtime_error("attachment parent has no mesh");
                    linked_world=attachment_transform(world,*part.mesh,part.pose,*child.attachment);
                } else if(child.attachment->transform) linked_world=multiply(world,*child.attachment->transform);
            }
            self(self,child,linked_world,child.world,true,visible);
        }
    };
    for(auto& e:entities_) if(e.alive && e.kind!=3) {
        const Matrix* owner=&e.orientation;
        bool culling_enabled=true;
        if(const auto* actor=actors_.find(e.handle)) {
            e.orientation=actor->orientation;e.position=scene_position(e.orientation);
            owner=&actor->orientation;
            culling_enabled=e.kind!=1 || (actor->combat_state!=6 && actor->combat_state!=7);
        }
        update_actor_projection(e);
        const Matrix world=(e.kind==1 || e.kind==2)?actors_.frame(e.handle,1):e.orientation;
        emit(emit,e.model,world,*owner,culling_enabled,e.visible);
    }
    update_camera_scene();
    const auto objects=effects_.objects();draws_.insert(draws_.end(),objects.begin(),objects.end());
    const auto particles=effects_.particles();particles_.insert(particles_.end(),particles.begin(),particles.end());
    const auto impacts=physical_materials_.objects();draws_.insert(draws_.end(),impacts.begin(),impacts.end());
    // A paused menu retains old draws: acknowledge revisions only after publication.
    const auto particle_revision=effects_.resource_revision();
    const auto material_revision=physical_materials_.resource_revision();
    if(particle_revision!=effects_resource_revision_||material_revision!=materials_resource_revision_)
        scene_changed_=true;
    effects_resource_revision_=particle_revision;materials_resource_revision_=material_revision;
    // Zero-ID effects use their final draw addresses as renderer cache keys.
    if(previous_draws!=draws_.data()&&std::any_of(draws_.begin(),draws_.end(),
        [](const RenderObject& draw){return !draw.lighting_id&&draw.mesh;}))
        scene_changed_=true;
}

void Game::update_camera_scene() {
    const auto camera=selected_camera_?selected_camera_:main_camera_;
    if(!camera) return;
    const auto& e=entity(camera);
    camera_.frame0=e.orientation;
    // 00443b30 transposes the raw basis even for scaled/nonrigid camera frames.
    camera_.position=scene_position(e.orientation);camera_.view=inverse_rigid(e.orientation);
    camera_.near_plane=e.near_plane;camera_.far_plane=e.far_plane;
    const auto* player=actors_.player();
    // 0041f780 death wins scope/script even while blocked; 0041cef0 detaches
    // the player's camera while an authored camera is selected.
    const bool death=!selected_camera_&&player&&(player->combat_state==4||player->combat_state==5);
    const bool scope=player && player->player_camera.special!=0;
    const float actual=death?player->player_camera.fov:
        scope?camera_fov_:(e.camera_fov_explicit?e.camera_fov:settings_.reference_fov);
    camera_.reference_fov_degrees=(death || scope || e.camera_fov_explicit)?actual:0;
    camera_fov_=actual;
}

Matrix Game::keyframe_forward_binding_base(const Matrix& raw,std::string_view target_name) {
    // 0041c930: target NAME substring, not entity kind or mesh-name scene placement.
    if(target_name.find("bot_")==std::string_view::npos) return raw;
    // 00436620 -> 004430b0 supplies B; 0041cb24..0041cb68 applies M*inverse(B), then yaw.
    Matrix base=multiply(raw,inverse_rigid(original_actor_basis));
    rotate_world(base,{0,-90,0});
    return base;
}

void Game::host_keyframe(std::string_view name,int mode,int count,std::string_view linked) {
    const auto identity=scene_key(name);
    auto clip=keyframes_.find(identity);
    if(clip==keyframes_.end()) {
        const auto path="object/anim/pka/"+std::string(name)+".pka";
        if(!assets_.contains(path)) return; // Original 0041d8b0 returns null for absent files.
        clip=keyframes_.emplace(identity,read_keyframes(assets_.path(path))).first;
    }
    const std::uint32_t duration_ticks=keyframe_playback_duration_ticks(clip->second);
    runtime_.invoke_event(name,"_on_start");
    KeyframePlayback playback;
    playback.name=std::string(name);playback.linked=std::string(linked);
    playback.event=playback.name+"_on_end";playback.clip=&clip->second;
    playback.mode=mode;playback.repeats=count;playback.active=true;
    // 0041d8b0 converts the PKA header to milliseconds; frame_count remains the sample count.
    playback.duration=.001f*duration_ticks;
    playback.transforms.resize(clip->second.tracks.size());
    const bool named=!linked.empty();
    for(std::size_t track=0;track<clip->second.tracks.size();++track) {
        if(named && track) break;
        for(auto e=entities_.rbegin();e!=entities_.rend();++e) {
            if(!e->alive || !scene_equal(e->name,named?linked:clip->second.tracks[track].name)) continue;
            playback.targets.push_back(e->handle);playback.target_tracks.push_back(track);
            // 0041c930 captures live raw FRAME0, including the actor basis, before rebasing.
            Matrix base=(e->kind==1 || e->kind==2)?actors_.frame(e->handle,0):e->orientation;
            if(mode==1) {
                const auto& last=clip->second.tracks[track].frames.back();
                base=multiply(base,inverse_rigid(transform(last.position,last.rotation,last.scale)));
            } else base=keyframe_forward_binding_base(base,e->name);
            playback.target_base.push_back(base);
            if(e->name.find("camera_")!=std::string::npos) playback.camera_target=e->handle;
        }
    }
    // 0041d6e0 samples the initial clock after attaching all targets.
    playback.start_tick=game_tick_;
    keyframe_playbacks_.push_back(std::move(playback));
}

void Game::host_pause_keyframe(std::string_view name,float seconds,std::string_view linked) {
    for(auto& playback:keyframe_playbacks_) {
        if(!playback.active || playback.paused || !scene_equal(playback.name,name)) continue;
        if(!linked.empty() && std::none_of(playback.targets.begin(),playback.targets.end(),
            [&](std::uint32_t handle){return entities_[handle-1].alive && scene_equal(entities_[handle-1].name,linked);})) continue;
        // 0041ccc0 adds the unsigned start clock before x87 truncation.
        const auto frozen_tick=seconds==0 ? game_tick_ : static_cast<std::uint32_t>(
            static_cast<std::int64_t>(static_cast<long double>(playback.start_tick)+
                                      1000*static_cast<long double>(seconds)));
        playback.frozen_phase=frozen_tick-playback.start_tick;
        playback.paused=true;playback.pause_dirty=true;
    }
}

void Game::host_resume_keyframe(std::string_view name,std::string_view linked) {
    for(auto& playback:keyframe_playbacks_) {
        if(!playback.active || !playback.paused || !scene_equal(playback.name,name)) continue;
        if(!linked.empty() && std::none_of(playback.targets.begin(),playback.targets.end(),
            [&](std::uint32_t handle){return entities_[handle-1].alive && scene_equal(entities_[handle-1].name,linked);})) continue;
        // 0041ce70 preserves the frozen phase, including unsigned timer wrap.
        playback.start_tick=game_tick_-playback.frozen_phase;
        playback.paused=false;
    }
}

void Game::host_character(std::string_view name,std::string_view animation,bool loop) {
    for(auto e=entities_.rbegin();e!=entities_.rend();++e) {
        if(!e->alive || (e->kind!=1 && e->kind!=2) || !scene_equal(e->name,name)) continue;
        for(auto& playback:character_playbacks_) if(playback.entity==e->handle)
            reset_animation_controller(playback);
        actors_.script_animation(e->handle);
        for(std::size_t i=0;i<e->projection_geometry.active_batches;++i) {
            auto& batch=e->projection_geometry.batches[i];
            batch.mesh.positions.clear();batch.mesh.texcoords.clear();
            batch.mesh.normals.clear();batch.mesh.indices.clear();
            batch.colors.clear();batch.mesh.bounds={};
        }
        e->projection_geometry.active_batches=0;scene_changed_=true;
        host_character_layer(e->handle,"cut_scene/"+std::string(animation),loop,0,1,true);
        break; // Original 0041b890 selects the newest named actor.
    }
}

const std::pair<const std::string,Animation>* Game::character_animation(
    Entity::Model& model,std::string_view clip) {
    if(!model.definition) return nullptr;
    const auto cached=model.character_clips.find(clip);
    if(cached!=model.character_clips.end()) return cached->second;
    // 00451123..0045113a defaults the animation prefix to the loaded object name.
    const auto set=scene_key(model.definition->animation_set ?
        std::string_view(*model.definition->animation_set) : std::string_view(model.name));
    auto path=scene_key(clip);
    if(!path.starts_with("object/anim/pa/")) {
        if(!path.starts_with(set+"/")) path=set+"/"+path;
        path="object/anim/pa/"+path;
    }
    if(!path.ends_with(".pa")) path+=".pa";
    auto animation=animations_.find(path);
    if(animation==animations_.end() && assets_.contains(path))
        animation=animations_.emplace(path,read_animation(assets_.path(path))).first;
    const auto* result=animation==animations_.end() ? nullptr : &*animation;
    model.character_clips.emplace(std::string(clip),result);
    return result;
}

Game::CharacterPlayback* Game::character_controller(std::uint32_t handle,int channel) noexcept {
    for(auto& playback:character_playbacks_)
        if(playback.entity==handle && playback.channel==channel)return &playback;
    return nullptr;
}
const Game::CharacterPlayback* Game::character_controller(std::uint32_t handle,int channel) const noexcept {
    for(const auto& playback:character_playbacks_)
        if(playback.entity==handle && playback.channel==channel)return &playback;
    return nullptr;
}
ActorAnimationController Game::animation_controller(std::uint32_t handle,int channel) const noexcept {
    const auto* playback=character_controller(handle,channel);
    if(!playback)return {};
    const auto animation=animations_.find(playback->clip);
    return {playback->clip,playback->animation_mode,playback->flags,playback->start_tick,
        animation==animations_.end()?0:animation_duration_ms(animation->second),playback->sample_tick};
}

void Game::bind_character_layer(Entity::Model& model,const Animation& animation,int channel) {
    // 0044f8f0 maps PA track names against the RECEIVING model's bones.
    for(auto& part:model.parts) {
        if(!part.mesh)continue;
        if(part.layer_evaluators[channel] && part.layer_animations[channel]==&animation)continue;
        part.layer_evaluators[channel].emplace(*part.mesh,&animation);
        part.layer_animations[channel]=&animation;
        part.animation_locals.resize(part.mesh->bones.size());
        part.animation_touched.resize(part.mesh->bones.size());
    }
    for(auto& child:model.linked)bind_character_layer(child,animation,channel);
}

void Game::rebind_character_model(Entity& e) {
    bool retained=false;
    for(const auto& playback:character_playbacks_) {
        if(playback.entity!=e.handle)continue;
        const auto animation=animations_.find(playback.clip);
        if(animation==animations_.end())
            throw std::runtime_error("Retained character animation is not loaded: "+playback.clip);
        // Retain inactive bindings too: original reset does not unload the clip.
        bind_character_layer(e.model,animation->second,playback.channel);
        retained=true;
    }
    // Reconstruction does not run 00414fa0 or 0044f430; no clock/state/RNG changes.
    if(retained)evaluate_character_model(e.model,e.handle);
}

void Game::evaluate_character_model(Entity::Model& model,std::uint32_t handle) {
    for(auto& part:model.parts) {
        if(!part.mesh)continue;
        std::array<AnimationLayer,6> layers{};
        for(const auto& playback:character_playbacks_) {
            if(!playback.active || playback.entity!=handle)continue;
            const auto channel=static_cast<std::size_t>(playback.channel);
            if(channel>=part.layer_evaluators.size() || !part.layer_evaluators[channel])continue;
            layers[channel]={&*part.layer_evaluators[channel],playback.elapsed,
                playback.blend,false,playback.animation_mode==4?4u:1u};
        }
        // 00447cd0/00448120 rebuild bind hierarchy even after every controller resets.
        evaluate_layered_pose(*part.mesh,std::span(layers),
            part.pose,part.animation_locals,part.animation_touched);
    }
    for(auto& child:model.linked)evaluate_character_model(child,handle);
}

void Game::host_character_layer(std::uint32_t handle,std::string_view clip,bool loop,
                               int channel,float blend,bool restart,std::uint32_t animation_mode) {
    auto& e=entity(handle);
    if(channel<0 || channel>=6) throw std::runtime_error("Original character animation slot is out of range");
    const auto* animation=character_animation(e.model,clip);
    if(!animation) return;
    bind_character_layer(e.model,animation->second,channel);
    auto* playback=character_controller(handle,channel);
    if(!playback) {
        character_playbacks_.emplace_back();playback=&character_playbacks_.back();
        playback->entity=handle;playback->channel=channel;
    }
    // 0044f1c0 retains clocks when the already bound clip is selected again.
    if(!scene_equal(playback->clip,animation->first) || restart) {
        playback->clip=animation->first;
        reset_animation_controller(*playback);
    }
    play_animation_controller(*playback,loop?2u:animation_mode,game_tick_);
    playback->blend=blend;
}

void Game::host_character_pair(const ActorEvent& event) {
    const int first=event.animation_channel;
    if(first!=0 && first!=2)throw std::runtime_error("Invalid original animation pair");
    auto& model=entity(event.entity).model;
    auto* a=character_controller(event.entity,first);
    auto* b=character_controller(event.entity,first+1);
    const auto live=[](const CharacterPlayback* playback) {
        return playback && playback->active && (playback->flags&3)!=3;
    };
    const auto cached=model.character_clips.find(event.animation_clip);
    const auto matches=[&](const CharacterPlayback* playback,const std::string& clip,std::uint32_t mode) {
        return live(playback) && playback->animation_mode==mode && scene_equal(playback->clip,clip);
    };
    // Retain a live matching member BEFORE path resolution, I/O or binding work.
    if(!event.restart_animation && cached!=model.character_clips.end() && cached->second &&
       (matches(a,cached->second->first,event.animation_mode) ||
        matches(b,cached->second->first,event.animation_mode)))return;
    const auto* animation=cached==model.character_clips.end()?
        character_animation(model,event.animation_clip):cached->second;
    if(!animation)return;
    const auto duration=animation_duration_ms(animation->second);
    // The first use of an alternate key may resolve to an already bound clip.
    if(!event.restart_animation &&
       (matches(a,animation->first,event.animation_mode)||matches(b,animation->first,event.animation_mode)))return;
    if(event.animation_reverse_match && !event.restart_animation) {
        for(int channel=first;channel<=first+1;++channel) {
            auto* incoming=character_controller(event.entity,channel);
            if(!matches(incoming,animation->first,event.animation_previous_mode))continue;
            reverse_animation_controller(*incoming,event.animation_mode,duration,game_tick_);
            auto* outgoing=character_controller(event.entity,channel==first?first+1:first);
            if(live(outgoing)) {
                const auto old=animations_.find(outgoing->clip);
                fade_animation_controller(*outgoing,4,
                    old==animations_.end()?0:animation_duration_ms(old->second),game_tick_);
                outgoing->blend=1-incoming->blend;
            }
            return;
        }
    }
    // Replace an empty member first, otherwise the outgoing member, otherwise0.
    const int target=!live(a)?first:!live(b)?first+1:
        (a->flags&4)?first:(b->flags&4)?first+1:first;
    const int other=target==first?first+1:first;
    const auto* previous=character_controller(event.entity,target);
    const float retained=previous && scene_equal(previous->clip,animation->first)?previous->blend:1;
    host_character_layer(event.entity,animation->first,event.animation_mode==2,
        target,retained,event.restart_animation,event.animation_mode);
    auto* incoming=character_controller(event.entity,target);
    auto* outgoing=character_controller(event.entity,other);
    if(!incoming)return;
    if(live(outgoing)) {
        if(event.animation_immediate) {
            reset_animation_controller(*incoming);reset_animation_controller(*outgoing);
            play_animation_controller(*incoming,event.animation_mode,game_tick_);
        } else {
            const auto old=animations_.find(outgoing->clip);
            fade_animation_controller(*outgoing,4,
                old==animations_.end()?0:animation_duration_ms(old->second),game_tick_);
            fade_animation_controller(*incoming,8,duration,game_tick_);
            incoming->blend=1-outgoing->blend;
            return;
        }
    }
    incoming->blend=1;
}

void Game::character_animation_event(const ActorEvent& event) {
    using Operation=ActorEvent::AnimationOperation;
    if(event.animation_operation==Operation::pair) {host_character_pair(event);return;}
    if(event.animation_operation==Operation::direct) {
        if(event.animation_channel!=4 && event.animation_channel!=5)
            throw std::runtime_error("Invalid original dependent animation slot");
        if(event.animation_master_channel!=2 && event.animation_master_channel!=3)
            throw std::runtime_error("Invalid original dependent animation master");
        const auto* master_clip=character_animation(entity(event.entity).model,event.animation_master_clip);
        if(!master_clip)return;
        host_character_layer(event.entity,event.animation_clip,event.animation_mode==2,
            event.animation_channel,1,event.restart_animation,event.animation_mode);
        if(auto* playback=character_controller(event.entity,event.animation_channel)) {
            playback->flags=(playback->flags&~(4u|8u))|16u;
            playback->master_channel=event.animation_master_channel;
            if(playback->master_clip!=master_clip->first)playback->master_clip=master_clip->first;
            playback->coefficient=event.animation_blend;
            if(const auto* master=character_controller(event.entity,playback->master_channel))
                playback->blend=master->blend*playback->coefficient;
        }
        return;
    }
    if(event.animation_operation==Operation::pause_controller) {
        if(auto* playback=character_controller(event.entity,event.animation_channel);playback && playback->active) {
            const auto animation=animations_.find(playback->clip);
            pause_animation_controller(*playback,
                animation==animations_.end()?0:animation_duration_ms(animation->second),game_tick_);
        }
        return;
    }
    if(event.animation_operation==Operation::reset_aim_unless_master) {
        const auto* expected=character_animation(entity(event.entity).model,"common/motionless");
        for(int channel=2;channel<=3;++channel) {
            const auto* master=character_controller(event.entity,channel);
            if(expected && master && master->active && (master->flags&3)!=3 &&
               master->animation_mode==4 && scene_equal(master->clip,expected->first))return;
        }
    }
    for(auto& playback:character_playbacks_)
        if(playback.entity==event.entity &&
           (event.animation_operation==Operation::reset_all || playback.channel>=4))
            reset_animation_controller(playback);
}

void Game::advance_character_controllers() {
    // 00448120 walks the actual six slots ascending, so aim sees advanced torso weights.
    for(int channel=0;channel!=6;++channel)
        for(auto& playback:character_playbacks_) {
            if(!playback.active || playback.channel!=channel)continue;
            const auto animation=animations_.find(playback.clip);
            if(animation==animations_.end()) {reset_animation_controller(playback);continue;}
            const auto* master=character_controller(playback.entity,playback.master_channel);
            advance_animation_controller(playback,animation_duration_ms(animation->second),game_tick_,master,
                master && scene_equal(master->clip,playback.master_clip));
        }
}

double Game::animation_duration(std::uint32_t handle,std::string_view clip) {
    const auto* animation=character_animation(entity(handle).model,clip);
    if(!animation) return 0;
    // 0044f8b0/0044f8d0: authored duration, including a sole-frame clip's zero.
    return static_cast<double>(animation_duration_ms(animation->second));
}

Game::KeyframeStep Game::advance_keyframe(KeyframePlayback& playback,std::uint32_t now,
                                        std::uint32_t duration_ticks) noexcept {
    KeyframeStep step;
    const std::uint32_t clock=playback.paused ?
        playback.start_tick+playback.frozen_phase : now;
    step.elapsed_ticks=clock-playback.start_tick;
    if(step.elapsed_ticks>duration_ticks) {
        playback.completed=static_cast<int>(static_cast<std::uint32_t>(playback.completed)+1u);
        switch(playback.mode) {
        case 0:
        case 1:
            // 0041cf54 truncates now-duration+0.1, then stores the low 32 bits.
            playback.start_tick=static_cast<std::uint32_t>(static_cast<std::int64_t>(
                static_cast<long double>(now)-duration_ticks+.1f));
            step.ended=true;
            break;
        case 2:
        case 3:
            if(playback.repeats!=0 && playback.completed==playback.repeats) {
                playback.active=false;
                // 0041cf91 exits before sampling, _on_end or camera restoration.
                return step;
            }
            playback.start_tick=now-step.elapsed_ticks%duration_ticks;
            break;
        }
        // Preserve the original frozen absolute clock after a start-clock rebase.
        if(playback.paused) playback.frozen_phase=clock-playback.start_tick;
    }
    step.sample_ticks=playback.paused ? playback.frozen_phase : now-playback.start_tick;
    if(playback.mode==1 || (playback.mode==3 && (playback.completed&1))) {
        const auto reverse_ticks=static_cast<std::int64_t>(
            static_cast<long double>(duration_ticks)-step.sample_ticks-.1f);
        step.sample_ticks=static_cast<std::uint32_t>(std::max<std::int64_t>(0,reverse_ticks));
    }
    return step;
}

void Game::update_animations(float) {
    // Original list is prepended: newer playbacks update before older ones.
    const auto initial_count=keyframe_playbacks_.size();
    for(std::size_t index=initial_count;index--;) {
        if(index>=keyframe_playbacks_.size()) continue;
        bool ended=false,restore_camera=false;
        std::uint32_t previous_camera{};
        std::string event;
        {
            auto& playback=keyframe_playbacks_[index];
            if(!playback.active || (playback.paused && !playback.pause_dirty)) continue;
            const std::uint32_t duration_ticks=keyframe_playback_duration_ticks(*playback.clip);
            const auto step=advance_keyframe(playback,game_tick_,duration_ticks);
            if(!playback.active) continue;
            ended=step.ended;
            // The native camera test uses the old phase, not a loop's residual phase.
            if(playback.camera_target && step.elapsed_ticks>125u && !playback.camera_selected) {
                playback.previous_camera=selected_camera_;selected_camera_=playback.camera_target;
                playback.camera_selected=true;
            }
            const auto sample_ticks=step.sample_ticks;
            evaluate_keyframes(*playback.clip,sample_ticks,duration_ticks,false,playback.transforms);
            // 0041d660 prepends bindings; consume the stored encounter order backwards.
            for(std::size_t target=playback.targets.size();target--;) {
                auto& e=entities_[playback.targets[target]-1];
                if(!e.alive) continue;
                const auto track=playback.target_tracks[target];
                const auto& clip_track=playback.clip->tracks[track];
                const bool camera=e.name.find("camera_")!=std::string::npos;
                const bool track_camera=clip_track.name.find("camera_")!=std::string::npos;
                const Matrix animated=camera==track_camera ? playback.transforms[track] :
                    evaluate_keyframe_track(clip_track,sample_ticks,duration_ticks,camera);
                e.orientation=multiply(playback.target_base[target],animated);
                e.position=scene_position(e.orientation);
                if(auto* actor=actors_.find(e.handle)) {
                    actor->orientation=e.orientation;
                    if(e.name.find("bot_")!=std::string::npos) {
                        // 0041cef0/004116e0 reset both caches to base + LOCAL track translation.
                        actor->position=scene_position(playback.target_base[target])+scene_position(animated);
                        actor->previous_position=actor->position;actor->position_time_tick=game_tick_;
                    }
                }
                if(e.kind!=3) refresh_membership(e);
            }
            if(ended) {
                playback.active=false;
                event=playback.event;
                previous_camera=playback.previous_camera;
                restore_camera=playback.camera_target!=0;
                playback.camera_selected=false;
            } else if(playback.pause_dirty && playback.camera_selected) {
                selected_camera_=playback.previous_camera;
                playback.camera_selected=false;
                playback.previous_camera=0;
            }
            playback.pause_dirty=false;
        }
        if(ended) {
            runtime_.invoke(event);
            // VM execution may replace the playback vector; only captured values survive.
            if(restore_camera) selected_camera_=previous_camera;
        }
    }
    actors_.prepare_render(game_tick_,33);
    dispatch_actor_events();
    advance_character_controllers();
    for(auto& e:entities_)if(e.alive)evaluate_character_model(e.model,e.handle);
}

void Game::host_weapon_phase(std::uint32_t handle,int phase) {
    auto& e=entity(handle);
    const auto set=[&](auto&& self,Entity::Model& model)->void {
        for(auto& part:model.parts) {
            if(part.mesh_name.find("fire_01")!=std::string::npos) part.visible=phase==1;
            else if(part.mesh_name.find("fire_02")!=std::string::npos) part.visible=phase==2;
        }
        for(auto& child:model.linked) self(self,child);
    };
    set(set,e.model);
}

// Original 004224c0 traces oriented mesh bounds, NOT triangles. Regional meshes
// are the original col_* attachments created by 00417ed0, not invented hulls.
namespace {
float trace_plane_side(Vec3 n,Vec3 point,float distance,float radius=0) noexcept {
    // 00444590 evaluates z+y+x using x87, then its caller spills to float.
    return static_cast<float>(static_cast<long double>(n.z)*point.z+
        static_cast<long double>(n.y)*point.y+static_cast<long double>(n.x)*point.x-distance-radius);
}
struct BoundsHit { Vec3 position, normal; float distance, plane_distance; };
bool trace_oriented_bounds(Vec3 start, Vec3 end, Vec3 direction, Bounds hull,
                           const Bounds& bounds, const Matrix& world,
                           BoundsHit& result) noexcept {
    const Vec3 center = transform_point(world, (bounds.minimum + bounds.maximum) * 0.5f);
    const Vec3 half = (bounds.maximum - bounds.minimum) * 0.5f;
    // 00436430/50/70 extract columns unchanged, including their original scale.
    const Vec3 axes[3]{{world[0], world[1], world[2]},
                       {world[4], world[5], world[6]},
                       {world[8], world[9], world[10]}};
    const float extents[3]{half.x, half.y, half.z};
    const Vec3 query_half = (hull.maximum - hull.minimum) * 0.5f;
    Vec3 a = start, b = end, normal{};
    float hit_plane = 0;
    bool entered = false;
    // 004224c0 clips mutable endpoints in +X,-X,+Y,-Y,+Z,-Z order.
    for (int axis = 0; axis < 3; ++axis) {
        for (int side = 0; side < 2; ++side) {
            const Vec3 n = axes[axis] * (side ? -1.0f : 1.0f);
            const float plane = dot(center + n * extents[axis], n);
            const float radius = std::abs(n.x) * query_half.x +
                                 std::abs(n.y) * query_half.y +
                                 std::abs(n.z) * query_half.z;
            const float da=trace_plane_side(n,a,plane,radius);
            const float db=trace_plane_side(n,b,plane,radius);
            if (da > -0.001f) {
                if (db > -0.001f) return false;
                normal = n;
                hit_plane = plane;
                a = a + (b - a) * (da / (da - db));
                entered = true;
            } else if (da < 0.001f && db >= 0.001f) {
                b = b + (a - b) * (db / (db - da));
            }
        }
    }
    // The original does not report a ray which starts and stays inside.
    if (!entered) return false;
    result = {a, normal, static_cast<float>(original_dot(a-start,direction)), hit_plane};
    return true;
}
} // namespace

// Template helpers may consume Game's private nested types without naming them.
template<class Model>
Matrix trace_link_world(const Model& parent, const Model& child,
                        const Matrix& parent_world) {
    if (!child.attachment) return parent_world;
    const auto& attachment = *child.attachment;
    if (!attachment.bone) {
        return attachment.transform ? multiply(parent_world, *attachment.transform) : parent_world;
    }
    const auto& part = parent.parts.at(child.attachment_part);
    if (!part.mesh) throw std::runtime_error("attachment parent has no mesh");
    return attachment_transform(parent_world, *part.mesh, part.pose, attachment);
}

template<class Model>
bool trace_named_matrix(const Model& model, const Matrix& world,
                        std::string_view name, Matrix* found=nullptr) {
    if (scene_equal(model.name,name)) { if(found) *found=world;return true; }
    for (const auto& part : model.parts) {
        if (scene_equal(part.mesh_name,name)) { if(found) *found=world;return true; }
        if (!part.mesh) continue;
        for (std::size_t i = 0; i < part.mesh->bones.size(); ++i) {
            if (scene_equal(part.mesh->bones[i].name,name)) {
                if(found) *found=multiply(world,part.pose.at(i));
                return true;
            }
        }
    }
    for (const auto& child : model.linked) {
        if(found) {
            const Matrix child_world=trace_link_world(model,child,world);
            if(trace_named_matrix(child,child_world,name,found)) return true;
        } else if(trace_named_matrix(child,world,name)) return true;
    }
    return false;
}

template<class Entity, class Actors>
Matrix trace_entity_world(const Entity& entity,const Actors& actors,int selector=0) {
    return (entity.kind==1 || entity.kind==2)?actors.frame(entity.handle,selector):entity.orientation;
}

Matrix Game::attachment_world(std::uint32_t handle, std::string_view name) {
    const auto& e = entity(handle);
    Matrix world=trace_entity_world(e,actors_,1),found{};
    if (!name.empty() && trace_named_matrix(e.model,world,name,&found)) return found;
    throw std::runtime_error("named attachment does not exist: " + std::string(name));
}

bool Game::has_attachment(std::uint32_t handle, std::string_view name) const {
    if (name.empty()) return false;
    const auto e = std::find_if(entities_.begin(), entities_.end(),
        [handle](const Entity& candidate) { return candidate.alive && candidate.handle == handle; });
    if (e == entities_.end()) return false;
    return trace_named_matrix(e->model,e->orientation,name);
}

ActorHit Game::trace_scene(Vec3 start, Vec3 end, std::uint32_t source) {
    return trace_scene(start, end, source, 0x400u, 7u, false);
}

ActorHit Game::trace_scene(Vec3 start, Vec3 end, std::uint32_t source,
                           std::uint32_t shader_mask, std::uint32_t flags, bool pickups) {
    const Vec3 segment=end-start;
    return trace_query(start,end,original_normalized(segment),original_length(segment),
        {},source,shader_mask,flags,pickups,source,false);
}

ActorHit Game::trace_motion(Vec3 start,Vec3 direction,float distance,Bounds hull,
                            std::uint32_t source,std::uint32_t shader_mask,
                            std::uint32_t flags,bool pickups,std::uint32_t pickup_actor,bool box_query) {
    return trace_query(start,start+direction*distance,direction,distance,
        hull,source,shader_mask,flags,pickups,pickup_actor,box_query);
}

ActorHit Game::trace_query(Vec3 start,Vec3 end,Vec3 direction,float extent,Bounds hull,
                           std::uint32_t source,std::uint32_t shader_mask,
                           std::uint32_t flags,bool pickups,std::uint32_t pickup_actor,bool box_query) {
    ActorHit result;
    result.position=end;result.distance=extent;
    const Level* query_level=level_.get();
    bool invalidated=false;
    const auto* pickup_recipient=pickups ? actors_.find(pickup_actor) : nullptr;
    const bool allow_pickups=pickup_recipient && pickup_recipient->kind==ActorKind::player;
    const auto leaf_trace=[&](std::uint32_t leaf,Vec3 a,Vec3 b)->bool {
    float nearest=std::numeric_limits<float>::max();
    bool leaf_hit=false;
    if ((flags&1u) && collision_) {
        const auto hit=collision_->trace_leaf(leaf,a,b,hull,shader_mask,direction);
        if (hit.hit) {
            result=ActorHit{};result.hit=true;leaf_hit=true;
            result.position=hit.end;result.normal=hit.normal;
            result.distance=static_cast<float>(original_dot(hit.end-start,direction));
            result.plane_distance=hit.plane_distance;
            result.part=hit.brush;result.material=hit.shader;
            result.shader=level_->shaders.at(static_cast<std::size_t>(hit.shader)).name;
            nearest=hit.distance;
        }
    }
    if (!(flags&6u)) return leaf_hit;
    const auto& members=leaf_entities_.at(leaf);
    std::size_t candidate=0;
    while (candidate<members.size()) {
        const auto handle=members[candidate];
        const auto next=candidate+1<members.size() ? members[candidate+1] : 0u;
        auto& e=entity(handle);
        ++candidate;
        const bool actor_category=e.kind==1 || e.kind==2;
        if (!e.alive || e.handle==source || (!(flags&2u) && e.kind==0) ||
            (!(flags&4u) && actor_category) || (!(e.visible && e.model.visible) && e.kind!=2)) continue;
        const Matrix world=trace_entity_world(e,actors_);
        const auto* actor=actors_.find(e.handle);
        bool taken = false;
        if (allow_pickups) {
            const bool takeable = std::any_of(e.model.parts.begin(), e.model.parts.end(),
                [](const Entity::Part& p) { return p.mesh && (p.trace_mask & 0x1000u) != 0; });
            if (takeable && e.model.has_bounds) {
                const Bounds& bound=e.model.bounds;
                BoundsHit pickup_hit{};
                if (trace_oriented_bounds(a,b,direction,hull,bound,world,pickup_hit)) {
                    if (e.pickup_kind == 1) taken = actors_.take_weapon(pickup_actor,e.pickup_weapon);
                    else if (e.pickup_kind == 2)
                        taken = actors_.take_ammunition(pickup_actor,e.pickup_ammunition_type,e.pickup_ammunition_count);
                    else if (e.pickup_kind == 3) {
                        auto* recipient = actors_.find(pickup_actor);
                        if (recipient && recipient->health < 1.0f) {
                            actors_.set_health(pickup_actor,std::min(1.0f,recipient->health+e.pickup_health));
                            taken = true;
                            ActorEvent sound;
                            sound.kind=ActorEvent::Kind::sound;sound.entity=pickup_actor;
                            sound.name="medikit_taken";sound.category="gun";
                            actor_events_.push_back(std::move(sound));
                        }
                    }
                    else {
                        if (scene_equal(e.name,"anahtar") || scene_equal(e.name,"obj_c4_bomb")) {
                            actors_.add_inventory(pickup_actor,e.name);
                            if (scene_equal(e.name,"anahtar")) {
                                ActorEvent sound;
                                sound.kind=ActorEvent::Kind::sound;sound.entity=pickup_actor;
                                sound.name="card_taken";sound.category="effect";
                                actor_events_.push_back(std::move(sound));
                            }
                        }
                        taken=true;
                    }
                    if (taken) {
                        // invoke_event owns the concatenated name before the VM
                        // runs, so no borrowed name survives the callback.
                        runtime_.invoke_event(e.name,"_on_taken");
                        if (level_.get()!=query_level) {
                            invalidated=true;
                            result=ActorHit{};result.position=end;result.distance=extent;
                            return false;
                        }
                        if (handle<=entities_.size() && entities_[handle-1].alive) host_delete_entity(handle);
                        // Resolve the captured next handle after immediate VM
                        // callbacks prepend/delete members or recursively trace.
                        const auto resume=std::find(members.begin(),members.end(),next);
                        candidate=static_cast<std::size_t>(resume-members.begin());
                    }
                }
            }
        }
        if (taken || !e.alive) continue;
        const auto visit = [&](auto&& self, const Entity::Model& model,
                               const Matrix& model_world, bool root) -> void {
            // Physical type 0 is a simple object; actor-linked objects remain
            // actor-owned for bit4 filtering (237d0 +1dc owner test).
            if ((!(flags&2u) && e.kind==0) || (!(flags&4u) && actor_category)) return;
            const bool region_proxy = !root && model.name.find("col_") != std::string::npos;
            if (!root && (!actor_category || !region_proxy)) return;
            if (root) {
                if (!(e.visible && model.visible) && e.kind != 2) return;
            }
            if (!root && region_proxy && actor && (actor->combat_state == 4 ||
                actor->combat_state == 5 || actor->combat_state == 6)) return;
            const std::size_t count=root ? model.parts.size() : std::min<std::size_t>(1,model.parts.size());
            for (std::size_t i=0;i<count;++i) {
                const auto& part = model.parts[i];
                if (!part.mesh || !(part.trace_mask & shader_mask)) continue;
                BoundsHit hit{};
                if (!trace_oriented_bounds(a,b,direction,hull,part.collision_bounds,model_world,hit)) continue;
                if (!(hit.distance<nearest)) continue;
                nearest = hit.distance;
                result.hit = true;leaf_hit=true;result.entity = e.handle;
                result.position=hit.position;result.normal=hit.normal;
                result.distance=static_cast<float>(original_dot(hit.position-start,direction));
                result.plane_distance = hit.plane_distance;
                result.part=static_cast<int>(i);result.region=model.region;
                // Original col_* results use the parent's first shader, while
                // mask testing uses the proxy's own collision material.
                const auto& shader_part=!root && !e.model.parts.empty() ? e.model.parts.front() : part;
                result.material=shader_part.material;result.shader=shader_part.shader;
                result.mesh=part.mesh_name;
                result.owner=root ? 0u : e.handle;
                result.actor_type=e.kind==4 ? 3 : e.kind;
            }
            if(root && actor_category && (!actor || (actor->combat_state!=4 &&
                actor->combat_state!=5 && actor->combat_state!=6))) {
                const Matrix attachment_parent=actors_.frame(e.handle,1);
                for(auto child=model.linked.rbegin();child!=model.linked.rend();++child)
                    if(child->name.find("col_")!=std::string::npos)
                        self(self,*child,trace_link_world(model,*child,attachment_parent),false);
            }
        };
        visit(visit,e.model,world,true);
    }
    return leaf_hit;
    };
    if (!level_ || level_->leaves.empty()) return result;
    // Exact 00423ee0: clipped nearest segment first; no hull node expansion and
    // no entity deduplication. Stack-local endpoints survive nested VM traces.
    const auto walk=[&](auto&& self,std::int32_t node,Vec3 a,Vec3 b,std::size_t depth)->bool {
        if (invalidated) return false;
        if (node<0) return leaf_trace(static_cast<std::uint32_t>(~node),a,b);
        if (depth>level_->nodes.size()) throw std::runtime_error("cyclic trace BSP");
        const auto branch=level_->nodes.at(static_cast<std::size_t>(node));
        const auto plane=level_->planes.at(branch.plane);
        const float da=trace_plane_side(plane.normal,a,plane.distance);
        const float db=trace_plane_side(plane.normal,b,plane.distance);
        const unsigned side=da>-0.001f ? 0u : 1u;
        if ((db>-0.001f ? 0u : 1u)==side)
            return self(self,branch.children[side],a,b,depth+1);
        const Vec3 split=a+(b-a)*(da/(da-db));
        if (self(self,branch.children[side],a,split,depth+1)) return true;
        return self(self,branch.children[1-side],split,b,depth+1);
    };
    // Exact 00424150 box recursion: back first, then shorten the front interval
    // to the back hit. Each leaf compares its own native dot distances.
    const Vec3 half=(hull.maximum-hull.minimum)*0.5f;
    const auto walk_box=[&](auto&& self,std::int32_t node,Vec3 a,Vec3 b,std::size_t depth)->bool {
        if (invalidated) return false;
        if (node<0) return leaf_trace(static_cast<std::uint32_t>(~node),a,b);
        if (depth>level_->nodes.size()) throw std::runtime_error("cyclic trace BSP");
        const auto branch=level_->nodes.at(static_cast<std::size_t>(node));
        const auto plane=level_->planes.at(branch.plane);
        const float radius=static_cast<float>(std::abs(static_cast<long double>(plane.normal.z))*half.z+
            std::abs(static_cast<long double>(plane.normal.y))*half.y+
            std::abs(static_cast<long double>(plane.normal.x))*half.x);
        float da=trace_plane_side(plane.normal,a,plane.distance,radius);
        float db=trace_plane_side(plane.normal,b,plane.distance,radius);
        bool hit=false;
        if (da<0.001f || db<0.001f) {
            Vec3 back_start=a,back_end=b;
            if (da>=0.001f) back_start=a+(b-a)*(da/(da-db));
            else if (db>=0.001f) back_end=a+(b-a)*(da/(da-db));
            hit=self(self,branch.children[1],back_start,back_end,depth+1);
            if (invalidated) return false;
            if (hit) b=result.position;
        }
        da=trace_plane_side(plane.normal,a,plane.distance,-radius);
        db=trace_plane_side(plane.normal,b,plane.distance,-radius);
        if (da<=-0.001f && db<=-0.001f) return hit;
        Vec3 front_start=a,front_end=b;
        if (da<=-0.001f) front_start=a+(b-a)*(da/(da-db));
        else if (db<=-0.001f) front_end=a+(b-a)*(da/(da-db));
        return self(self,branch.children[0],front_start,front_end,depth+1) || hit;
    };
    if (level_->nodes.empty()) leaf_trace(0,start,end);
    else if (box_query) walk_box(walk_box,0,start,end,0);
    else walk(walk,0,start,end,0);
    return result;
}

namespace {
// Exact contiguous float words assigned by 00417ed0 and copied unchanged by
// 00411100. These are model-space transforms, not bone-local guessed offsets.
constexpr Matrix trace_gun_matrices[]{
    Matrix{.01f,-.96f,-.29f,0,.31f,-.28f,.91f,0,-.95f,-.1f,.29f,0,-.75f,23.21f,-3.62f,1},
    Matrix{0,-.96f,-.29f,0,.32f,-.28f,.91f,0,-.95f,-.09f,.30f,0,-1.22f,23.52f,-4.7f,1},
    Matrix{0,.46f,.89f,0,1,0,0,0,0,.89f,-.46f,0,5.3f,-25.9f,.91f,1},
    Matrix{0,.46f,.89f,0,1,0,0,0,0,.89f,-.46f,0,5.84f,-24.31f,-.01f,1},
    Matrix{0,.48f,.88f,0,1,0,0,0,0,.88f,-.48f,0,7.34f,-25.15f,.60f,1},
    Matrix{0,-.95f,-.30f,0,.36f,-.28f,.89f,0,-.93f,-.11f,.34f,0,.70f,22.97f,-3.62f,1}
};
constexpr Matrix trace_normal_region_matrices[]{
    Matrix{.99f,0,.15f,0,-.01f,1,.07f,0,-.15f,-.07f,.99f,0,1.5f,7.58f,-36.56f,1},
    Matrix{.99f,.03f,.15f,0,-.02f,1,-.06f,0,-.15f,.06f,.99f,0,1.5f,-7.25f,-36.56f,1},
    Matrix{.95f,.15f,-.28f,0,-.02f,.92f,.40f,0,.32f,-.37f,.87f,0,1.25f,21.44f,6.21f,1},
    Matrix{.95f,-.11f,-.30f,0,-.02f,.92f,-.39f,0,.32f,.38f,.87f,0,1.25f,-20.93f,6.48f,1},
    Matrix{1,0,0,0,0,1,0,0,0,0,1,0,.2f,0,14.06f,1},
    Matrix{.97f,0,-.26f,0,0,1,0,0,.26f,0,.97f,0,1.04f,.15f,38.98f,1},
    Matrix{1,0,-.05f,0,0,1,.09f,0,.05f,-.09f,.99f,0,-1.42f,5.77f,-12.52f,1},
    Matrix{1,0,-.05f,0,-.01f,1,-.09f,0,.05f,.09f,.99f,0,-1.42f,-5.31f,-12.52f,1},
    Matrix{.98f,-.09f,.16f,0,0,.88f,.47f,0,-.18f,-.46f,.87f,0,2.89f,13.35f,21.90f,1},
    Matrix{.98f,.10f,.15f,0,0,.84f,-.55f,0,-.18f,.54f,.82f,0,2.86f,-12.64f,21.90f,1}
};
constexpr Matrix trace_fat_region_matrices[]{
    Matrix{1,.01f,.07f,0,-.01f,1,.08f,0,-.07f,-.08f,.99f,0,1.72f,7.61f,-36.63f,1},
    Matrix{1,.01f,.07f,0,0,1,-.06f,0,-.07f,.06f,1,0,1.72f,-7.27f,-36.63f,1},
    Matrix{1,.01f,0,0,-.01f,.91f,.41f,0,0,-.41f,.91f,0,3.16f,21.77f,5.94f,1},
    Matrix{1,.03f,-.02f,0,-.04f,.91f,-.42f,0,0,.42f,.91f,0,3.05f,-21.25f,6.19f,1},
    Matrix{.99f,0,-.16f,0,0,1,0,0,.16f,0,.99f,0,.25f,-.07f,12.21f,1},
    Matrix{1,0,-.08f,0,0,1,0,0,.08f,0,1,0,1.71f,.05f,38.79f,1},
    Matrix{1,.01f,0,0,-.01f,1,.09f,0,0,-.09f,1,0,.78f,5.78f,-12.44f,1},
    Matrix{1,-.01f,0,0,.01f,1,-.09f,0,0,.09f,1,0,.78f,-5.31f,-12.44f,1},
    Matrix{1,-.02f,.02f,0,0,.89f,.46f,0,-.03f,-.46f,.89f,0,3.03f,13.27f,22.19f,1},
    Matrix{1,.03f,.01f,0,-.02f,.84f,-.54f,0,-.03f,.53f,.84f,0,3.04f,-12.54f,22.20f,1}
};
} // namespace

void Game::configure_actor_model(Entity& e) {
    configure_actor_material(e);
    const auto append = [&](std::string_view name, std::string_view object,
                            std::string_view bone, const Matrix& matrix, int region,
                            bool renderable) {
        Entity::Model child;
        build_model(child, object);
        child.name = name;
        child.attachment = Attachment{std::string(object),std::string(bone),matrix};
        child.region = region;
        child.renderable = renderable;
        // Original 00417ed0:1254-1261 clears visibility for every linked child.
        // Weapon-selection events subsequently show the selected gun; regional
        // collision tests deliberately do not test this visibility flag.
        child.visible = false;
        const auto parent = std::find_if(e.model.parts.begin(),e.model.parts.end(),
            [&](const Entity::Part& part) {
                return part.mesh && std::any_of(part.mesh->bones.begin(),part.mesh->bones.end(),
                    [&](const Bone& b) { return scene_equal(b.name,bone); });
            });
        if (parent == e.model.parts.end())
            throw std::runtime_error("actor attachment bone not found: "+std::string(bone));
        child.attachment_part = static_cast<std::size_t>(parent-e.model.parts.begin());
        e.model.linked.push_back(std::move(child));
    };
    constexpr std::string_view guns[]{
        "gun_pistol_cz75","gun_pistol_desert","gun_rifle_ak101",
        "gun_rifle_m4","gun_shotgun_baba","gun_rifle_uzi"
    };
    constexpr std::string_view gun_bones[]{
        "Bip01 R Hand","Bip01 R Hand","Bip01 L Hand",
        "Bip01 L Hand","Bip01 L Hand","Bip01 R Hand"
    };
    for (std::size_t i=0;i<std::size(guns);++i)
        append(std::string(guns[i])+"_attachment",guns[i],gun_bones[i],trace_gun_matrices[i],0,true);
    // 004680a0 is case-sensitive strstr: demos omit collision attachments.
    if (e.name.find("_demo") != std::string::npos) return;
    constexpr std::string_view regions[]{
        "col_alt_bacak","col_alt_bacak_sol","col_alt_kol","col_alt_kol_sol",
        "col_govde","col_kafa","col_ust_bacak","col_ust_bacak_sol","col_ust_kol","col_ust_kol_sol"
    };
    constexpr std::string_view region_bones[]{
        "Bip01 R Calf","Bip01 L Calf","Bip01 R Forearm","Bip01 L Forearm",
        "Bip01 Spine","Bip01 Head","Bip01 R Thigh","Bip01 L Thigh",
        "Bip01 R UpperArm","Bip01 L UpperArm"
    };
    const auto& matrices = e.object_name.find("sisman") != std::string::npos ?
        trace_fat_region_matrices : trace_normal_region_matrices;
    for (std::size_t i=0;i<std::size(regions);++i) {
        // Semantic damage class only: actor+2cc is a separate directional
        // death-reaction code, NOT an ordinal for these ten collision boxes.
        const int region = i==5 ? 1 : (i==0 || i==1 || i==6 || i==7) ? 2 : 0;
        append(std::string(regions[i])+"_attachment",regions[i],region_bones[i],matrices[i],region,false);
    }
}

void Game::configure_actor_material(Entity& e) {
    if (e.kind != 1 && e.kind != 2) return;
    auto& part = e.model.parts.at(0);
    if (part.runtime_material) return;
    // 00450ff0 owns an actor-local shader copy; never mutate the library.
    part.runtime_material.emplace(materials_.construct(part.shader,0,0,true));
    auto& material = *part.runtime_material;
    auto& first = material.passes.at(0);
    // 00417ed0:1131-1139 changes only the original first pass's RGBA/RGB mode.
    first.color = {255,255,255,255};
    first.rgb_gen = MaterialRgbGen::constant;
    material.passes.reserve(material.passes.size()+2);

    // 0040d330 -> 0040d2a0: selector0, depth-write0, LEQUAL, whiteRGBA,
    // no alpha-test/tcmods, tcgen0/base. Appended passes are not re-finalized.
    MaterialPass blood;
    blood.texture.kind = MaterialTextureKind::image;
    blood.texture.image = "textures/3te/decal_bot_blood_01";
    blood.source_mode = MaterialSourceMode::image;
    blood.blend = true;
    blood.blend_source = MaterialBlendFactor::src_alpha;
    blood.blend_destination = MaterialBlendFactor::one_minus_src_alpha;
    blood.color[3] = 0;
    material.passes.push_back(std::move(blood));

    // 00417ed0:1170-1199, DST_COLOR/ZERO, vertex RGB selector5.
    MaterialPass white;
    white.texture.kind = MaterialTextureKind::image;
    white.texture.image.reserve(11);
    white.texture.image = "whiteimage";
    white.source_mode = MaterialSourceMode::image;
    white.blend = true;
    white.blend_source = MaterialBlendFactor::dst_color;
    white.blend_destination = MaterialBlendFactor::zero;
    white.rgb_gen = MaterialRgbGen::vertex;
    material.passes.push_back(std::move(white));
    // Native initial flash indexes pass2 even for authored multi-pass shaders.
    material.passes.at(2).texture.image.reserve(11);
    scene_changed_ = true;
}

void Game::host_weapon_light(const ActorEvent& event) {
    auto& e = entity(event.entity);
    if (e.kind != 1 || (event.value != 1 && event.value != 5)) return;
    auto& part = e.model.parts.at(0);
    if (!part.runtime_material)
        throw std::runtime_error("weapon light requires the actor's owned root material");
    auto& material = *part.runtime_material;
    // 004179f0 emits reset after strict >50ms and checks exactly three passes.
    if (event.value == 5 && material.passes.size() != 3) return;
    auto& pass = material.passes.at(2);
    // 00416d50 changes only pass2. 00406940 yellowimage is4x4RGB255/240/150.
    pass.texture.kind = MaterialTextureKind::image;
    if (event.value == 1) {
        pass.texture.image = "yellowimage";
        pass.rgb_gen = MaterialRgbGen::constant;
        pass.color = {200,200,200,255};
    } else {
        // 004179f0 resets RGB but does not rewrite the existing alpha.
        pass.texture.image = "whiteimage";
        pass.rgb_gen = MaterialRgbGen::vertex;
        pass.color[0] = pass.color[1] = pass.color[2] = 255;
    }
    scene_changed_ = true;
}

void Game::update_actor_projection(Entity& e) {
    const auto* actor = actors_.find(e.handle);
    if (!actor) return;
    const auto previous_count = e.projection_geometry.active_batches;
    const auto retire = [&] {
        for (std::size_t i=0;i<previous_count;++i) {
            auto& batch = e.projection_geometry.batches[i];
            batch.mesh.positions.clear(); batch.mesh.texcoords.clear();
            batch.mesh.normals.clear(); batch.mesh.indices.clear(); batch.colors.clear();
            batch.mesh.bounds = {};
        }
        e.projection_geometry.active_batches = 0;
        if (previous_count) scene_changed_ = true;
    };
    // 00435ed0 skips corpse/dead/retired combat states.
    if (!collision_ || actor->combat_state == 4 || actor->combat_state == 5 ||
        actor->combat_state == 7) {
        retire();
        return;
    }
    // Exact point query: frame0 origin, down1000, mask0x200, world-only mode1.
    const Matrix frame0 = actors_.frame(e.handle,0);
    const Vec3 origin{frame0[12],frame0[13],frame0[14]};
    const auto hit = collision_->trace_ray(origin,{0,0,-1},1000,{},0x200);
    if (!hit.hit) {
        retire();
        return;
    }
    const Vec3 center = hit.end + hit.normal*2;
    // 004115b0(selector1) supplies the upright basis; 004363a0 negates column0.
    const Matrix frame1 = actors_.frame(e.handle,1);
    const Vec3 forward{-frame1[0],-frame1[1],-frame1[2]};
    const Vec3 side = original_normalized(cross(hit.normal,forward));
    const Vec3 u = original_normalized(cross(side,hit.normal));
    // 00416d50 selects golge_aydinlik/byte240; 004179f0 restores golge/byte60
    // strictly after66.667ms. The separate50ms material reset does not retire it.
    const bool bright = actor->weapon_phase != 0;
    float size = 60;
    if (bright) {
        // 00443000 retains x87 precision through 00467dbc's truncation.
        constexpr float reciprocal = std::bit_cast<float>(std::uint32_t{0x38000100});
        const long double fraction = static_cast<long double>(host_random())*reciprocal;
        size = static_cast<float>(static_cast<std::uint32_t>(150.L+90.L*fraction));
    }
    // 00477654 is1/300f; native hit+0x50 is a leaf-local dot displacement.
    constexpr float attenuation = std::bit_cast<float>(std::uint32_t{0x3b5a740e});
    const float width = std::clamp(size-(size*attenuation)*hit.leaf_distance,0.f,size);
    // 004259a0/00425750/00424f40 clip actual world triangles, depth20.
    // The shared projector clears the previous active prefix and reuses capacity.
    const std::string_view shader = bright ? "golge_aydinlik" : "golge";
    const Material* material = &materials_.construct(shader,0,0,false);
    physical_materials_.project_world(e.projection_geometry,center,hit.normal,u,width,width);
    for (std::size_t i=0;i<e.projection_geometry.active_batches;++i) {
        const auto& batch = e.projection_geometry.batches[i];
        RenderObject draw{&batch.mesh,shader};
        draw.visible = e.visible;
        draw.vertex_colors = batch.colors;
        draw.geometry_generation = batch.generation;
        draw.material_override = material;
        draws_.push_back(draw);
    }
    if (previous_count || e.projection_geometry.active_batches) scene_changed_ = true;
}

} // namespace pusu
