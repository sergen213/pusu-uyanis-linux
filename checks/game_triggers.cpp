#include "game.hpp"
#include "scene_math.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void word(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(value >> shift);
}
pusu::script::Program callback(std::string_view name,
                              std::initializer_list<std::uint32_t> data,
                              std::initializer_list<std::uint32_t> code) {
    std::vector<std::uint8_t> bytes;
    word(bytes, 0); word(bytes, static_cast<std::uint32_t>(name.size()));
    bytes.insert(bytes.end(), name.begin(), name.end());
    word(bytes, static_cast<std::uint32_t>(data.size()));
    for (auto value : data) word(bytes, value);
    word(bytes, static_cast<std::uint32_t>(code.size()));
    for (auto value : code) word(bytes, value);
    return pusu::script::Program::decode(bytes);
}
pusu::Level trigger_level(bool angled_first = false) {
    using namespace pusu;
    Level level;
    level.bounds = {{-3,-3,-3},{3,3,3}};
    level.shaders.push_back({"trigger",0,0});
    // A convex cube cut by x+y <= .5; its AABB is not its contact volume.
    level.planes = {{{1,0,0},1},{{-1,0,0},1},{{0,1,0},1},
                    {{0,-1,0},1},{{0,0,1},1},{{0,0,-1},1},
                    {{.70710678f,.70710678f,0},.35355339f},{{1,0,0},0}};
    if (angled_first) level.brush_sides.push_back({6,0});
    for (std::uint32_t i = 0; i != 6; ++i) level.brush_sides.push_back({i,0});
    if (!angled_first) level.brush_sides.push_back({6,0});
    level.brushes.push_back({0,7,0});
    Node split;
    split.plane = 7;
    split.children = {-1,-2};
    split.bounds.bounds = level.bounds;
    level.nodes.push_back(split);
    Leaf front, back;
    front.bounds.bounds = {{0,-3,-3},{3,3,3}};
    back.bounds.bounds = {{-3,-3,-3},{0,3,3}};
    front.first_brush = 0; back.first_brush = 1;
    front.brush_count = back.brush_count = 1;
    level.leaves = {front,back};
    level.leaf_brushes = {0,0};
    // Four genuine overlapping records also make inside[] a whole VM word.
    for (unsigned i = 0; i != 4; ++i)
        level.triggers.push_back({"trigger" + std::to_string(i),0,
                                  {{-1,-1,-1},{1,1,1}},.1f});
    return level;
}
}

