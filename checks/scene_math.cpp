#include "scene_math.hpp"
#include "resources.hpp"
#include "game_effects.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>
#include <string_view>

int main() {
    using namespace pusu;
    const auto near=[](float a,float b){return std::abs(a-b)<0.0001f;};
    const Vec3 actor_current{33,66,99};
    const Vec3 actor_partial=interpolate_actor_position({},actor_current,100,111,33);
    assert(near(actor_partial.x,11)&&near(actor_partial.y,22)&&near(actor_partial.z,33));
    const Vec3 actor_clamped=interpolate_actor_position({},actor_current,100,150,33);
    assert(near(actor_clamped.x,33)&&near(actor_clamped.y,66)&&near(actor_clamped.z,99));
    const Vec3 actor_underflow=interpolate_actor_position({},actor_current,100,99,33);
    assert(near(actor_underflow.x,33)&&near(actor_underflow.y,66)&&near(actor_underflow.z,99));
    const Vec3 actor_cached=interpolate_actor_position({},actor_current,100,133,66);
    assert(near(actor_cached.x,33)&&near(actor_cached.y,66)&&near(actor_cached.z,99));
    const Matrix world=transform({10,20,30},{0,0,0.70710678f,0.70710678f},{2,3,4});
    const Vec3 p=transform_point(world,{1,2,3});
    assert(near(p.x,16)&&near(p.y,18)&&near(p.z,42));
    const Vec3 restored=transform_point(inverse(world),p);
    assert(near(restored.x,1)&&near(restored.y,2)&&near(restored.z,3));
    const Matrix product=multiply(world,inverse(world));
    for(unsigned i=0;i!=16;++i)assert(near(product[i],identity_matrix()[i]));
    bool rejected=false;
    try{(void)inverse(Matrix{});}catch(const std::runtime_error&){rejected=true;}
    assert(rejected);
    assert(original_sqrt(std::bit_cast<float>(0x3f8000ffu))==1.f);
    assert(std::bit_cast<std::uint32_t>(original_sqrt(2.f))==0x3fb504f3u);
    assert(interpolate({0,0,0,2},{0,0,0,2},0.5f).w==2.f);
    assert(std::isnan(inverse_unchecked(Matrix{})[0]));
    const Matrix raw=transform({0,0,0},{0,0,0,1},{1,1,std::numeric_limits<float>::quiet_NaN()});
    assert(std::isnan(raw[8])&&std::isnan(raw[10]));
    Mesh mesh;
    mesh.positions={{1,0,0},{2,0,0}};
    mesh.normals={{0,1,0},{0,1,0}};
    mesh.vertex_bones={0,-1};
    Bone bone;
    bone.local_bind=bone.global_bind=bone.inverse_bind=identity_matrix();
    bone.vertices={0};
    mesh.bones.push_back(bone);
    const std::array<Matrix,1> pose{transform(Vec3{5,0,0},Quaternion{0,0,0,1})};
    std::array<Vec3,2> positions,normals;
    skin_mesh(mesh,pose,positions,normals);
    assert(near(positions[0].x,6)&&near(positions[1].x,2));
    assert(near(normals[0].y,1)&&near(normals[1].y,1));
    mesh.bones[0].name="hand";
    mesh.bones[0].global_bind=transform({5,0,0},{0,0,0,1});
    mesh.bones[0].inverse_bind=inverse_rigid(mesh.bones[0].global_bind);
    const std::array<Matrix,1> bind_pose{mesh.bones[0].global_bind};
    Attachment attachment;
    attachment.bone="hand";
    attachment.transform=transform({2,0,0},{0,0,0,1});
    assert(near(attachment_transform(transform({10,0,0},{0,0,0,1}),mesh,bind_pose,attachment)[12],12));

    Mesh skeleton;
    Bone root;
    root.name="hand";
    root.local_bind=root.global_bind=transform({10,0,0},{0,0,0,1});
    root.inverse_bind=inverse_rigid(root.global_bind);
    skeleton.bones.push_back(root);
    const BoneFrame first{{2,0,0},{0,0,0,1}},second{{3,0,0},{0,0,0,1}};
    Animation duplicate;
    duplicate.header_word=100;
    duplicate.frame_count=2;
    duplicate.tracks={{"HAND",{first,first}},{"missing",{second,second}}};
    PoseEvaluator repeated(skeleton,&duplicate);
    std::array<Matrix,1> animated;
    repeated.evaluate(.02f,false,animated);
    // Missing names bind ordinal zero; authored duplicate tracks sum matrices.
    assert(near(animated[0][0],2)&&near(animated[0][12],25)&&near(animated[0][15],2));
    std::array<BoneFrame,1> scratch;
    std::array<std::uint8_t,1> touched;
    const std::array zero_layer{AnimationLayer{&repeated,.02f,0,false}};
    evaluate_layered_pose(skeleton,zero_layer,animated,scratch,touched);
    for(float value:animated[0])assert(value==0);
    const std::array skipped{AnimationLayer{&repeated,.02f,1,false,4}};
    evaluate_layered_pose(skeleton,skipped,animated,scratch,touched);
    assert(near(animated[0][0],1)&&near(animated[0][12],10)&&near(animated[0][15],1));

    Animation straight,turned;
    straight.header_word=turned.header_word=100;
    straight.frame_count=turned.frame_count=2;
    const BoneFrame still{{0,0,0},{0,0,0,1}},half_turn{{0,0,0},{0,0,1,0}};
    straight.tracks={{"hand",{still,still}}};
    turned.tracks={{"hand",{half_turn,half_turn}}};
    PoseEvaluator a(skeleton,&straight),b(skeleton,&turned);
    const std::array layers{AnimationLayer{&a,.02f,.5f,false},AnimationLayer{&b,.02f,.5f,false}};
    evaluate_layered_pose(skeleton,layers,animated,scratch,touched);
    // Layer blending is matrix addition, not quaternion interpolation.
    assert(near(animated[0][0],0)&&near(animated[0][1],0)&&near(animated[0][4],0));
    assert(near(animated[0][10],1)&&near(animated[0][12],10)&&near(animated[0][15],1));

    // PA timing is spacing between keys, unlike the PKA total-duration header.
    Animation ramp;
    ramp.header_word=100;
    ramp.frame_count=3;
    ramp.tracks={{"hand",{{{0,0,0},{0,0,0,1}},
                           {{10,0,0},{0,0,0,1}},
                           {{20,0,0},{0,0,0,1}}}}};
    PoseEvaluator ramp_pose(skeleton,&ramp);
    assert(animation_duration_ms(ramp)==200);
    assert(near(ramp_pose.duration_seconds(),.2f));

    // 0044f430: complementary 6/sec fades freeze both sample cursors.
    AnimationControllerState incoming,outgoing;
    play_animation_controller(incoming,2,100);
    play_animation_controller(outgoing,2,100);
    incoming.blend=0;
    fade_animation_controller(incoming,8,200,150);
    fade_animation_controller(outgoing,4,200,150);
    advance_animation_controller(incoming,200,200);
    advance_animation_controller(outgoing,200,200);
    assert(near(incoming.blend,.3f)&&near(outgoing.blend,.7f));
    assert(near(incoming.blend+outgoing.blend,1));
    assert(incoming.sample_tick==50&&outgoing.sample_tick==50);
    const std::array fading{
        AnimationLayer{&ramp_pose,incoming.elapsed,incoming.blend,false},
        AnimationLayer{&repeated,outgoing.elapsed,outgoing.blend,false}};
    evaluate_layered_pose(skeleton,fading,animated,scratch,touched);
    assert(near(animated[0][0],1.7f)&&near(animated[0][12],22));
    assert(near(animated[0][15],1.7f));
    advance_animation_controller(incoming,200,250);
    advance_animation_controller(outgoing,200,250);
    assert(near(incoming.blend,.6f)&&near(outgoing.blend,.4f));
    assert((incoming.flags&8)&&(outgoing.flags&4));
    advance_animation_controller(incoming,200,317);
    advance_animation_controller(outgoing,200,317);
    assert(incoming.active&&incoming.flags==1&&near(incoming.blend,1));
    assert(incoming.sample_tick==50&&incoming.start_tick==267);
    assert(!outgoing.active&&outgoing.flags==3&&near(outgoing.blend,1));
    assert(outgoing.start_tick==0&&outgoing.pause_tick==0);
    assert(outgoing.sample_tick==50&&outgoing.fade_tick==317);
    advance_animation_controller(incoming,200,342);
    assert(incoming.sample_tick==75);

    // 0044f200/3b0 pause/resume excludes the entire paused clock interval.
    AnimationControllerState paused;
    play_animation_controller(paused,1,100);
    pause_animation_controller(paused,200,150);
    assert(paused.flags==2&&paused.pause_tick==150&&paused.sample_tick==50);
    advance_animation_controller(paused,200,450);
    assert(paused.sample_tick==50&&near(paused.elapsed,.05f));
    play_animation_controller(paused,1,450);
    assert(paused.start_tick==400&&paused.flags==1);
    advance_animation_controller(paused,200,475);
    assert(paused.sample_tick==75);
    ramp_pose.evaluate(paused.elapsed,false,animated);
    assert(near(animated[0][12],17.5f));
    const auto wrap_start=std::numeric_limits<std::uint32_t>::max()-24;
    AnimationControllerState wrapped;
    play_animation_controller(wrapped,2,wrap_start);
    advance_animation_controller(wrapped,200,25);
    assert(wrapped.sample_tick==50);
    pause_animation_controller(wrapped,200,25);
    play_animation_controller(wrapped,2,75);
    advance_animation_controller(wrapped,200,100);
    assert(wrapped.sample_tick==75);
    incoming=AnimationControllerState{};
    outgoing=AnimationControllerState{};
    play_animation_controller(incoming,1,wrap_start);
    play_animation_controller(outgoing,1,wrap_start);
    incoming.blend=0;
    fade_animation_controller(incoming,8,200,wrap_start);
    fade_animation_controller(outgoing,4,200,wrap_start);
    advance_animation_controller(incoming,200,25);
    advance_animation_controller(outgoing,200,25);
    assert(near(incoming.blend,.3f)&&near(outgoing.blend,.7f));

    // Original HOLD draws duration-5, LOOP rolls over at >=duration.
    AnimationControllerState held,looped,reversed,stopped;
    play_animation_controller(held,1,0);
    advance_animation_controller(held,200,199);
    assert(held.sample_tick==199);
    advance_animation_controller(held,200,200);
    assert(held.active&&held.sample_tick==195);
    ramp_pose.evaluate(held.elapsed,false,animated);
    assert(near(animated[0][12],29.5f));
    assert(held.start_tick==0); // HOLD reanchors with199ms, not its195ms sample.
    ramp_pose.evaluate(.2f,false,animated);
    assert(near(animated[0][12],29.5f));
    ramp_pose.evaluate(.2f,true,animated);
    assert(near(animated[0][12],10));
    const std::array stop_endpoint{AnimationLayer{&ramp_pose,.2f,1,false,0}};
    evaluate_layered_pose(skeleton,stop_endpoint,animated,scratch,touched);
    assert(near(animated[0][12],10)&&near(animated[0][15],1));
    play_animation_controller(looped,2,0);
    advance_animation_controller(looped,200,199);
    assert(looped.sample_tick==199&&looped.loop);
    advance_animation_controller(looped,200,200);
    assert(looped.sample_tick==0&&looped.start_tick==200);
    advance_animation_controller(looped,200,450);
    assert(looped.sample_tick==50);
    ramp_pose.evaluate(looped.elapsed,true,animated);
    assert(near(animated[0][12],15));
    // FTOL(float(now)-50*.001f*1000) truncates 399.9999976 to399.
    assert(looped.start_tick==399);
    advance_animation_controller(looped,200,451);
    assert(looped.sample_tick==52);
    play_animation_controller(stopped,0,0);
    advance_animation_controller(stopped,200,199);
    assert(stopped.active);
    advance_animation_controller(stopped,200,200);
    assert(!stopped.active&&stopped.flags==3&&near(stopped.blend,1));
    play_animation_controller(reversed,3,0);
    advance_animation_controller(reversed,200,0);
    assert(reversed.sample_tick==195);
    ramp_pose.evaluate(reversed.elapsed,false,animated);
    assert(near(animated[0][12],29.5f));
    advance_animation_controller(reversed,200,75);
    assert(reversed.sample_tick==125);
    pause_animation_controller(reversed,200,75);
    advance_animation_controller(reversed,200,150);
    assert(reversed.flags==2&&reversed.sample_tick==125);
    play_animation_controller(reversed,3,150);
    advance_animation_controller(reversed,200,275);
    assert(reversed.active&&reversed.sample_tick==5);
    ramp_pose.evaluate(reversed.elapsed,false,animated);
    assert(near(animated[0][12],10.5f));
    AnimationControllerState changing_direction;
    play_animation_controller(changing_direction,1,0);
    advance_animation_controller(changing_direction,200,75);
    reverse_animation_controller(changing_direction,3,200,75);
    assert(changing_direction.flags==10&&changing_direction.sample_tick==75);
    advance_animation_controller(changing_direction,200,75);
    assert(changing_direction.flags==1&&changing_direction.sample_tick==75);
    advance_animation_controller(changing_direction,200,100);
    assert(changing_direction.sample_tick==50);
    const std::array reverse_endpoint{AnimationLayer{&ramp_pose,0,1,false,3}};
    evaluate_layered_pose(skeleton,reverse_endpoint,animated,scratch,touched);
    assert(near(animated[0][12],29.5f));
    const std::array reverse_finished{AnimationLayer{&ramp_pose,.2f,1,false,3}};
    evaluate_layered_pose(skeleton,reverse_finished,animated,scratch,touched);
    assert(near(animated[0][12],10.5f));

    // Mode4 is a live no-pose weight master, not a frozen skeletal layer.
    AnimationControllerState master,dependent;
    play_animation_controller(master,4,100);
    master.blend=0;
    fade_animation_controller(master,8,200,100);
    play_animation_controller(dependent,1,100);
    dependent.flags|=16;
    dependent.master_channel=2;
    dependent.coefficient=.5f;
    advance_animation_controller(master,200,150);
    advance_animation_controller(dependent,200,150,&master,true);
    assert(master.active&&master.animation_mode==4&&master.sample_tick==0);
    assert(near(master.blend,.3f)&&near(dependent.blend,.15f));
    const std::array mastered{
        AnimationLayer{&ramp_pose,master.elapsed,master.blend,false,4},
        AnimationLayer{&repeated,dependent.elapsed,dependent.blend,false}};
    evaluate_layered_pose(skeleton,mastered,animated,scratch,touched);
    assert(near(animated[0][0],.3f)&&near(animated[0][12],3.75f));
    assert(near(animated[0][15],.3f));
    advance_animation_controller(master,200,267);
    advance_animation_controller(dependent,200,267,&master,true);
    assert(master.flags==1&&master.sample_tick==0);
    assert(near(dependent.blend,.5f)&&dependent.sample_tick==167);
    advance_animation_controller(master,200,1000);
    assert(master.active&&master.sample_tick==0);
    advance_animation_controller(dependent,200,1000,&master,false);
    assert(!dependent.active&&dependent.flags==3&&near(dependent.blend,1));
    assert(dependent.master_channel==2&&near(dependent.coefficient,.5f));
    play_animation_controller(dependent,1,1000);
    dependent.flags|=16;
    reset_animation_controller(master);
    advance_animation_controller(dependent,200,1000,&master,true);
    assert(!dependent.active&&dependent.flags==3&&near(dependent.blend,1));
    play_animation_controller(dependent,1,1000);
    dependent.flags|=16;
    advance_animation_controller(dependent,200,1000,nullptr,true);
    assert(!dependent.active&&dependent.flags==3);

    Animation sole;
    sole.header_word=100;
    sole.frame_count=1;
    sole.tracks={{"hand",{first}}};
    PoseEvaluator sole_frame(skeleton,&sole);
    sole_frame.evaluate(.5f,true,animated);
    // Approved generic memory-safety fix: never read an unauthored adjacent key.
    assert(near(animated[0][12],12)&&near(animated[0][15],1));
    assert(animation_duration_ms(sole)==0&&sole_frame.duration_seconds()==0);
    for(std::uint32_t mode=0;mode<=4;++mode) {
        AnimationControllerState sole_controller;
        play_animation_controller(sole_controller,mode,100);
        advance_animation_controller(sole_controller,0,100);
        assert(sole_controller.active&&sole_controller.sample_tick==0);
        advance_animation_controller(sole_controller,0,1234);
        assert(sole_controller.sample_tick==0);
        assert(sole_controller.active==(mode!=0));
        const std::array sole_layer{AnimationLayer{&sole_frame,123.456f,1,mode==2,mode}};
        evaluate_layered_pose(skeleton,sole_layer,animated,scratch,touched);
        assert(near(animated[0][12],mode==4?10.f:12.f));
        assert(near(animated[0][15],1));
        sole_frame.evaluate(123.456f,mode==2,animated);
        assert(near(animated[0][12],12));
    }
    AnimationControllerState sole_reverse;
    play_animation_controller(sole_reverse,1,100);
    reverse_animation_controller(sole_reverse,3,0,1234);
    advance_animation_controller(sole_reverse,0,1300);
    assert(sole_reverse.active&&sole_reverse.sample_tick==0);

    Keyframes keyframes;
    keyframes.header_word=2; // Original PKA duration header: milliseconds, not sample count.
    keyframes.frame_count=2;
    assert(keyframe_duration_ticks(keyframes)==2);
    assert(keyframe_playback_duration_ticks(keyframes)==keyframe_duration_ticks(keyframes));
    keyframes.tracks.push_back({"item",{{{0,0,0},{1,1,1},{0,0,0,1}},
                                      {{10,0,0},{1,1,1},{0,0,0,1}}}});
    evaluate_keyframes(keyframes,1u,2u,false,animated);
    assert(near(animated[0][12],5));
    assert(near(evaluate_keyframe_track(keyframes.tracks[0],1u,2u,true)[12],10));
    Keyframes duration_metadata;
    duration_metadata.header_word=50000;
    duration_metadata.frame_count=2001;
    assert(keyframe_duration_ticks(duration_metadata)==50000);
    assert(keyframe_playback_duration_ticks(duration_metadata)==keyframe_duration_ticks(duration_metadata));
    duration_metadata.header_word=18000;
    duration_metadata.frame_count=721;
    assert(keyframe_duration_ticks(duration_metadata)==18000);
    duration_metadata.header_word=std::numeric_limits<std::uint32_t>::max();
    assert(keyframe_duration_ticks(duration_metadata)==202);
    duration_metadata.header_word=0;
    assert(keyframe_duration_ticks(duration_metadata)==0);
    bool zero_duration_rejected=false;
    try {
        (void)keyframe_playback_duration_ticks(duration_metadata);
    } catch(const std::runtime_error& error) {
        zero_duration_rejected=true;
        assert(std::string(error.what())=="Zero-duration PKA cannot be played");
    }
    assert(zero_duration_rejected);

    namespace fs=std::filesystem;
    const fs::path temporary=fs::temp_directory_path()/("pusu-scene-math-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if(fs::exists(temporary))throw std::runtime_error("scene check temporary path collision");
    struct TemporaryTree {
        fs::path path;
        ~TemporaryTree(){std::error_code ignored;fs::remove_all(path,ignored);}
    } cleanup{temporary};
    fs::create_directories(temporary/"level/scene");
    const auto put=[](const fs::path& path,std::string_view text){
        std::ofstream file;
        file.exceptions(std::ios::failbit|std::ios::badbit);
        file.open(path,std::ios::binary);
        file.write(text.data(),static_cast<std::streamsize>(text.size()));
    };
    put(temporary/"level/scene/room.txt","nested\n{\npos 2 0 0\nmesh sample\n}\n");
    put(temporary/"test.txt",
        "5286241829\nroom\n{\nPos 5 0 0\n}\nitem\n{\npos 1 2 3\n"
        "orientation 1 0 0 0 1 0 0 0 1 4 5 6\npos 7 8 9\n"
        "rotlocal 10 20 30\nrotlocal 0 0 0\n}\n");
    put(temporary/"level/scene/scn_mo4_a_09.TXT",
        "obj?garbage_papers_02\n{\n"
        "    mesh obj?garbage_papers_02\n"
        "    pos -126.705429 216.676391 -6.674890\n"
        "    rotlocal 0.000000 0.000001 32.500000\n}\n");
    put(temporary/"retail_question.txt","scn_mo4_a_09\n{\npos -3304 124 -1848\n}\n");
    const std::array<std::string_view,8> unsafe_scene_names{
        "/bad?", "\\bad?", "C:bad?", "bad?/../mesh", "bad?/./mesh",
        "bad?//mesh", "bad?/trailing.", "bad?/trailing "};
    for(std::size_t i=0;i<unsafe_scene_names.size();++i) {
        const auto operand=std::string(unsafe_scene_names[i]);
        put(temporary/("unsafe_scene_"+std::to_string(i)+".txt"),
            "entity\n{\nmesh \""+operand+"\"\n}\n");
    }
    fs::create_directories(temporary/"particle/group");
    fs::create_directories(temporary/"particle/system");
    const char* live_particle=
        "particle_vis\nparticle_count 1\nbirth_rate 1\nlife 2\nlook_back_limit 2\n"
        "explosion 0\nelasticity 0\nshader particle_history\nshader textures/check_particle\n";
    put(temporary/"particle/group/live.txt","live\n");
    put(temporary/"particle/system/live.txt",live_particle);
    put(temporary/"particle/group/malformed.txt","\"live\n");
    put(temporary/"particle/group/oversized.txt",
        "live\nlive\nlive\nlive\nlive\nlive\nlive\nlive\nlive\nlive\nlive\n");
    const std::string_view protected_marker("\0TMSAMVOH",9);
    put(temporary/"particle/group/corrupt.txt",protected_marker);
    put(temporary/"particle/group/corrupt_template.txt","corrupt\n");
    put(temporary/"particle/system/corrupt.txt",protected_marker);
    put(temporary/"particle/group/malformed_template.txt","malformed\n");
    put(temporary/"particle/system/malformed.txt","life 0\n");
    put(temporary/"particle/system/cache_probe.txt","shader never_claimed\n");
    fs::create_directories(temporary/"object/po");
    fs::create_directories(temporary/"object/pm");
    put(temporary/"object/po/missing_pm_only.po","{\npart absent_mesh check/absent\n}\n");
    put(temporary/"object/po/mixed_pm.po",
        "{\npart absent_mesh textures/shared\npart valid_mesh textures/SHARED\n}\n");
    put(temporary/"object/po/missing_pm_bone.po",
        "{\npart absent_mesh check/bone\n}\nboneattachment mixed_pm missing_bone\n");
    for(const auto name:{"missing_pm_only","mixed_pm","missing_pm_bone"}) {
        put(temporary/("particle/group/"+std::string(name)+".txt"),std::string(name)+"\n");
        put(temporary/("particle/system/"+std::string(name)+".txt"),
            std::string(live_particle)+"object "+name+"\nshader textures/SHARED\n");
    }
    // Same strict PM0300 fixture layout as checks/game_triggers.cpp.
    std::string valid_pm="PUPM0300";
    const auto pm_word=[&](std::uint32_t value){
        for(unsigned shift=0;shift<32;shift+=8)valid_pm.push_back(static_cast<char>(value>>shift));
    };
    const auto pm_float=[&](float value){pm_word(std::bit_cast<std::uint32_t>(value));};
    pm_word(3);pm_word(3);pm_word(0);
    for(float value:{-1.f,-1.f,-1.f,1.f,1.f,1.f})pm_float(value);
    for(float value:{0.f,0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f})pm_float(value);
    for(unsigned i=0;i<6;++i)pm_float(0);
    for(unsigned i=0;i<3;++i){pm_float(1);pm_float(0);pm_float(0);}
    for(char value:{0,0,1,0,2,0})valid_pm.push_back(value);
    put(temporary/"object/pm/valid_mesh.pm",valid_pm);
    put(temporary/"particle/group/moving_objects.txt","moving_objects\n");
    put(temporary/"particle/system/moving_objects.txt",
        "particle_vis\nparticle_count 2\nbirth_rate 4\nlife 1\nlook_back_limit 2\n"
        "explosion 0\nvelocity 2 0 0\nobject mixed_pm\n");
    put(temporary/"object/pm/alternate_mesh.pm",valid_pm);
    put(temporary/"object/po/alternate_object.po",
        "{\npart alternate_mesh textures/SHARED\n}\n");
    put(temporary/"particle/group/alternate_objects.txt","alternate_objects\n");
    put(temporary/"particle/system/alternate_objects.txt",
        std::string(live_particle)+"object alternate_object\n");
    AssetStore store(temporary);
    const SceneDefinition scene=read_scene(store,"test.txt",{10,0,0});
    assert(scene.entries.size()==2&&scene.entries[0].name=="nested");
    assert(near(scene.entries[0].position.x,17));
    assert(scene.entries[0].orientation==identity_matrix());
    // A later pos changes only parsed position, not raw orientation translation.
    assert(near(scene.entries[1].position.x,17)&&near(scene.entries[1].orientation[12],4));
    assert(near(scene.entries[1].local_rotation.x,10)&&near(scene.entries[1].local_rotation.y,20));
    const auto retail=read_scene(store,"retail_question.txt");
    assert(retail.entries.size()==1);
    const auto& papers=retail.entries[0];
    assert(papers.name=="obj?garbage_papers_02"&&papers.mesh==papers.name);
    assert(papers.source=="level/scene/scn_mo4_a_09.txt");
    assert(near(papers.position.x,-3430.705429f)&&near(papers.position.y,340.676391f)&&
           near(papers.position.z,-1854.674890f));
    assert(near(papers.local_rotation.y,.000001f)&&near(papers.local_rotation.z,32.5f));
    const auto scene_rejects=[&](const auto& operation){
        bool failed=false;
        try {operation();} catch(const std::runtime_error&) {failed=true;}
        assert(failed);
    };
    scene_rejects([&]{(void)store.contains("level/scene/obj?garbage_papers_02.txt");});
    for(std::size_t i=0;i<unsafe_scene_names.size();++i)
        scene_rejects([&]{(void)read_scene(store,"unsafe_scene_"+std::to_string(i)+".txt");});
    for(const auto name:unsafe_scene_names)
        scene_rejects([&]{(void)read_scene(store,name);});
    scene_rejects([&]{(void)read_scene(store,std::string_view("bad?\0.txt",9));});
    scene_rejects([&]{(void)read_scene(store,"bad?\x01.txt");});

    Level level;
    level.shaders.push_back({"solid",0,1});
    level.planes={{{1,0,0},1},{{-1,0,0},1},{{0,1,0},1},
                  {{0,-1,0},1},{{0,0,1},1},{{0,0,-1},1}};
    for(std::uint32_t i=0;i<6;++i)level.brush_sides.push_back({i,0});
    level.brushes={{0,6,0},{0,6,0}};
    level.leaf_brushes={0,1};
    Leaf leaf;
    leaf.brush_count=2;
    level.leaves.push_back(leaf);
    CollisionWorld collision(level);
    const Trace point=collision.trace_ray({3,0,0},{-1,0,0},3,{});
    assert(point.hit&&point.brush==0&&near(point.end.x,1)&&near(point.fraction,2.f/3));
    const Trace hull=collision.trace_ray({3,0,0},{-1,0,0},3,{{-1,-.5f,-.5f},{1,.5f,.5f}});
    assert(hull.hit&&near(hull.end.x,2)&&near(hull.fraction,1.f/3));
    // Back-cull original brush tracing does NOT invent Quake startsolid hits.
    assert(!collision.trace_ray({0,0,0},{1,0,0},3,{}).hit);
    assert(collision.trace_trigger(0,{3,0,0},{0,0,0},{})==TriggerTrace::enter);
    assert(collision.trace_trigger(0,{0,0,0},{3,0,0},{})==TriggerTrace::exit);
    assert(collision.trace_trigger(0,{0,0,0},{.5f,0,0},{})==TriggerTrace::inside);


    // Missing optional groups return zero systems, not a partially initialized group.
    {
        MaterialLibrary materials;
        const auto* first_default=&materials.construct("no_shader",1,0x10,true);
        const auto* first_sprite=&materials.construct("textures/check_particle",0,0x10,false);
        ParticleRuntime effects(store,materials);
        struct RandomState {std::uint32_t seed{1},draws{};} random;
        effects.set_random_source({&random,[](void* context){
            auto& state=*static_cast<RandomState*>(context);
            ++state.draws;
            state.seed=state.seed*214013u+2531011u;
            return (state.seed>>16)&32767u;
        }});
        effects.set_camera(RenderCamera{});
        effects.create("live","live",true,{3,4,5},{});
        effects.update(.125f,collision);
        assert(effects.particles().size()==1&&effects.objects().empty());
        const RenderParticle visible=effects.particles()[0];
        assert(visible.position.x==3&&visible.position.y==4&&visible.position.z==5);
        assert(visible.size.x>0&&visible.color[3]>0&&visible.triangle);
        assert(visible.triangle_positions[0].y!=visible.triangle_positions[1].y);
        assert(visible.material=="textures/check_particle"&&effects.resource_revision()==1);
        assert(random.draws==1&&random.seed==2745024u);
        // The default shader and overridden directive both retain constructor provenance.
        assert(materials.find_instance("no_shader")==first_default&&first_default->passes.size()==1);
        assert(materials.find_instance("textures/check_particle")==first_sprite&&first_sprite->passes.size()==2);
        const auto* intermediate=materials.find_instance("particle_history");
        assert(intermediate&&intermediate->passes.empty());
        assert(materials.runtime_trace_mask("no_shader")==0x108200u);
        assert(materials.runtime_trace_mask("textures/check_particle")==0x108000u);
        const auto serialized=[&]{
            std::ostringstream output(std::ios::binary);
            effects.save(output);
            return output.str();
        };
        const auto checkpoint=serialized();
        auto history=materials.take_runtime_masks();
        auto restore_history=history;
        materials.swap_runtime_masks(restore_history);
        const auto revision=effects.resource_revision();
        const auto seed=random.seed,draws=random.draws;
        const auto unchanged=[&]{
            // Full checkpoint equality also proves no groups/systems, impacts or settlements added.
            assert(serialized()==checkpoint);
            assert(effects.resource_revision()==revision&&random.seed==seed&&random.draws==draws);
            assert(effects.objects().empty()&&effects.impacts().empty()&&effects.settlements().empty());
            assert(effects.particles().size()==1);
            const auto& output=effects.particles()[0];
            assert(output.position.x==visible.position.x&&output.position.y==visible.position.y&&
                output.position.z==visible.position.z&&output.size.x==visible.size.x&&output.size.y==visible.size.y);
            assert(output.color==visible.color&&output.material==visible.material&&
                output.rotation==visible.rotation&&output.autosprite==visible.autosprite&&
                output.orientation==visible.orientation&&output.triangle==visible.triangle);
            for(std::size_t i=0;i<3;++i) {
                assert(output.triangle_positions[i].x==visible.triangle_positions[i].x&&
                    output.triangle_positions[i].y==visible.triangle_positions[i].y&&
                    output.triangle_positions[i].z==visible.triangle_positions[i].z);
                assert(output.triangle_uv[i].x==visible.triangle_uv[i].x&&
                    output.triangle_uv[i].y==visible.triangle_uv[i].y);
            }
            auto current_history=materials.take_runtime_masks();
            auto restore=current_history;
            materials.swap_runtime_masks(restore);
            assert(current_history.size()==history.size());
            for(const auto& [name,entry]:history) {
                const auto found=current_history.find(name);
                assert(found!=current_history.end()&&found->second.definition==entry.definition&&
                    found->second.mask==entry.mask);
            }
            assert(materials.definition_count()==0&&materials.find_instance("never_claimed")==nullptr);
        };
        assert(!store.contains("particle/group/missing.txt"));
        effects.create("live","missing",true,{9,8,7},{1,2,3});
        unchanged();
        effects.create("missing_explicit","PARTICLE\\GROUP\\missing.TXT",true,{9,8,7},
            std::optional<Vec3>{{1,2,3}},std::optional<Vec3>{{0,0,1}},
            std::array<Vec3,2>{{{1,0,0},{0,1,0}}});
        unchanged();
        effects.create("missing_punctuation","obj?garbage_papers_02",true,{9,8,7},{});
        unchanged();
        // Reindex the same borrowed store: absence stays cached even when the group appears.
        put(temporary/"particle/group/missing.txt","cache_probe\n");
        store=AssetStore(temporary);
        assert(store.contains("particle/group/missing.txt"));
        effects.create("missing_cached","missing",true,{9,8,7},{});
        unchanged();
        const auto rejects=[&](std::string_view definition,std::string_view message){
            bool failed=false;
            try {effects.create("rejected",definition,true,{},{ });}
            catch(const std::runtime_error& error) {
                failed=true;
                assert(std::string_view(error.what()).find(message)!=std::string_view::npos);
            }
            assert(failed);
            unchanged();
        };
        rejects("malformed","Unterminated particle definition quote");
        rejects("oversized","Particle group exceeds original ten systems");
        rejects("corrupt","truncated protected container");
        rejects("corrupt_template","truncated protected container");
        rejects("malformed_template","Invalid particle population or fade duration");
        rejects("../live","invalid asset path component");
        rejects("bad:name","invalid Windows asset name");
        rejects("bad?\x01name","invalid Windows asset name");
        // Indexed malformed files can be repaired: failures are not cached as empty success.
        put(temporary/"particle/group/malformed.txt","live\n");
        effects.create("repaired_group","malformed",true,{6,7,8},{});
        assert(serialized()!=checkpoint);
        effects.update(.125f,collision);
        assert(effects.particles().size()==2);
        assert(effects.particles()[1].position.x==6&&effects.particles()[1].material==visible.material);
        put(temporary/"particle/system/malformed.txt",live_particle);
        effects.create("repaired_template","malformed_template",true,{9,10,11},{});
        effects.update(.125f,collision);
        assert(effects.particles().size()==3&&effects.particles()[2].position.x==9);
        assert(random.draws==3&&effects.resource_revision()==revision);
        assert(materials.find_instance("no_shader")==first_default&&
            materials.find_instance("textures/check_particle")==first_sprite&&
            materials.find_instance("particle_history")==intermediate);
        assert(materials.runtime_trace_mask("no_shader")==0x108200u&&
            materials.runtime_trace_mask("textures/check_particle")==0x108000u);
    }
    // Absent PMs keep logical object parts/shader claims, never become sprites.
    {
        MaterialLibrary materials;
        ParticleRuntime effects(store,materials);
        std::uint32_t draws{};
        effects.set_random_source({&draws,[](void* context){
            ++*static_cast<std::uint32_t*>(context);return 41u;
        }});
        effects.set_camera(RenderCamera{});
        effects.create("absent","missing_pm_only",true,{3,4,5},{});
        assert(draws==0);
        effects.update(.125f,collision);
        assert(draws==1&&effects.objects().empty()&&effects.particles().empty());
        assert(effects.resource_revision()==0);
        assert(materials.find_instance("check/absent")&&
            materials.runtime_trace_mask("check/absent")==0x108600u);
        effects.create("mixed","mixed_pm",true,{6,7,8},{});
        effects.update(.125f,collision);
        assert(draws==2&&effects.objects().size()==1&&effects.particles().empty());
        const auto& object=effects.objects()[0];
        assert(object.mesh&&object.mesh->positions.size()==3&&object.mesh->indices.size()==3);
        assert(object.material=="textures/SHARED"&&object.transform[12]==6&&object.transform[14]==8);
        const auto revision=effects.resource_revision();
        // A prior sprite-context alias claimed this name in missing_pm_only.
        assert(materials.find_instance("textures/shared")->passes.size()==2);
        (void)materials.take_runtime_masks();
        effects.create("cached_mixed","mixed_pm",true,{9,10,11},{});
        assert(draws==2);
        const auto* replay=materials.find_instance("textures/SHARED");
        assert(replay&&replay->name=="textures/shared"&&replay->passes.size()==1);
        assert(materials.runtime_trace_mask("textures/SHARED")==0x108600u);
        effects.update(.125f,collision);
        assert(draws==3&&effects.objects().size()==2&&effects.particles().empty());
        assert(effects.resource_revision()>revision);
        std::ostringstream snapshot(std::ios::binary);
        effects.save(snapshot);
        ParticleRuntime restored(store,materials);
        restored.set_random_source({&draws,[](void* context){
            ++*static_cast<std::uint32_t*>(context);return 41u;
        }});
        std::istringstream input(snapshot.str(),std::ios::binary);
        restored.load(input);
        assert(draws==3&&restored.objects().size()==2&&restored.particles().empty());
        bool failed=false;
        try {effects.create("bone","missing_pm_bone",true,{},{ });}
        catch(const std::runtime_error& error) {
            failed=true;
            assert(std::string_view(error.what()).find("Unknown object particle attachment bone")!=
                std::string_view::npos);
        }
        assert(failed&&draws==3&&effects.objects().size()==2&&effects.particles().empty());
    }
    // Cached-resource births/deaths change preparation topology; motion does not.
    {
        MaterialLibrary materials;
        ParticleRuntime effects(store,materials);
        effects.set_random_source({nullptr,[](void*){return 41u;}});
        effects.create("moving","moving_objects",true,{3,4,5},{});
        effects.update(.125f,collision);
        assert(effects.objects().size()==1);
        const auto* first_mesh=effects.objects()[0].mesh;
        const auto first_revision=effects.resource_revision();
        effects.update(.25f,collision);
        assert(effects.objects().size()==2&&effects.resource_revision()>first_revision);
        assert(effects.objects()[0].mesh==first_mesh&&effects.objects()[1].mesh==first_mesh);
        const auto full_revision=effects.resource_revision();
        const auto prior_transform=effects.objects()[0].transform;
        effects.update(.125f,collision);
        assert(effects.objects().size()==2&&effects.resource_revision()==full_revision);
        assert(effects.objects()[0].transform!=prior_transform);
        effects.update(.625f,collision);
        assert(effects.objects().size()==1&&effects.resource_revision()>full_revision);
        const auto survivor_revision=effects.resource_revision();
        effects.stop("moving");
        assert(effects.objects().empty()&&effects.resource_revision()>survivor_revision);
        const auto stopped_revision=effects.resource_revision();
        effects.create("cached","moving_objects",true,{6,7,8},{});
        effects.update(.125f,collision);
        assert(effects.objects().size()==1&&effects.objects()[0].mesh==first_mesh);
        assert(effects.resource_revision()>stopped_revision);
    }
    // Equal draw counts can still replace a prepared slot by a different cached mesh.
    {
        MaterialLibrary materials;
        ParticleRuntime effects(store,materials);
        effects.set_random_source({nullptr,[](void*){return 41u;}});
        effects.create("warm_a","mixed_pm",true,{},{});
        effects.create("warm_b","alternate_objects",true,{},{});
        effects.update(.125f,collision);
        assert(effects.objects().size()==2);
        const auto* first_mesh=effects.objects()[0].mesh;
        const auto* alternate_mesh=effects.objects()[1].mesh;
        assert(first_mesh!=alternate_mesh);
        effects.stop("warm_a");
        effects.stop("warm_b");
        effects.create("old","mixed_pm",true,{},{});
        effects.update(.125f,collision);
        effects.update(1.5f,collision);
        assert(effects.objects().size()==1&&effects.objects()[0].mesh==first_mesh);
        const auto revision=effects.resource_revision();
        effects.create("replacement","alternate_objects",true,{},{});
        effects.update(.5f,collision);
        assert(effects.objects().size()==1&&effects.objects()[0].mesh==alternate_mesh);
        assert(effects.resource_revision()>revision);
    }
    CharacterMotionState falling;
    falling.position={0,0,1000};
    assert(advance_character(falling,{true,false,33,1000},collision));
    // The 33-ms scheduler does not replace the original K/H recurrence.
    assert(near(falling.position.z,999.5555419921875f));
    assert(falling.gravity_step==1&&near(falling.vertical_speed,26.666667938232422f));
    assert(falling.airborne&&falling.crouch_phase==5&&falling.fall_time_tick==0);
}
