#undef NDEBUG
#include "interface.hpp"
#include "resources.hpp"
#include "media.hpp"
#include "game.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <set>
#include <string>
#include <vector>
#include <tuple>

namespace {
using ResourceSet=std::set<std::tuple<std::string,bool,bool>>;
struct ResourceSnapshot {
    std::uint64_t revision;
    ResourceSet resources;
    explicit ResourceSnapshot(const pusu::Interface& ui):revision(ui.resource_revision()){
        // Own strings: draw_data is borrowed and the next rebuild may replace its keys.
        for(const auto& quad:ui.draw_data())if(!quad.shader.empty())
            resources.emplace(quad.shader,quad.font_atlas,quad.shader.find("textures")!=std::string_view::npos);
    }
};
struct ResourceSetCheck {
    ResourceSnapshot previous;
    explicit ResourceSetCheck(const pusu::Interface& ui):previous(ui){}
    bool observe(const pusu::Interface& ui){
        ResourceSnapshot current(ui);const bool changed=current.resources!=previous.resources;
        // Main compares revisions for inequality; extra invalidations and counter wrap are legal.
        assert(!changed||current.revision!=previous.revision);
        previous=std::move(current);return changed;
    }
};
}

int main(int argc,char** argv) {
    if(argc!=2){std::cerr<<"usage: interface-check ORIGINAL_ASSET_ROOT\n";return 2;}
    const auto temporary=std::filesystem::temp_directory_path()/("pusu-interface-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temporary);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{temporary};
    SDL_setenv("XDG_DATA_HOME",temporary.c_str(),1);SDL_setenv("XDG_CONFIG_HOME",temporary.c_str(),1);SDL_setenv("SDL_VIDEODRIVER","dummy",1);
    if(SDL_Init(SDL_INIT_VIDEO)!=0)throw std::runtime_error(SDL_GetError());
    struct Quit {~Quit(){SDL_Quit();}} quit;
    pusu::AssetStore assets(argv[1]);pusu::MaterialLibrary materials(assets);pusu::Settings settings;pusu::Interface ui(assets,settings,materials);ui.set_settings_file(temporary/"settings.cfg");
    {
        std::string heading_atlas,version_atlas;
        std::istringstream list(assets.text("font/fonts.lst"));std::string line,name,image;
        while(std::getline(list,line)){std::istringstream record(line);record>>std::quoted(name,'"', '\0')>>std::quoted(image,'"', '\0');if(name=="menu_ana_baslik_font")heading_atlas=image;if(name=="menu_secenekler")version_atlas=image;}
        assert(heading_atlas==R"(textures\3te_text\menu_ana_baslik.tga)"&&version_atlas==R"(textures\3te_text\menu_secenekler.tga)");
        const auto quads=ui.draw_data();
        const auto check_heading=[&](float x,float width,pusu::RenderRect uv){
            const auto found=std::find_if(quads.begin(),quads.end(),[&](const auto& quad){return quad.shader==heading_atlas&&quad.rect.x==x&&quad.rect.y==329;});
            assert(found!=quads.end());const auto& quad=*found;
            assert(quad.font_atlas&&quad.rect.width==width&&quad.rect.height==52);
            assert(quad.uv.x==uv.x&&quad.uv.y==uv.y&&quad.uv.width==uv.width&&quad.uv.height==uv.height);
            assert((quad.color==std::array<float,4>{.42f,.42f,.42f,.5f}));
            assert(!quad.drawable_pixel_coordinates&&!quad.full_viewport);
        };
        // Protected ana_sayfa: O/Y/E retain the authored 256x128 atlas coordinates.
        check_heading(75,17,{.765625f,0,.06640625f,.4140625f});
        check_heading(92,15,{.46875f,.421875f,.05859375f,.4140625f});
        check_heading(183,13,{.2109375f,0,.05078125f,.4140625f});
        bool version_found=false,cursor_found=false,image_found=false;
        for(const auto& quad:quads){
            if(quad.shader==heading_atlas||quad.shader==version_atlas){assert(quad.font_atlas);if(quad.shader==version_atlas){version_found=true;assert(quad.color[3]==.4f&&quad.rect.height==23);}}
            if(quad.shader=="mouse_icon"){cursor_found=true;assert(!quad.font_atlas);}
            if(quad.shader=="menu_background"){image_found=true;assert(!quad.font_atlas&&quad.rect.width==1024&&quad.rect.height==768);}
        }
        assert(version_found&&cursor_found&&image_found);
    }
    {
        pusu::Settings cursor_settings;pusu::Interface cursor(assets,cursor_settings,materials);pusu::HudState frame{};
        ResourceSetCheck resources(cursor);
        SDL_Event motion{};motion.type=SDL_MOUSEMOTION;
        const auto has_cursor=[](const pusu::Interface& value){const auto quads=value.draw_data();return std::any_of(quads.begin(),quads.end(),[](const auto& quad){return quad.shader=="mouse_icon"&&!quad.font_atlas;});};
        assert(has_cursor(cursor));assert(!resources.observe(cursor));
        cursor.input(motion,{0,0},true);cursor.update(0,frame);
        assert(!has_cursor(cursor)&&resources.observe(cursor));
        cursor.input(motion,{0,0},false);cursor.update(0,frame);
        // Main reads this revision before borrowing draw_data for Renderer.prepare.
        assert(has_cursor(cursor)&&resources.observe(cursor));
        cursor.input(motion,{1,1},false);cursor.update(0,frame);
        assert(has_cursor(cursor)&&!resources.observe(cursor));
        cursor.input(motion,{0,0},true);cursor.update(0,frame);
        assert(!has_cursor(cursor)&&resources.observe(cursor));
        cursor.input(motion,{0,0},false);cursor.update(0,frame);
        assert(has_cursor(cursor)&&resources.observe(cursor)); // A known resource returning still invalidates preparation.
    }
    {
        pusu::Settings escape_settings;pusu::Interface escape(assets,escape_settings,materials);
        escape.set_game_active(true);escape.hide_menu();
        int resumes=0;
        escape.set_action_handler([&](const pusu::MenuAction& value){
            if(value.name=="oyuna_devam"){
                assert(!escape.captures_input()&&escape.wants_relative_mouse());
                ++resumes;
            }
        });
        SDL_Event key{};key.key.keysym.scancode=SDL_SCANCODE_ESCAPE;
        for(const auto type:{SDL_KEYDOWN,SDL_KEYUP}){
            key.type=type;assert(!escape.input(key,{0,0},false));
            assert(!escape.captures_input()&&escape.wants_relative_mouse()&&resumes==0);
        }
        escape.show_menu("ana_sayfa"); // Gameplay owns opening; Interface owns visible-page Escape.
        key.type=SDL_KEYDOWN;assert(escape.input(key,{0,0},false));
        assert(escape.captures_input()&&!escape.wants_relative_mouse()&&resumes==0);
        key.type=SDL_KEYUP;assert(escape.input(key,{0,0},false));
        assert(!escape.captures_input()&&escape.wants_relative_mouse()&&resumes==1);
        assert(!escape.input(key,{0,0},false)&&resumes==1);
        escape.show_menu("ayarlar_ses"); // Authored ordinary back returns to ana_sayfa, not gameplay.
        key.type=SDL_KEYDOWN;assert(escape.input(key,{0,0},false));
        assert(escape.captures_input()&&!escape.wants_relative_mouse()&&resumes==1);
        key.type=SDL_KEYUP;assert(escape.input(key,{0,0},false));
        assert(escape.captures_input()&&!escape.wants_relative_mouse()&&resumes==1);
        key.type=SDL_KEYDOWN;assert(escape.input(key,{0,0},false));
        key.type=SDL_KEYUP;assert(escape.input(key,{0,0},false));
        assert(!escape.captures_input()&&escape.wants_relative_mouse()&&resumes==2);
        escape.set_game_active(false);escape.show_menu("ana_sayfa");
        assert(escape.input(key,{0,0},false));
        assert(escape.captures_input()&&!escape.wants_relative_mouse()&&resumes==2);
    }
    {
        // Real runtime consumer fixture; dummy audio is not audible-output proof.
        SDL_setenv("SDL_AUDIODRIVER","dummy",1);
        pusu::MaterialLibrary materials(assets);pusu::Settings runtime_settings;pusu::Interface runtime_ui(assets,runtime_settings,materials);
        pusu::Media media(assets,runtime_settings);
        pusu::Game game(assets,materials,runtime_settings,runtime_ui,media);
        runtime_ui.set_action_handler([&](const pusu::MenuAction& value){game.action(value);});
        struct UnbindGameActions {pusu::Interface& ui;~UnbindGameActions(){ui.set_action_handler({});}} unbind{runtime_ui};
        runtime_ui.set_media(media);
        pusu::game_save_check(game); // The guarded helper owns boot/new_game/first update.
    }
    pusu::VideoTexture subtitle(assets,"textures/3te_subtitle/Giris_demosu_bolum_1.avi",1000);assert(subtitle.frame().width>0&&subtitle.frame().height>0&&subtitle.frame().rgba.empty());
    subtitle.update_clock(1066);assert(subtitle.frame().rgba.empty());subtitle.update_clock(1067);assert(!subtitle.frame().rgba.empty());const auto first=subtitle.frame().seconds;assert(first>0);
    subtitle.update_clock(1133);assert(subtitle.frame().seconds==first);subtitle.update_clock(1134);assert(subtitle.frame().seconds>first);subtitle.update_clock(6676);assert(subtitle.frame().seconds==0);
    {const auto load=static_cast<std::uint32_t>(-51);pusu::VideoTexture tv(assets,"textures/3te_missions/tv_haydarpasa_1.avi",load);assert(tv.frame().rgba.empty());tv.update_clock(load+1);assert(!tv.frame().rgba.empty()&&tv.frame().seconds==0);tv.update_clock(16);assert(tv.frame().seconds>0);}
    std::array<Uint8,SDL_NUM_SCANCODES> keyboard{};
    keyboard[SDL_SCANCODE_W]=1;assert(ui.action_down("forward",keyboard,0));keyboard[SDL_SCANCODE_W]=0;
    keyboard[SDL_SCANCODE_P]=1;assert(ui.action_down("next_weapon",keyboard,0));keyboard[SDL_SCANCODE_P]=0;
    keyboard[SDL_SCANCODE_Q]=1;assert(!ui.action_down("next_weapon",keyboard,0));keyboard[SDL_SCANCODE_Q]=0;
    assert(ui.action_down("fire",keyboard,SDL_BUTTON_LMASK));assert(ui.action_down("aim",keyboard,SDL_BUTTON_RMASK));
    keyboard[SDL_SCANCODE_F11]=1;assert(ui.action_down("screenshot",keyboard,0));keyboard[SDL_SCANCODE_F11]=0;
    std::string action;ui.set_action_handler([&](const pusu::MenuAction& value){action=value.name;});
    SDL_Event event{};event.type=SDL_MOUSEBUTTONDOWN;event.button.button=SDL_BUTTON_LEFT;
    ui.input(event,{90,400},false);assert(action.empty());
    event.type=SDL_MOUSEBUTTONUP;ui.input(event,{90,400},true);assert(action.empty());
    ui.input(event,{90,400},false);assert(action=="yeni_oyun");
    ui.set_game_active(true);action.clear();ui.input(event,{90,400},false);assert(action=="yeni_oyun_onay_on_activate");
    ui.show_menu("ayarlar_kontroller");ui.input(event,{500,330},false);ui.update(0,{});
    event.type=SDL_MOUSEBUTTONDOWN;event.button.button=SDL_BUTTON_MIDDLE;ui.input(event,{500,330},false);ui.update(0,{});
    assert(ui.action_down("forward",keyboard,SDL_BUTTON_MMASK));assert(!ui.action_down("forward",keyboard,SDL_BUTTON_LMASK));
    event.type=SDL_MOUSEBUTTONUP;ui.input(event,{500,330},false);
    ui.set_selected_item("ayarlar_kontroller","list_kontroller_klavye_ileri",6);assert(ui.binding("forward")==SDL_SCANCODE_PERIOD);
    ui.set_selected_item("ayarlar_kontroller","list_kontroller_klavye_sola",24);
    event.type=SDL_KEYDOWN;event.key.keysym.scancode=SDL_SCANCODE_S;ui.input(event,{500,330},false);
    event.type=SDL_MOUSEBUTTONUP;event.button.button=SDL_BUTTON_LEFT;ui.input(event,{500,330},false);ui.update(0,{});ui.update(0,{});
    assert(ui.binding("forward")==SDL_SCANCODE_S&&ui.binding("left")==SDL_SCANCODE_PERIOD);
    event.type=SDL_KEYUP;event.key.keysym.scancode=SDL_SCANCODE_S;ui.input(event,{500,330},false);
    event.type=SDL_KEYDOWN;event.key.keysym.scancode=SDL_SCANCODE_ESCAPE;ui.input(event,{500,330},false);event.key.keysym.scancode=SDL_SCANCODE_A;ui.input(event,{500,330},false);
    event.type=SDL_MOUSEBUTTONUP;event.button.button=SDL_BUTTON_LEFT;ui.input(event,{500,330},false);ui.update(0,{});ui.update(0,{});assert(ui.binding("forward")==SDL_SCANCODE_S);
    event.type=SDL_KEYUP;event.key.keysym.scancode=SDL_SCANCODE_ESCAPE;ui.input(event,{500,330},false);ui.update(0,{});assert(ui.binding("forward")==SDL_SCANCODE_A);event.key.keysym.scancode=SDL_SCANCODE_A;ui.input(event,{500,330},false);
    settings.mouse_sensitivity=17;{pusu::Interface nearest(assets,settings,materials);nearest.set_settings_file(temporary/"settings.cfg");nearest.show_menu("ayarlar_kontroller");event.type=SDL_MOUSEBUTTONUP;event.button.button=SDL_BUTTON_LEFT;nearest.input(event,{900,250},false);assert(settings.mouse_sensitivity==30);}
    ui.hide_menu();ui.set_subtitle_text(0,"AB");ui.set_subtitle_quad("",true);ui.update(0,{});
    assert(ui.snapshot().subtitle_character==0);ui.update(.051f,{});assert(ui.snapshot().subtitle_character==1);
    ui.set_subtitle_quad("progress_bar_transparent",true);ui.update(0,{});for(const auto& quad:ui.draw_data())assert(quad.shader!="progress_bar_transparent");
    ui.set_fullscreen_quad("progress_bar_transparent",true);ui.update(0,{});bool fullscreen_found=false;for(const auto& quad:ui.draw_data())if(quad.shader=="progress_bar_transparent"){fullscreen_found=true;assert(quad.full_viewport&&quad.rect.width==1024&&quad.rect.height==768&&quad.uv.width==1&&quad.uv.height==1);}assert(fullscreen_found);ui.set_fullscreen_quad("",false);ui.update(0,{});
    ui.set_fade(true,2);ui.update(1,{});const auto saved=ui.snapshot();assert(saved.fade_alpha==.5f);ui.validate_state(saved);ui.restore_state(saved);
    int history_visible=0;for(const auto& quad:ui.draw_data())if(quad.history_capture_visible){++history_visible;assert(quad.shader.empty()&&quad.full_viewport&&quad.color[0]==0&&quad.color[3]==.5f);}assert(history_visible==1);
    auto invalid=saved;invalid.subtitle_row=5;bool rejected=false;try{ui.validate_state(invalid);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    {
        pusu::Settings fade_settings;pusu::Interface fade(assets,fade_settings,materials);
        fade.hide_menu();fade.show_hud(false);
        const auto check_fade=[&](float alpha){
            int count=0;
            for(const auto& quad:fade.draw_data())if(quad.history_capture_visible){
                ++count;assert(alpha>=0&&quad.shader.empty()&&quad.full_viewport&&!quad.font_atlas&&!quad.drawable_pixel_coordinates);
                assert((quad.color==std::array<float,4>{0,0,0,alpha}));
                assert(quad.rect.x==0&&quad.rect.y==0&&quad.rect.width==1024&&quad.rect.height==768);
                assert(quad.uv.x==0&&quad.uv.y==0&&quad.uv.width==1&&quad.uv.height==1);
            }
            assert(count==(alpha>=0?1:0));
        };
        fade.update(0,{});check_fade(-1);
        fade.set_fade(true,2);fade.update(.5f,{});check_fade(.25f);
        fade.set_fade_time(1);fade.update(0,{});check_fade(.5f);
        auto state=fade.snapshot();assert(state.fade_elapsed==.5f&&state.fade_in&&state.fade_duration==1);
        fade.update(1,{});check_fade(1); // fade_in clamps opaque without expiring or stopping its clock.
        fade.set_fade_time(2);fade.update(0,{});check_fade(.75f);
        state=fade.snapshot();assert(state.fade_elapsed==1.5f&&state.fade_in&&state.fade_duration==2);
        fade.set_fullscreen_quad("progress_bar",true);fade.update(0,{});check_fade(.75f);
        bool fullscreen=false;for(const auto& quad:fade.draw_data())if(quad.shader=="progress_bar"){fullscreen=true;assert(quad.full_viewport&&!quad.history_capture_visible);}assert(fullscreen);
        fade.set_fullscreen_quad("",false);fade.update(0,{});check_fade(.75f);
        fade.show_menu();fade.update(.5f,{});check_fade(-1);
        fade.hide_menu();fade.update(0,{});check_fade(1); // Only the menu gates the standalone black quad.

        fade.set_fade(false,1);fade.update(.25f,{});check_fade(.75f);
        fade.set_fade_time(.5f);fade.update(0,{});check_fade(.5f);
        state=fade.snapshot();assert(state.fade_elapsed==.25f&&!state.fade_in&&state.fade_duration==.5f);
        fade.update(.5f,{});check_fade(-1);
        fade.update(.75f,{});check_fade(-1); // Expired fade_out still ages while it emits no quad.
        fade.set_fade_time(1);fade.update(0,{});check_fade(-1); // Longer duration still below actual age cannot revive an expired fade.
        fade.set_fade_time(2);fade.update(0,{});check_fade(.25f);
        state=fade.snapshot();assert(state.fade_elapsed==1.5f&&!state.fade_in&&state.fade_duration==2);
        fade.update(.5f,{});check_fade(0);fade.update(.25f,{});check_fade(-1);

        fade.set_fade(true,1);fade.update(2,{});check_fade(1);
        fade.set_fade(false,.5f);fade.update(0,{});check_fade(1);
        state=fade.snapshot();assert(state.fade_elapsed==0&&!state.fade_in&&state.fade_duration==.5f);
        fade.update(.25f,{});check_fade(.5f);fade.update(.5f,{});check_fade(-1);
        fade.set_fade(false,.5f); // Level reset precedes the authored first frame; it is not a veto.
        fade.set_fade(true,1);fade.update(0,{});check_fade(0);
        state=fade.snapshot();assert(state.fade_elapsed==0&&state.fade_in&&state.fade_duration==1);
        fade.update(.25f,{});check_fade(.25f);fade.update(1,{});check_fade(1);
    }
    pusu::HudState hud;hud.health=.25f;hud.scope_visible=true;hud.scope_angle_degrees=30;ui.update(0,hud);
    int scope_count=0,margin_masks=0;bool health_found=false;for(const auto& quad:ui.draw_data()){if(quad.shader.starts_with("scope_"))++scope_count;if(quad.mask_legacy_margins)++margin_masks;if(quad.shader=="health_panel_health"){health_found=true;assert(std::abs(quad.rect.x-940)<.01f&&std::abs(quad.rect.y-711)<.01f&&std::abs(quad.rect.width-64)<.01f&&std::abs(quad.rect.height-32)<.01f&&quad.uv.y==.75f&&quad.uv.height==.25f);}if(quad.shader=="scope_inner_left_highlight")assert(quad.rotation_degrees==30);if(quad.shader=="scope_glass_right_highlight")assert(quad.rotation_degrees==-30);}assert(health_found&&scope_count==17&&margin_masks==1);
    hud.scope_visible=false;hud.health=.75f;hud.crosshair=true;hud.camera_blocked=true;hud.game_tick=1500;hud.camera_transition_tick=1000;ui.show_hud(true);
    for(int frame=0;frame<2;++frame){ui.update(3,hud);int bars=0;bool visible_health=false;for(const auto& quad:ui.draw_data()){if(quad.native_bar_fraction){++bars;assert(*quad.native_bar_fraction==.5f);}assert(quad.shader!="crosshair_single");if(quad.shader=="health_panel_health")visible_health=true;}assert(bars==2&&visible_health);}
    hud.game_tick=250;hud.camera_transition_tick=static_cast<std::uint32_t>(-250);ui.update(0,hud);for(const auto& quad:ui.draw_data())if(quad.native_bar_fraction)assert(*quad.native_bar_fraction==.5f);
    ui.set_loading("weapon_panel_gun_pistol_cz75",.5f,true);ui.update(0,hud);assert(ui.captures_input());const auto loading=ui.draw_data();assert(loading.size()==3&&loading[0].shader=="weapon_panel_gun_pistol_cz75"&&loading[1].shader=="progress_bar_transparent"&&loading[2].shader=="progress_bar");for(const auto& quad:loading)assert(quad.full_viewport&&!quad.font_atlas);assert(loading[2].uv.width==.5f&&std::abs(loading[2].rect.width-409.6f)<.01f);
    ui.set_loading("weapon_panel_gun_pistol_cz75",.75f,true);ui.set_loading("",1,false);ui.update(0,hud);assert(!ui.captures_input());
    const auto console_count=[](const pusu::Interface& value){const auto quads=value.draw_data();return std::count_if(quads.begin(),quads.end(),[](const auto& quad){return quad.drawable_pixel_coordinates;});};
    std::string font_shader,font_metrics;
    float font_spacing{},font_sx{},font_sy{};
    {std::istringstream list(assets.text("font/fonts.lst"));std::string line,name;while(std::getline(list,line)){std::istringstream record(line);record>>std::quoted(name,'"', '\0');if(name=="notification_font"){assert(record>>std::quoted(font_shader,'"', '\0')>>std::quoted(font_metrics,'"', '\0')>>font_spacing>>font_sx>>font_sy);break;}}}
    assert(!font_shader.empty()&&font_spacing==0&&font_sx==1&&font_sy==1);
    const auto font_atlas=assets.bytes(font_shader);assert(font_atlas.size()>=18);
    const float atlas_width=font_atlas[12]+256*font_atlas[13],atlas_height=font_atlas[14]+256*font_atlas[15];
    std::array<pusu::RenderRect,256> glyphs{};float font_height{};
    {std::istringstream metrics(assets.text(font_metrics));assert(metrics>>font_height);int code;float x,y,width;while(metrics>>code>>x>>y>>width)glyphs[static_cast<unsigned char>(code)]={x,y,width,font_height};assert(metrics.eof());}
    assert(font_height==22&&glyphs['A'].width>0&&glyphs['B'].width>0&&glyphs['C'].width>0);
    const auto check_glyph=[&](const pusu::InterfaceQuad& quad,unsigned char code,float x,float y){
        const auto& glyph=glyphs[code];
        assert(quad.font_atlas&&quad.drawable_pixel_coordinates&&quad.full_viewport&&!quad.history_capture_visible);
        assert(quad.shader==font_shader&&(quad.color==std::array<float,4>{1,1,0,1}));
        assert(quad.rect.x==x&&quad.rect.y==y&&quad.rect.width==glyph.width&&quad.rect.height==font_height-1);
        assert(quad.uv.x==glyph.x/atlas_width&&quad.uv.y==glyph.y/atlas_height&&quad.uv.width==glyph.width/atlas_width&&quad.uv.height==font_height/atlas_height);
    };
    {
        pusu::Settings console_settings;pusu::Interface console(assets,console_settings,materials);pusu::HudState frame{};
        ResourceSetCheck resources(console);
        console.update(0,frame);assert(console_count(console)==0&&!resources.observe(console));
        console.show_menu();console.show_hud(true);console.update(0,frame);assert(!resources.observe(console));
        frame.game_tick=40000;console.update(0,frame);assert(console_count(console)==0&&!resources.observe(console));
        console.console_warning("A",0);console.console_warning("B",0);
        frame.game_tick=70000;console.update(0,frame);assert(console_count(console)==2);resources.observe(console);
        const auto ordered=console.draw_data();assert(ordered.size()>2);check_glyph(ordered[0],'A',5,0);check_glyph(ordered[1],'B',5,22);
        for(std::size_t i=2;i<ordered.size();++i)assert(!ordered[i].drawable_pixel_coordinates);
        console.hide_menu();console.show_hud(false);frame.game_tick=100000;console.update(0,frame);assert(console_count(console)==2);resources.observe(console);
        frame.game_tick=200000;console.update(0,frame);
        assert(console_count(console)==2&&!resources.observe(console)); // Builds never start the lifetime.
        const auto borrowed=console.draw_data();const std::vector<pusu::InterfaceQuad> saved_quads(borrowed.begin(),borrowed.end());
        std::vector<std::string> saved_shaders;for(const auto& quad:borrowed)saved_shaders.emplace_back(quad.shader);
        console.console_warning("C",200000);console.console_presented(200000);
        assert(console.draw_data().data()==borrowed.data()&&console.draw_data().size()==borrowed.size());
        const auto same_rect=[](const auto& a,const auto& b){return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;};
        for(std::size_t i=0;i<borrowed.size();++i){
            const auto& quad=borrowed[i];const auto& saved_quad=saved_quads[i];
            assert(same_rect(quad.rect,saved_quad.rect)&&same_rect(quad.uv,saved_quad.uv)&&quad.color==saved_quad.color);
            assert(quad.shader.data()==saved_quad.shader.data()&&quad.shader==saved_shaders[i]);
            assert(quad.rotation_degrees==saved_quad.rotation_degrees&&quad.rotation_center.x==saved_quad.rotation_center.x&&quad.rotation_center.y==saved_quad.rotation_center.y);
            assert(quad.mask_legacy_margins==saved_quad.mask_legacy_margins&&quad.full_viewport==saved_quad.full_viewport&&quad.native_bar_fraction==saved_quad.native_bar_fraction);
            assert(quad.history_capture_visible==saved_quad.history_capture_visible&&quad.drawable_pixel_coordinates==saved_quad.drawable_pixel_coordinates&&quad.font_atlas==saved_quad.font_atlas);
        }
        frame.game_tick=230000;console.update(0,frame);assert(console_count(console)==3&&!resources.observe(console));
        console.console_presented(230000);frame.game_tick=230001;console.update(0,frame);
        assert(console_count(console)==1&&!resources.observe(console));check_glyph(console.draw_data()[0],'C',5,0);
        frame.game_tick=260000;console.update(0,frame);assert(console_count(console)==1&&!resources.observe(console));
        frame.game_tick=260001;console.update(0,frame);assert(console_count(console)==0);resources.observe(console);
        frame.game_tick=300000;console.update(0,frame);assert(!resources.observe(console));
    }
    {
        pusu::Settings console_settings;pusu::Interface console(assets,console_settings,materials);pusu::HudState frame{};
        console.hide_menu();console.show_hud(false);console.console_warning("A",0);console.console_presented(0);
        frame.game_tick=40000;console.update(0,frame);assert(console_count(console)==1); // Enqueue/present without a build cannot stamp.
    }
    for(const std::uint32_t first_tick:{0u,static_cast<std::uint32_t>(-100)}){
        pusu::Settings console_settings;pusu::Interface console(assets,console_settings,materials);pusu::HudState frame{};
        console.hide_menu();console.show_hud(false);console.console_warning("A",first_tick);
        frame.game_tick=first_tick;console.update(0,frame);console.console_presented(first_tick);
        frame.game_tick=first_tick+30000u;console.update(0,frame);assert(console_count(console)==1);
        frame.game_tick=first_tick+30001u;console.update(0,frame);assert(console_count(console)==0);
    }
    {
        pusu::Settings console_settings;pusu::Interface console(assets,console_settings,materials);pusu::HudState frame{};
        console.hide_menu();console.show_hud(false);console.console_warning("A",0);console.update(0,frame);assert(console_count(console)==1);
        console.set_loading("weapon_panel_gun_pistol_cz75",.5f,true);frame.game_tick=40000;console.update(0,frame);
        assert(console.draw_data().size()==3&&console_count(console)==0);console.console_presented(40000);
        console.set_loading("",1,false);frame.game_tick=80000;console.update(0,frame);assert(console_count(console)==1);
        console.console_presented(80000);frame.game_tick=110000;console.update(0,frame);assert(console_count(console)==1);
        frame.game_tick=110001;console.update(0,frame);assert(console_count(console)==0);
    }
    {
        pusu::Settings console_settings;pusu::Interface console(assets,console_settings,materials);pusu::HudState frame{};
        console.hide_menu();console.show_hud(false);console.set_drawable_size(1024,1);
        for(int i=0;i<80;++i)console.console_warning("A",0);
        frame.game_tick=100000;console.update(0,frame);assert(console_count(console)==1);console.console_presented(100000);
        console.set_drawable_size(1024,2048);frame.game_tick=130000;console.update(0,frame);
        assert(console_count(console)==80);
        for(std::size_t i=0;i<80;++i)check_glyph(console.draw_data()[i],'A',5,22*static_cast<float>(i));
        console.set_drawable_size(1024,1);frame.game_tick=130001;console.update(0,frame);assert(console_count(console)==0);
        console.set_drawable_size(1024,2048);console.update(0,frame);assert(console_count(console)==0); // Offscreen records were also stamped and expired.
    }
    {
        pusu::Settings console_settings;pusu::Interface console(assets,console_settings,materials);pusu::HudState frame{};
        console.hide_menu();console.show_hud(false);console.set_drawable_size(10000,768);
        std::string owned(300,'A');console.console_warning(owned,0);owned.assign(300,'B');console.console_warning("B",0);
        console.console_warning(std::string(1,static_cast<char>(0xfd)),0);
        frame.game_tick=50000;console.update(0,frame);assert(console_count(console)==302);
        float x=5;for(std::size_t i=0;i<300;++i){check_glyph(console.draw_data()[i],'A',x,0);x=std::trunc(x+glyphs['A'].width+font_spacing);}
        check_glyph(console.draw_data()[300],'B',5,22);check_glyph(console.draw_data()[301],0xfd,5,44);
        console.set_drawable_size(static_cast<unsigned>(2*glyphs['A'].width),22);console.update(0,frame);
        assert(console_count(console)==4);
        for(std::size_t i=0;i<3;++i)check_glyph(console.draw_data()[i],'A',5+glyphs['A'].width*static_cast<float>(i),0);
        check_glyph(console.draw_data()[3],'B',5,22); // Pen-origin clipping, no wrapping, and top==height is retained.
    }
    {
        pusu::Settings resource_settings;pusu::Interface value(assets,resource_settings,materials);pusu::HudState frame{};
        ResourceSetCheck resources(value);
        const auto rebuild=[&](float seconds=0){value.update(seconds,frame);return resources.observe(value);};
        const auto contains=[&](std::string_view shader,bool font=false){
            return resources.previous.resources.contains({std::string(shader),font,shader.find("textures")!=std::string_view::npos});
        };
        assert(!rebuild()); // Constructor already publishes its first complete draw set.
        value.hide_menu();value.show_hud(false);assert(!resources.observe(value));assert(rebuild());
        assert(resources.previous.resources.empty());assert(!rebuild());

        std::string menu_atlas;
        {std::istringstream list(assets.text("font/fonts.lst"));std::string line,name,image;while(std::getline(list,line)){std::istringstream record(line);record>>std::quoted(name,'"', '\0')>>std::quoted(image,'"', '\0');if(name=="menu_secenekler")menu_atlas=image;}}
        assert(!menu_atlas.empty());
        value.set_subtitle_text(0,"");value.set_subtitle_quad("",true);assert(!rebuild());
        value.set_subtitle_text(0,"A");assert(rebuild()&&contains(menu_atlas,true));
        value.set_subtitle_text(0,"AB");assert(!rebuild());assert(!rebuild(.051f));
        value.set_subtitle_quad("progress_bar_transparent",true);assert(!rebuild());
        assert(!contains("progress_bar_transparent")); // Stored subtitle material never emits a quad.
        value.set_fullscreen_quad(menu_atlas,true);assert(rebuild());
        assert(resources.previous.resources.size()==2&&contains(menu_atlas)&&contains(menu_atlas,true));
        value.set_subtitle_quad("",false);assert(rebuild()&&resources.previous.resources.size()==1);
        const auto material_only=resources.previous.resources;
        value.set_fullscreen_quad("",false);value.set_subtitle_quad("",true);assert(rebuild());
        assert(resources.previous.resources.size()==material_only.size()&&contains(menu_atlas,true)&&!contains(menu_atlas));
        value.set_subtitle_quad("",false);assert(rebuild()&&resources.previous.resources.empty());

        value.set_fullscreen_quad("progress_bar",true);assert(rebuild());
        const auto one_material_count=resources.previous.resources.size();
        value.set_fullscreen_quad("progress_bar_transparent",true);assert(rebuild());
        assert(resources.previous.resources.size()==one_material_count); // Same count, different identity.
        value.set_fullscreen_quad(menu_atlas,true);assert(rebuild()&&contains(menu_atlas));
        std::string case_variant=menu_atlas;assert(case_variant.starts_with("textures"));case_variant[0]='T';
        value.set_fullscreen_quad(case_variant,true);assert(rebuild()&&contains(case_variant));
        assert(std::get<2>(*resources.previous.resources.begin())==false); // Renderer context is case-sensitive.
        value.set_fullscreen_quad(menu_atlas,true);assert(rebuild()&&contains(menu_atlas));
        value.set_fullscreen_quad("",false);assert(rebuild()&&resources.previous.resources.empty());

        value.set_loading("weapon_panel_gun_pistol_cz75",0,true);assert(rebuild()&&resources.previous.resources.size()==3);
        for(float progress:{.25f,.75f,1.0f}){value.set_loading("weapon_panel_gun_pistol_cz75",progress,true);assert(!rebuild());const auto quads=value.draw_data();assert(quads.size()==3&&quads[2].uv.width==progress);}
        value.show_menu("ayarlar_goruntu");value.show_hud(true);frame.scope_visible=true;frame.notification="A";
        assert(!rebuild()); // Loading hides all changed menu/HUD resources.
        value.set_loading("progress_bar_transparent",.5f,true);assert(rebuild()&&resources.previous.resources.size()==2);
        value.set_loading("weapon_panel_gun_pistol_cz75",.5f,true);assert(rebuild()&&resources.previous.resources.size()==3);
        value.set_loading("",1,false);value.hide_menu();frame={};assert(rebuild());
        frame.health=.7f;assert(!rebuild());
        frame.health=.25f;assert(!rebuild());assert(rebuild(.5f)&&contains("health_panel_base_red"));
        frame.health=1;assert(rebuild()&&contains("health_panel_base_yellow"));
        frame.crosshair=true;frame.camera_blocked=true;assert(!rebuild()&&!contains("crosshair_single"));
        frame.camera_blocked=false;assert(rebuild()&&contains("crosshair_single"));
        frame.camera_blocked=true;assert(rebuild()&&!contains("crosshair_single"));
        frame.crosshair=false;assert(!rebuild());frame.camera_blocked=false;assert(!rebuild());
        frame.scope_visible=true;frame.crosshair=true;assert(rebuild()&&contains("scope_circle")&&!contains("crosshair_single"));
        frame.scope_angle_degrees=45;assert(!rebuild());frame.crosshair=false;assert(!rebuild());
        frame.scope_visible=false;assert(rebuild()&&!contains("scope_circle"));
        frame.damage_direction=0;assert(rebuild()&&contains("take_hit_top"));
        frame.damage_elapsed=.2f;assert(!rebuild());
        frame.damage_elapsed=.6f;assert(rebuild()&&!contains("take_hit_top"));
        frame.damage_elapsed=.1f;assert(rebuild()&&contains("take_hit_top"));
        const auto damage_count=resources.previous.resources.size();
        frame.damage_direction=1;assert(rebuild()&&contains("take_hit_bottom")&&resources.previous.resources.size()==damage_count);
        frame.damage_direction=8;assert(rebuild()&&contains("take_hit_all"));
        frame.damage_direction=9;assert(!rebuild());frame.damage_direction=10;assert(rebuild());
        frame.weapon_shader="weapon_panel_gun_pistol_cz75";frame.weapon_name="";frame.magazine=5;frame.ammunition=20;
        assert(rebuild()&&contains(frame.weapon_shader));
        frame.weapon_name="A";assert(rebuild()&&contains(menu_atlas,true));
        frame.weapon_name="AB";frame.magazine=12;frame.ammunition=99;assert(!rebuild());
        const auto weapon_count=resources.previous.resources.size();
        frame.weapon_shader="weapon_panel_gun_rifle_ak101";frame.weapon_name="AK-101";
        assert(rebuild()&&contains(frame.weapon_shader)&&resources.previous.resources.size()==weapon_count);
        value.show_panel("weapon_name",false);assert(rebuild()&&!contains(menu_atlas,true));
        value.show_panel("weapon_name",true);assert(rebuild()&&contains(menu_atlas,true));
        frame.weapon_shader="";assert(rebuild());
        frame.notification="A";assert(rebuild()&&contains(font_shader,true));
        value.console_warning("A",0);assert(!rebuild()&&console_count(value)==1);
        value.console_presented(0);frame.game_tick=30001;assert(!rebuild()&&console_count(value)==0);
        frame.notification="";assert(rebuild()&&!contains(font_shader,true));
        value.show_hud(false);assert(rebuild()&&resources.previous.resources.empty());
        value.set_fade(true,2);assert(!rebuild(1));assert(!rebuild(1)); // Empty-shader fades/bars are not resources.
        const auto state=value.snapshot();value.restore_state(state);assert(!rebuild());
        auto overlay_state=state;overlay_state.fullscreen_shader="progress_bar";overlay_state.fullscreen_visible=true;
        value.restore_state(overlay_state);assert(rebuild()&&contains("progress_bar"));
        value.restore_state(state);assert(rebuild()&&resources.previous.resources.empty());
        value.set_control_shader("frame_background_pusu","progress_bar");assert(!rebuild());
        value.set_control_shader("frame_background_pusu","menu_background_pusu");assert(!rebuild()); // Hidden-page edits do not invalidate.

        for(const auto page:{"ana_sayfa","ayarlar_goruntu","ayarlar_kontroller","ayarlar_ses","cikis_onay","emegi_gecenler","oyun_yukle","yeni_oyun_onay"}){
            value.show_menu(page);rebuild();assert(!rebuild());value.show_menu(page);assert(!rebuild());
        }
        value.show_menu("ana_sayfa");rebuild();
        value.set_text("label_versiyon","");assert(rebuild()&&!contains(menu_atlas,true));
        value.set_text("label_versiyon","A");assert(rebuild()&&contains(menu_atlas,true));
        value.set_text("label_versiyon","AB");assert(!rebuild());
        const auto menu_count=resources.previous.resources.size();
        // Mutators can replace shader-string storage backing the previous borrowed draw list.
        value.set_control_shader("frame_background_pusu","progress_bar");
        assert(rebuild()&&contains("progress_bar")&&!contains("menu_background_pusu")&&resources.previous.resources.size()==menu_count);
        value.set_control_shader("frame_background_pusu","menu_background_pusu");
        assert(rebuild()&&contains("menu_background_pusu")&&!contains("progress_bar"));
        value.set_control_enabled("ayarlar",false);assert(!rebuild());value.set_control_enabled("ayarlar",true);assert(!rebuild());
        value.show_menu("ayarlar_goruntu");rebuild();
        SDL_Event motion{};motion.type=SDL_MOUSEMOTION;
        const auto help_count=[&]{const auto quads=value.draw_data();return std::count_if(quads.begin(),quads.end(),[&](const auto& quad){return quad.font_atlas&&quad.shader==menu_atlas&&quad.rect.y==700;});};
        value.input(motion,{0,0},false);assert(!rebuild()&&help_count()==0);
        value.input(motion,{380,230},false);assert(!rebuild()&&help_count()>0);
        value.input(motion,{381,230},false);assert(!rebuild()&&help_count()>0);
        value.input(motion,{0,0},false);assert(!rebuild()&&help_count()==0);
        value.show_menu("oyun_yukle");rebuild();
        const std::array<std::string,2> labels{"A","AB"};
        value.set_save_slots(labels);assert(!rebuild());value.set_selected_item("oyun_yukle","list_oyun_yukle_checkpoint",1);assert(!rebuild());
        value.set_save_slots(std::span<const std::string>{});assert(!rebuild());
        value.hide_menu();assert(rebuild());value.show_menu("oyun_yukle");assert(rebuild());assert(!rebuild());
    }
    std::cout<<"Protected original menu/actions/bindings, subtitle/fade/console and exact UI resource-set revision closure checked\n";
}