namespace pusu {
void game_keyframe_check() {
    Keyframes clip;
    clip.header_word=50000;clip.frame_count=2001;
    clip.tracks.push_back({"object",{}});
    for(std::uint32_t i=0;i<clip.frame_count;++i)
        clip.tracks[0].frames.push_back({{static_cast<float>(i),0,0},{1,1,1},{0,0,0,1}});
    const auto duration=keyframe_duration_ticks(clip);
    const auto playback=[&](int mode) {
        Game::KeyframePlayback state;
        state.clip=&clip;state.mode=mode;state.active=true;
        return state;
    };
    const auto sample=[&](const Game::KeyframeStep& step,float x) {
        std::array<Matrix,1> matrices;
        evaluate_keyframes(clip,step.sample_ticks,duration,false,matrices);
        assert(std::fabs(matrices[0][12]-x)<.001f);
        const auto matrix=evaluate_keyframe_track(clip.tracks[0],step.sample_ticks,duration,false);
        assert(std::fabs(matrix[12]-x)<.001f);
    };
    // Genuine HEADER timing must not finish at the much smaller sample count.
    auto once=playback(0);
    auto step=Game::advance_keyframe(once,2002,duration);
    assert(!step.ended && once.completed==0);sample(step,80.08f);
    step=Game::advance_keyframe(once,25000,duration);
    assert(!step.ended);sample(step,1000);
    step=Game::advance_keyframe(once,50000,duration);
    assert(!step.ended && once.completed==0);sample(step,2000);
    step=Game::advance_keyframe(once,50001,duration);
    assert(step.ended && once.completed==1 && once.start_tick==1);
    assert(step.elapsed_ticks==50001 && step.sample_ticks==50000);sample(step,2000);

    auto reverse=playback(1);
    step=Game::advance_keyframe(reverse,0,duration);
    assert(!step.ended && step.sample_ticks==49999);sample(step,1999.96f);
    step=Game::advance_keyframe(reverse,25000,duration);
    assert(!step.ended && step.sample_ticks==24999);sample(step,999.96f);
    step=Game::advance_keyframe(reverse,50000,duration);
    assert(!step.ended && reverse.completed==0 && step.sample_ticks==0);sample(step,0);
    step=Game::advance_keyframe(reverse,50001,duration);
    assert(step.ended && reverse.completed==1 && step.sample_ticks==0);sample(step,0);

    auto loop=playback(2);
    loop.repeats=0;
    step=Game::advance_keyframe(loop,50000,duration);
    assert(!step.ended && loop.completed==0);sample(step,2000);
    step=Game::advance_keyframe(loop,50001,duration);
    assert(!step.ended && loop.active && loop.completed==1 && loop.start_tick==50000);
    assert(step.elapsed_ticks==50001 && step.sample_ticks==1);sample(step,.04f);
    step=Game::advance_keyframe(loop,100001,duration);
    assert(!step.ended && loop.completed==2 && loop.start_tick==100000);sample(step,.04f);

    auto pingpong=playback(3);
    pingpong.repeats=0;
    step=Game::advance_keyframe(pingpong,50001,duration);
    assert(!step.ended && pingpong.completed==1 && step.sample_ticks==49998);sample(step,1999.92f);
    step=Game::advance_keyframe(pingpong,100001,duration);
    assert(!step.ended && pingpong.completed==2 && step.sample_ticks==1);sample(step,.04f);
    for(int mode:{2,3}) {
        auto finite=playback(mode);
        step=Game::advance_keyframe(finite,50001,duration);
        assert(!finite.active && !step.ended && finite.completed==1);
    }

    auto paused=playback(0);
    paused.paused=true;paused.frozen_phase=25000;
    step=Game::advance_keyframe(paused,60000,duration);
    assert(!step.ended && step.elapsed_ticks==25000);sample(step,1000);
    paused.start_tick=60000-paused.frozen_phase;paused.paused=false;
    step=Game::advance_keyframe(paused,85000,duration);
    assert(!step.ended && step.elapsed_ticks==50000);sample(step,2000);
    step=Game::advance_keyframe(paused,85001,duration);
    assert(step.ended);sample(step,2000);
    auto wrapped=playback(0);
    wrapped.start_tick=std::numeric_limits<std::uint32_t>::max()-100;
    step=Game::advance_keyframe(wrapped,399,duration);
    assert(!step.ended && step.elapsed_ticks==500);sample(step,20);

    Keyframes zero;
    zero.header_word=0;zero.frame_count=2;
    zero.tracks.push_back({"object",{{{0,0,0},{1,1,1},{0,0,0,1}},
                                     {{1,0,0},{1,1,1},{0,0,0,1}}}});
    for(int mode:{2,1}) for(bool paused:{false,true}) {
        auto rejected=playback(mode);
        rejected.clip=&zero;rejected.repeats=0;rejected.completed=3;
        rejected.start_tick=9;rejected.frozen_phase=17;
        rejected.paused=paused;rejected.pause_dirty=true;
        rejected.camera_target=19;rejected.previous_camera=13;rejected.camera_selected=true;
        rejected.duration=.125f;
        const auto before=rejected;
        bool threw=false;
        try {
            (void)Game::advance_keyframe(rejected,100,keyframe_playback_duration_ticks(zero));
        } catch(const std::runtime_error& error) {
            threw=true;
            assert(std::string_view(error.what())=="Zero-duration PKA cannot be played");
        }
        assert(threw);
        assert(rejected.start_tick==before.start_tick && rejected.frozen_phase==before.frozen_phase);
        assert(rejected.completed==before.completed && rejected.active==before.active);
        assert(rejected.paused==before.paused && rejected.pause_dirty==before.pause_dirty);
        assert(rejected.mode==before.mode && rejected.repeats==before.repeats);
        assert(rejected.duration==before.duration && rejected.clip==before.clip);
        assert(rejected.camera_target==before.camera_target && rejected.previous_camera==before.previous_camera);
        assert(rejected.camera_selected==before.camera_selected);
    }

    // Ordinary player/entity names retain the raw captured selector-zero matrix.
    const Matrix raw{2,.25f,-.5f,0, 1,3,.75f,0, -.25f,2,4,0, 17,-23,41,1};
    for(std::string_view name:{"fatih","ordinary_player","BOT_guard","Bot_guard","bot","vehicle_botrig"})
        assert(Game::keyframe_forward_binding_base(raw,name)==raw);

    // Cancel the original actor basis before applying the world yaw. These
    // numeric columns are yaw -90 about world Z, not a second copy of the helper.
    Matrix actor=original_actor_basis;
    actor[12]=17;actor[13]=-23;actor[14]=41;
    const Matrix yaw{0,-1,0,0, 1,0,0,0, 0,0,1,0, 17,-23,41,1};
    for(std::string_view name:{"bot_name","vehicle_bot_rig"}) {
        const auto binding=Game::keyframe_forward_binding_base(actor,name);
        for(std::size_t i=0;i<binding.size();++i)
            assert(std::fabs(binding[i]-yaw[i])<.00001f);
        assert(binding[12]==actor[12] && binding[13]==actor[13] && binding[14]==actor[14]);
    }
}

void game_trigger_check() {
    game_keyframe_check();
    const Level level = trigger_level();
    CollisionWorld collision(level);
    const Bounds hull{{-.3f,-.3f,-.3f},{.3f,.3f,.3f}};
    constexpr Vec3 outside{2,-.8f,0}, touching{1.2f,-.8f,0};
    constexpr std::uint64_t frame = (std::uint64_t{1} << 32) + 2;
    constexpr std::array<std::string_view,4> suffixes{
        "_on_enter","_on_exit","_on_inside","_on_use"};
    const auto name = [&](unsigned index, unsigned event) {
        return level.triggers[index].name + std::string(suffixes[event]);
    };
    const auto load_callbacks = [&](Game::TriggerState& state, script::Runtime& vm,
                                    std::uint32_t& order) {
        const auto stamps = vm.bind_words(std::as_writable_bytes(std::span(state.encounter_stamps)));
        const auto ticks = vm.bind_words(std::as_writable_bytes(std::span(state.last_ticks)));
        const auto inside = vm.bind_words(std::as_writable_bytes(std::span(state.inside)));
        const auto sequence = vm.bind_words(std::as_writable_bytes(std::span(&order,1)));
        for (unsigned i = 0; i != 4; ++i) {
            for (unsigned event = 0; event != suffixes.size(); ++event) {
                // Own count, live stamp/tick/membership observations, and a shared
                // decimal order accumulator. All changes execute real PCS opcodes;
                // there are no host callbacks, event echoes, or replaced dispatch.
                vm.load(callback(name(i,event),
                    {0,stamps+i*8,0,ticks+i*4,0,inside,0,sequence},
                    {2,0,0,1,8,2,0,6,       // ++local count
                     3,1,2,2,6,             // observed stamp before callback returns
                     3,3,2,4,6,             // observed last tick
                     3,5,2,6,6,             // observed packed inside bytes
                     3,7,0,10,9,0,i+1,8,3,7,6})); // order = order*10 + i+1
            }
        }
    };
    const auto count = [&](const script::Runtime& vm, unsigned i, unsigned event) {
        return vm.data_word(name(i,event),0);
    };
    const auto assert_counts = [&](const script::Runtime& vm, unsigned event, unsigned expected) {
        for (unsigned i = 0; i != 4; ++i) assert(count(vm,i,event) == expected);
        assert(vm.stack_size() == 0);
    };

    assert(static_cast<unsigned>(TriggerTrace::miss) == 0);
    assert(static_cast<unsigned>(TriggerTrace::enter) == 1);
    assert(static_cast<unsigned>(TriggerTrace::exit) == 2);
    assert(static_cast<unsigned>(TriggerTrace::inside) == 3);
    assert(collision.leaf_at({-.5f,0,0}) == 1);
    assert(collision.leaf_at({.5f,0,0}) == 0);
    assert(collision.trace_trigger(0,{.8f,.8f,0},{.9f,.9f,0},{}) == TriggerTrace::miss);
    assert(!collision.intersects_brush(0,touching,{}));
    assert(collision.intersects_brush(0,touching,hull));
    assert(collision.trace_trigger(0,outside,touching,hull) == TriggerTrace::enter);

    {
        Game::TriggerState state;
        Game::reset_trigger_state(state,level);
        assert(state.active.empty());
        assert((state.leaf_offsets == std::vector<std::size_t>{0,4,8}));
        assert((state.leaf_triggers == std::vector<std::uint32_t>{0,1,2,3,0,1,2,3}));
        std::uint32_t order = 0;
        script::Runtime vm;
        load_callbacks(state,vm,order);
        // Inactive records do not bootstrap from a wholly-inside movement.
        Game::trace_trigger_state(state,collision,{0,0,0},{.1f,0,0},{},frame,1000,vm);
        Game::trace_trigger_state(state,collision,{.8f,.8f,0},{.9f,.9f,0},{},frame,1000,vm);
        assert(state.active.empty());
        for (auto stamp : state.encounter_stamps) assert(stamp == 0);
        assert_counts(vm,0,0);
        // Both preceding non-transitions used this SAME frame: misses must not stamp.
        Game::trace_trigger_state(state,collision,outside,touching,hull,frame,2000,vm);
        assert_counts(vm,0,1);
        assert((state.active == std::vector<std::uint32_t>{0,1,2,3}));
        for (unsigned i = 0; i != 4; ++i) {
            assert(state.inside[i] == 1 && state.last_ticks[i] == 2000);
            assert(state.encounter_stamps[i] == frame);
            assert(vm.data_word(name(i,0),2) == 2);
            assert(vm.data_word(name(i,0),4) == 2000);
            assert(((vm.data_word(name(i,0),6) >> (i*8)) & 255) == 1);
        }
        // Same-frame enter/exit and motion substeps cannot undo the transition.
        Game::trace_trigger_state(state,collision,touching,outside,hull,frame,2001,vm);
        Game::trace_trigger_state(state,collision,outside,touching,hull,frame,2002,vm);
        assert_counts(vm,0,1); assert_counts(vm,1,0);
        assert(state.active.size() == 4);
        // A new-frame ENTER on an already active record is not another enter.
        Game::trace_trigger_state(state,collision,outside,touching,hull,frame+1,2003,vm);
        assert_counts(vm,0,1);
        for (auto stamp : state.encounter_stamps) assert(stamp == frame);
        Game::repeat_trigger_state(state,level,2100,vm);
        assert_counts(vm,2,0); // 1000 * stored .1f is slightly GREATER than 100.
        order = 0;
        Game::repeat_trigger_state(state,level,2101,vm);
        assert_counts(vm,2,1); assert(order == 4321);
        for (unsigned i = 0; i != 4; ++i) {
            assert(vm.data_word(name(i,2),4) == 2000); // reset follows the event
            assert(vm.data_word(name(i,2),6) == 0x01010101u);
            assert(state.last_ticks[i] == 2101);
        }
        Game::repeat_trigger_state(state,level,2101,vm);
        assert_counts(vm,2,1);
        Game::trace_trigger_state(state,collision,touching,outside,hull,frame+2,2102,vm);
        assert_counts(vm,1,1);
        assert(state.active.empty());
        for (unsigned i = 0; i != 4; ++i) {
            assert(vm.data_word(name(i,1),2) == 2); // old stamp during exit
            assert(((vm.data_word(name(i,1),6) >> (i*8)) & 255) == 1);
            assert(state.inside[i] == 0 && state.encounter_stamps[i] == frame+2);
        }
        Game::repeat_trigger_state(state,level,10000,vm);
        assert_counts(vm,2,1);
        // The low 32 bits match the prior exit: a truncated frame stamp would skip.
        const auto later_frame = frame+2+(std::uint64_t{1} << 32);
        Game::trace_trigger_state(state,collision,outside,touching,hull,later_frame,3000,vm);
        assert_counts(vm,0,2);
        for (auto stamp : state.encounter_stamps) assert(stamp == later_frame);
    }

    {
        Game::TriggerState state;
        Game::reset_trigger_state(state,level);
        std::uint32_t order = 0;
        script::Runtime vm;
        load_callbacks(state,vm,order);
        // One pass enters in BACK, then exits in FRONT; the outer frame suppresses
        // that second leaf encounter rather than synthesizing an enter+exit pair.
        Game::trace_trigger_state(state,collision,{-2,-.75f,0},{2,-.75f,0},{},1,100,vm);
        assert_counts(vm,0,1); assert_counts(vm,1,0);
        assert(state.active.size() == 4);
        for (auto stamp : state.encounter_stamps) assert(stamp == 1);
    }

    // Both endpoints outside, in ONE BSP leaf: stored brush-side order controls
    // the native LAST clip flag. Endpoint containment cannot replace this query.
    for (bool angled_first : {false,true}) {
        const Level swept_level = trigger_level(angled_first);
        CollisionWorld swept(swept_level);
        constexpr Vec3 start{.25f,-2,0}, end{.25f,2,0};
        assert(!swept.intersects_brush(0,start,{}));
        assert(!swept.intersects_brush(0,end,{}));
        assert(swept.trace_trigger(0,start,end,{}) ==
               (angled_first ? TriggerTrace::enter : TriggerTrace::exit));
        Game::TriggerState state;
        Game::reset_trigger_state(state,swept_level);
        std::uint32_t order = 0;
        script::Runtime vm;
        load_callbacks(state,vm,order);
        Game::trace_trigger_state(state,swept,start,end,{},1,100,vm);
        assert_counts(vm,0,angled_first ? 1 : 0);
        assert_counts(vm,1,0);
        assert(state.active.size() == (angled_first ? 4 : 0));
        for (auto stamp : state.encounter_stamps) assert(stamp == (angled_first ? 1 : 0));
    }

    {
        Game::TriggerState state;
        Game::reset_trigger_state(state,level);
        std::uint32_t order = 0;
        script::Runtime vm;
        load_callbacks(state,vm,order);
        constexpr auto before_wrap = std::numeric_limits<std::uint32_t>::max()-50;
        Game::trace_trigger_state(state,collision,outside,touching,hull,1,before_wrap,vm);
        Game::repeat_trigger_state(state,level,49,vm); // unsigned elapsed = 100
        assert_counts(vm,2,0);
        order = 0;
        Game::repeat_trigger_state(state,level,50,vm); // unsigned elapsed = 101
        assert_counts(vm,2,1); assert(order == 4321);
        for (unsigned i = 0; i != 4; ++i) {
            assert(vm.data_word(name(i,2),4) == before_wrap);
            assert(state.last_ticks[i] == 50);
        }
        Game::repeat_trigger_state(state,level,50,vm);
        assert_counts(vm,2,1);
        Game::repeat_trigger_state(state,level,1050,vm); // no catch-up loop
        assert_counts(vm,2,2);
        for (auto tick : state.last_ticks) assert(tick == 1050);
    }

    {
        Level rounded_level = level;
        for (auto& trigger : rounded_level.triggers) trigger.repeat_interval_seconds = 16777.216f;
        const double threshold = 1000.0 * rounded_level.triggers[0].repeat_interval_seconds;
        assert(threshold > 16777216.0 && threshold < 16777217.0);
        Game::TriggerState state;
        Game::reset_trigger_state(state,rounded_level);
        CollisionWorld rounded(rounded_level);
        std::uint32_t order = 0;
        script::Runtime vm;
        load_callbacks(state,vm,order);
        Game::trace_trigger_state(state,rounded,outside,touching,hull,1,100,vm);
        // UInt32 16777217 rounds down to float 16777216 before comparison.
        Game::repeat_trigger_state(state,rounded_level,16777317,vm);
        assert_counts(vm,2,0);
        Game::repeat_trigger_state(state,rounded_level,16777318,vm);
        assert_counts(vm,2,1);
        for (auto tick : state.last_ticks) assert(tick == 16777318);
    }

    {
        Game::TriggerState state;
        Game::reset_trigger_state(state,level);
        std::uint32_t order = 0;
        script::Runtime vm;
        load_callbacks(state,vm,order);
        Game::use_trigger_state(state,collision,{0,0,0},1,vm,"_on_use");
        assert_counts(vm,3,1); assert(order == 1234);
        assert(state.active.empty());
        for (unsigned i = 0; i != 4; ++i) {
            assert(state.inside[i] == 0 && state.last_ticks[i] == 0);
            assert(state.encounter_stamps[i] == 0);
        }
        // Point use is not hull/AABB contact and never consumes a frame stamp.
        Game::use_trigger_state(state,collision,{.8f,.8f,0},1,vm,"_on_use");
        assert_counts(vm,3,1);
        order = 0;
        Game::use_trigger_state(state,collision,{0,0,0},1,vm,"_on_use");
        assert_counts(vm,3,2); assert(order == 1234);
        Game::trace_trigger_state(state,collision,outside,touching,hull,2,100,vm);
        Game::use_trigger_state(state,collision,{0,0,0},2,vm,"_on_use");
        assert_counts(vm,3,2); // this frame's successful transition gates use
        order = 0;
        Game::use_trigger_state(state,collision,{0,0,0},3,vm,"_on_use");
        assert_counts(vm,3,3); assert(order == 1234);
        assert(state.active.size() == 4);
        for (unsigned i = 0; i != 4; ++i) {
            assert(state.inside[i] == 1 && state.last_ticks[i] == 100);
            assert(state.encounter_stamps[i] == 2);
        }
        Game::repeat_trigger_state(state,level,200,vm);
        assert_counts(vm,2,0); // use did not advance the inside repeat clock
        Game::repeat_trigger_state(state,level,201,vm);
        assert_counts(vm,2,1);
    }

    {
        Level restricted = level;
        // Authored trigger VIS bounds are independent of the brush shape.
        // This record is visible only in BACK even though the convex brush
        // would contain the selected FRONT point.
        for (auto& trigger : restricted.triggers) trigger.bounds.maximum.x = -.1f;
        CollisionWorld restricted_collision(restricted);
        constexpr Vec3 point{.25f,0,0};
        assert(restricted_collision.intersects_brush(0,point,{}));
        assert(restricted_collision.leaf_at(point) == 0);
        Game::TriggerState state;
        Game::reset_trigger_state(state,restricted);
        assert((state.leaf_offsets == std::vector<std::size_t>{0,0,4}));
        std::uint32_t order = 0;
        script::Runtime vm;
        load_callbacks(state,vm,order);
        Game::use_trigger_state(state,restricted_collision,point,1,vm,"_on_use");
        assert_counts(vm,3,0); assert(order == 0);
        Game::use_trigger_state(state,restricted_collision,{-.25f,0,0},1,vm,"_on_use");
        assert_counts(vm,3,1); assert(order == 1234);
        assert(state.active.empty());
        for (auto stamp : state.encounter_stamps) assert(stamp == 0);
    }

    {
        // Real PO/PM construction and shared Game queries; no retail asset tree.
        namespace fs = std::filesystem;
        const fs::path temporary = fs::temp_directory_path() / ("pusu-game-shader-query-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (fs::exists(temporary)) throw std::runtime_error("shader check temporary path collision");
        struct TemporaryTree {
            fs::path path;
            ~TemporaryTree() { std::error_code ignored; fs::remove_all(path,ignored); }
        } cleanup{temporary};
        for (const auto directory : {"font","interface","object/po","object/pm"})
            fs::create_directories(temporary / directory);
        const auto put = [&](std::string_view name, std::string_view text) {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(temporary / name);
            file << text;
        };
        put("font/fonts.lst","");
        put("interface/interface.txt","");
        for (const auto page : {"ana_sayfa","ayarlar_kontroller",
                               "ayarlar_ses","cikis_onay","emegi_gecenler",
                               "oyun_yukle","yeni_oyun_onay"})
            put("interface/menu_" + std::string(page) + ".txt","");
        // Constructor-derived options need the original list order/defaults.
        // Labels preserve the retail menu's CP1254 bytes without font assets.
        put("interface/menu_ayarlar_goruntu.txt",
            "page_add_list list_kan\n"
            "list_add_item Kapal\xFD A\xE7\xFDk\n"
            "list_set_active_item 1\n"
            "page_add_list list_bulanik_hareket\n"
            "list_add_item A\xE7\xFDk Kapal\xFD\n"
            "list_set_active_item 1\n"
            "page_add_list list_bulanik_hareket_doku_boyutu\n"
            "list_add_item 128 256 512 1024\n"
            "list_set_active_item 1\n"
            "page_add_list list_bulanik_hareket_kare_sayisi\n"
            "list_add_item 2 3 4 5 6 7 8 9 10\n"
            "list_set_active_item 1\n"
            "page_add_list list_materyal_boyut\n"
            "list_add_item %6 %12 %25 %50 %100\n"
            "list_set_active_item 4\n"
            "page_add_list list_materyal_sikistirma\n"
            "list_add_item Kapal\xFD A\xE7\xFDk\n"
            "list_set_active_item 1\n"
            "page_add_list list_materyal_suzme\n"
            "list_add_item do\xF0rusal karesel k\xFC" "bik\n"
            "list_set_active_item 1\n"
            "page_add_list list_isik_boyut\n"
            "list_add_item %6 %12 %25 %50 %100\n"
            "list_set_active_item 4\n"
            "page_add_list list_isik_sikistirma\n"
            "list_add_item Kapal\xFD A\xE7\xFDk\n"
            "list_set_active_item 0\n"
            "page_add_list list_isik_suzme\n"
            "list_add_item do\xF0rusal karesel k\xFC" "bik\n"
            "list_set_active_item 1\n"
            "page_add_list list_yansima_boyut\n"
            "list_add_item Kapal\xFD %6 %12 %25 %50 %100\n"
            "list_set_active_item 5\n"
            "page_add_list list_yansima_sikistirma\n"
            "list_add_item Kapal\xFD A\xE7\xFDk\n"
            "list_set_active_item 1\n"
            "page_add_list list_yansima_suzme\n"
            "list_add_item do\xF0rusal karesel k\xFC" "bik\n"
            "list_set_active_item 1\n");
        put("object/po/query_initial.po","{\npart query_wall check/query_initial\n}\n");
        put("object/po/query_rebuilt.po","{\npart query_wall check/query_replacement\n}\n");
        put("object/po/query_world.po","{\npart query_wall check/query_world\n}\n");
        std::vector<std::uint8_t> mesh;
        for (const auto c : std::string_view("PUPM0300")) mesh.push_back(c);
        word(mesh,3); word(mesh,3); word(mesh,0);
        const auto f32 = [&](float value) { word(mesh,std::bit_cast<std::uint32_t>(value)); };
        for (float value : {-1.f,-100.f,-100.f,1.f,100.f,100.f}) f32(value);
        for (float value : {0.f,-100.f,-100.f,0.f,100.f,-100.f,0.f,0.f,100.f}) f32(value);
        for (unsigned i = 0; i != 6; ++i) f32(0);
        for (unsigned i = 0; i != 3; ++i) { f32(1); f32(0); f32(0); }
        for (std::uint8_t value : {0,0,1,0,2,0}) mesh.push_back(value);
        {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(temporary / "object/pm/query_wall.pm",std::ios::binary);
            file.write(reinterpret_cast<const char*>(mesh.data()),
                       static_cast<std::streamsize>(mesh.size()));
        }
        SDL_setenv("XDG_DATA_HOME",temporary.c_str(),1);
        SDL_setenv("XDG_CONFIG_HOME",temporary.c_str(),1);
        SDL_setenv("SDL_VIDEODRIVER","dummy",1);
        SDL_setenv("SDL_AUDIODRIVER","dummy",1);
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        struct Quit { ~Quit() { SDL_Quit(); } } quit;
        AssetStore assets(temporary);
        Settings settings;
        MaterialLibrary materials;
        materials.parse(
            "check/query_playerclip\n{\nsurfaceparm playerclip\n}\n"
            "check/query_nonsolid\n{\nsurfaceparm nonsolid\n}\n"
            "check/query_noimpact\n{\nsurfaceparm playerclip\nsurfaceparm noimpact\n}\n");
        Interface ui(assets,settings,materials);
        Media media(assets,settings);
        Game game(assets,materials,settings,ui,media);
        game.level_ = std::make_unique<Level>();
        game.level_->bounds = {{-200,-200,-200},{200,200,200}};
        game.level_->leaves.emplace_back();
        game.level_->leaves.front().bounds.bounds = game.level_->bounds;
        game.clear_membership();
        constexpr Vec3 start{-40,0,0}, end{40,0,0};
        const Bounds standing = CharacterMotionState{}.hull;
        const auto assert_queries = [&](std::uint32_t handle, std::string_view shader,
                                        bool blocks, bool impact) {
            const auto motion = game.trace_motion(start,{1,0,0},80,standing,
                                                  0,0x200u,7u,false,0,true);
            assert(motion.hit == blocks);
            if (blocks) {
                assert(motion.entity == handle && motion.part == 0);
                assert(motion.mesh == "query_wall" && motion.shader == shader);
                assert(motion.distance > 0 && motion.distance < 80 && motion.position.x < end.x);
            } else {
                assert(motion.distance == 80 && motion.position.x == end.x);
                assert(motion.position.y == end.y && motion.position.z == end.z);
            }
            const auto hit = game.trace_scene(start,end,0);
            assert(hit.hit == impact);
            if (impact) {
                assert(hit.entity == handle && hit.part == 0);
                assert(hit.mesh == "query_wall" && hit.shader == shader);
                assert(hit.distance > 0 && hit.distance < 80);
            } else {
                assert(hit.distance == 80 && hit.position.x == end.x);
                assert(hit.position.y == end.y && hit.position.z == end.z);
            }
        };
        const auto wall = game.host_create_entity("query_wall",0,"query_initial");
        assert_queries(wall,"check/query_initial",true,true);
        game.host_shader("query_wall","query_wall","check/query_replacement",false);
        assert_queries(wall,"check/query_replacement",false,true);
        // Shader construction is cached by name: returning to the initial one
        // retains its movement collision, even through runtime replacement.
        game.host_shader("query_wall","query_wall","check/query_initial",false);
        assert_queries(wall,"check/query_initial",true,true);
        game.host_shader("query_wall","query_wall","check/query_playerclip",false);
        assert_queries(wall,"check/query_playerclip",true,true);
        game.host_shader("query_wall","query_wall","check/query_nonsolid",false);
        assert_queries(wall,"check/query_nonsolid",false,true);
        game.host_shader("query_wall","query_wall","check/query_noimpact",false);
        assert_queries(wall,"check/query_noimpact",true,false);
        // Conversely, a shader first constructed by replacement remains
        // impact-only when a later PO initially references that same name.
        game.host_delete_entity(wall);
        const auto rebuilt = game.host_create_entity("query_rebuilt",0,"query_rebuilt");
        assert_queries(rebuilt,"check/query_replacement",false,true);
        // PL construction seeds the same name cache before PO construction:
        // zero-content world shaders stay impact-only on an initial PO part.
        game.host_delete_entity(rebuilt);
        game.level_->shaders.push_back({"check/query_world",0,0});
        game.collision_ = std::make_unique<CollisionWorld>(*game.level_,materials);
        const auto world_seeded = game.host_create_entity("query_world",0,"query_world");
        assert_queries(world_seeded,"check/query_world",false,true);
    }
}
}

int main() { pusu::game_trigger_check(); }
