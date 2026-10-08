#include "interface.hpp"
#include "resources.hpp"
#include "media.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <limits>

namespace pusu {
namespace {
std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> result;
    for(std::size_t i=0;i<line.size();) {
        while(i<line.size()&&(line[i]==' '||line[i]=='\t'||line[i]=='\r'))++i;
        if(i==line.size()||line.substr(i,2)=="//")break;
        if(line[i]=='"') {
            if(line.substr(i,3)=="\"\"\""){result.emplace_back("\"");i+=3;continue;}
            const auto start=++i;while(i<line.size()&&line[i]!='"')++i;
            if(i==line.size())throw std::runtime_error("Unterminated original UI string");
            result.emplace_back(line.substr(start,i-start));++i;
        }else {const auto start=i;while(i<line.size()&&line[i]!=' '&&line[i]!='\t'&&line[i]!='\r')++i;result.emplace_back(line.substr(start,i-start));}
    }
    return result;
}
float number(std::string_view value) {
    if(!value.empty()&&value.front()=='+')value.remove_prefix(1);
    float result{};const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||!std::isfinite(result))throw std::runtime_error("Invalid original UI number: "+std::string(value));return result;
}
std::string_view canonical(std::string_view original) {
    constexpr std::pair<std::string_view,std::string_view> names[]={{"ileri","forward"},{"geri","back"},{"sola","left"},{"saga","right"},{"ates","fire"},{"zipla","jump"},{"egil","crouch"},{"yuru","walk"},{"doldur","reload"},{"kullan","use"},{"durbun","aim"},{"tabanca","weapon_1"},{"tufek","weapon_2"},{"degistir","next_weapon"},{"at","drop_weapon"},{"goruntu_al","screenshot"}};
    for(auto [name,action]:names)if(original==name)return action;return original;
}
SDL_Scancode key_for(std::size_t original_index) {
    // 004864dc selector IDs + 0043de80 DirectInput codes: physical keys, not host-layout glyphs.
    static constexpr std::array<SDL_Scancode,103> keys{
        SDL_SCANCODE_UNKNOWN,SDL_SCANCODE_UNKNOWN,SDL_SCANCODE_UNKNOWN,SDL_SCANCODE_A,SDL_SCANCODE_B,SDL_SCANCODE_C,SDL_SCANCODE_PERIOD,SDL_SCANCODE_D,
        SDL_SCANCODE_E,SDL_SCANCODE_F,SDL_SCANCODE_G,SDL_SCANCODE_LEFTBRACKET,SDL_SCANCODE_H,SDL_SCANCODE_I,SDL_SCANCODE_APOSTROPHE,SDL_SCANCODE_J,
        SDL_SCANCODE_K,SDL_SCANCODE_L,SDL_SCANCODE_M,SDL_SCANCODE_N,SDL_SCANCODE_O,SDL_SCANCODE_COMMA,SDL_SCANCODE_P,SDL_SCANCODE_R,
        SDL_SCANCODE_S,SDL_SCANCODE_SEMICOLON,SDL_SCANCODE_T,SDL_SCANCODE_U,SDL_SCANCODE_RIGHTBRACKET,SDL_SCANCODE_V,SDL_SCANCODE_Y,SDL_SCANCODE_Z,
        SDL_SCANCODE_Q,SDL_SCANCODE_W,SDL_SCANCODE_X,SDL_SCANCODE_1,SDL_SCANCODE_2,SDL_SCANCODE_3,SDL_SCANCODE_4,SDL_SCANCODE_5,
        SDL_SCANCODE_6,SDL_SCANCODE_7,SDL_SCANCODE_8,SDL_SCANCODE_9,SDL_SCANCODE_0,SDL_SCANCODE_F1,SDL_SCANCODE_F2,SDL_SCANCODE_F3,
        SDL_SCANCODE_F4,SDL_SCANCODE_F5,SDL_SCANCODE_F6,SDL_SCANCODE_F7,SDL_SCANCODE_F8,SDL_SCANCODE_F9,SDL_SCANCODE_F10,SDL_SCANCODE_F11,
        SDL_SCANCODE_F12,SDL_SCANCODE_GRAVE,SDL_SCANCODE_SLASH,SDL_SCANCODE_BACKSLASH,SDL_SCANCODE_TAB,SDL_SCANCODE_LSHIFT,SDL_SCANCODE_RSHIFT,SDL_SCANCODE_LCTRL,
        SDL_SCANCODE_RCTRL,SDL_SCANCODE_LALT,SDL_SCANCODE_RALT,SDL_SCANCODE_SPACE,SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_HOME,SDL_SCANCODE_END,SDL_SCANCODE_INSERT,SDL_SCANCODE_DELETE,SDL_SCANCODE_PAGEUP,SDL_SCANCODE_PAGEDOWN,SDL_SCANCODE_PRINTSCREEN,SDL_SCANCODE_SCROLLLOCK,
        SDL_SCANCODE_PAUSE,SDL_SCANCODE_KP_PLUS,SDL_SCANCODE_KP_MINUS,SDL_SCANCODE_KP_MULTIPLY,SDL_SCANCODE_KP_DIVIDE,SDL_SCANCODE_KP_PERIOD,SDL_SCANCODE_KP_0,SDL_SCANCODE_KP_1,
        SDL_SCANCODE_KP_2,SDL_SCANCODE_KP_3,SDL_SCANCODE_KP_4,SDL_SCANCODE_KP_5,SDL_SCANCODE_KP_6,SDL_SCANCODE_KP_7,SDL_SCANCODE_KP_8,SDL_SCANCODE_KP_9,
        SDL_SCANCODE_MINUS,SDL_SCANCODE_EQUALS,SDL_SCANCODE_RETURN,SDL_SCANCODE_KP_ENTER,SDL_SCANCODE_BACKSPACE,SDL_SCANCODE_LGUI,SDL_SCANCODE_RGUI
    };
    if(original_index>=keys.size())throw std::runtime_error("Original physical key selector outside103-entry table");
    return keys[original_index];
}
constexpr std::array<std::uint8_t,103> selected_dik{
    0,0,0,0x1e,0x30,0x2e,0x34,0x20,0x12,0x21,0x22,0x1a,0x23,0x17,0x28,0x24,
    0x25,0x26,0x32,0x31,0x18,0x33,0x19,0x13,0x1f,0x27,0x14,0x16,0x1b,0x2f,0x15,0x2c,
    0x10,0x11,0x2d,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x3b,0x3c,0x3d,
    0x3e,0x3f,0x40,0x41,0x42,0x43,0x44,0x57,0x58,0x29,0x35,0x2b,0x0f,0x2a,0x36,0x1d,
    0x9d,0x38,0xb8,0x39,0xc8,0xd0,0xcb,0xcd,0xc7,0xcf,0xd2,0xd3,0xc9,0xd1,0xb7,0x46,
    0xc5,0x4e,0x4a,0x37,0xb5,0x53,0x52,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49,
    0x0c,0x0d,0x1c,0x9c,0x0e,0xdb,0xdc
};
bool inside(Vec2 p,InterfaceRect r){return p.x>=r.x&&p.y>=r.y&&p.x<r.x+r.width&&p.y<r.y+r.height;}
}
Interface::Interface(const AssetStore& assets,Settings& settings,const MaterialLibrary& materials):assets_(assets),settings_(settings),materials_(materials) {
    initialize_materials();
    quads_.reserve(8192);load_fonts();load_hud();
    int key_count{};const auto* initial_keys=SDL_GetKeyboardState(&key_count);std::copy_n(initial_keys,std::min<std::size_t>(key_count,key_state_.size()),key_state_.begin());buttons_=SDL_GetMouseState(nullptr,nullptr);
    SDL_DisplayMode desktop;if(SDL_GetDesktopDisplayMode(0,&desktop)!=0)throw std::runtime_error(SDL_GetError());desktop_width_=desktop.w;
    for(const auto page:{"ana_sayfa","ayarlar_goruntu","ayarlar_kontroller","ayarlar_ses","cikis_onay","emegi_gecenler","oyun_yukle","yeni_oyun_onay"})load_page(page);
    auto& controls=pages_.at("ayarlar_kontroller").controls;
    const auto controls_path=user_data_directory()/"controls.cfg";
    if(std::filesystem::exists(controls_path)) {
        std::ifstream file(controls_path);if(!file)throw std::runtime_error("Cannot read "+controls_path.string());
        std::string name;int selected;
        while(file>>name>>selected){Control* found=nullptr;for(auto& [page,definition]:pages_)for(auto& c:definition.controls)if(c.name==name&&!c.items.empty())found=&c;if(!found||selected<0||selected>=static_cast<int>(found->items.size()))throw std::runtime_error("Invalid persisted control selection "+name);found->selected=selected;}
        if(!file.eof())throw std::runtime_error("Malformed persisted control selections");
    }
    for(auto& c:controls)if(c.focus)update_binding(c);
    for(auto& c:controls)if(c.name=="list_fare_dikey_hareket")invert_mouse_=c.selected!=0;
    for(auto& c:controls)if(c.name=="list_fare_duyarlilik"){float distance=std::numeric_limits<float>::max();for(std::size_t i=0;i<c.items.size();++i){const float delta=std::abs(number(c.items[i].text)-settings_.mouse_sensitivity);if(delta<distance){distance=delta;c.selected=static_cast<int>(i);}}}
    for(auto& c:pages_.at("ayarlar_ses").controls) {
        if(c.name=="list_efekt")c.selected=std::clamp(static_cast<int>(std::lround(settings_.sfx_volume*10)),0,10);
        if(c.name=="list_muzik")c.selected=std::clamp(static_cast<int>(std::lround(settings_.music_volume*10)),0,10);
    }
    auto& graphics=pages_.at("ayarlar_goruntu").controls;
    for(auto& c:graphics) {
        if(c.name=="list_parlaklik")c.selected=std::clamp(static_cast<int>(std::lround((settings_.gamma-0.5f)*10)),0,15);
        if(c.name=="list_ekran_boyutu") {
            for(int display=0;display<SDL_GetNumVideoDisplays();++display)for(int mode=0;mode<SDL_GetNumDisplayModes(display);++mode){SDL_DisplayMode m;if(SDL_GetDisplayMode(display,mode,&m)!=0)throw std::runtime_error(SDL_GetError());const std::string label=std::to_string(m.w)+"x"+std::to_string(m.h);if(std::none_of(c.items.begin(),c.items.end(),[&](const Item& item){return item.text==label;}))c.items.push_back({label});}
            const std::string current=std::to_string(settings_.width)+"x"+std::to_string(settings_.height);auto found=std::find_if(c.items.begin(),c.items.end(),[&](const Item& i){return i.text==current;});if(found==c.items.end()){c.items.push_back({current});c.selected=static_cast<int>(c.items.size()-1);}else c.selected=static_cast<int>(found-c.items.begin());
        }
    }
    update_graphics_options();
    active_page_="ana_sayfa";rebuild({});
}
void Interface::initialize_materials() const {
    // 0045f0d0 constructs all fourteen in this order, independently of scope drawing.
    for(const auto shader:{"scope_circle","scope_circle_outer","scope_inner_left_highlight","scope_inner_right_highlight","scope_glass_left_highlight","scope_glass_right_highlight","scope_leftline_thick","scope_leftline_slim","scope_rightline_thick","scope_rightline_slim","scope_topline_thick","scope_topline_slim","scope_bottomline_thick","scope_bottomline_slim"})
        materials_.construct(shader,0,0,false);
}
void Interface::load_fonts() {
    std::istringstream list(assets_.text("font/fonts.lst"));std::string line;
    while(std::getline(list,line)) {
        const auto t=tokens(line);if(t.empty())continue;if(t.size()!=6)throw std::runtime_error("Invalid original font list record");
        Font f;f.shader=t[1];const auto atlas=assets_.bytes(t[1]);if(atlas.size()<18)throw std::runtime_error("Truncated font atlas TGA");
        f.atlas_width=atlas[12]+256*atlas[13];f.atlas_height=atlas[14]+256*atlas[15];if(f.atlas_width<=0||f.atlas_height<=0)throw std::runtime_error("Invalid font atlas dimensions");
        const int spacing=static_cast<int>(number(t[3]));const float sx=number(t[4]),sy=number(t[5]);
        f.spacing=static_cast<float>(spacing);
        f.glyphs.fill({0,0,1,1,1,1});
        std::istringstream metrics(assets_.text(t[2]));float height;if(!(metrics>>height)||height<=0)throw std::runtime_error("Invalid original font line height");f.height=height*sy;
        int code;float x,y,width;
        while(metrics>>code>>x>>y>>width){if(code<-128||code>255||x<0||y<0||width<0||x+width>f.atlas_width||y+height>f.atlas_height)throw std::runtime_error("Invalid original font glyph");f.glyphs[static_cast<unsigned char>(code)]={x/f.atlas_width,y/f.atlas_height,width*sx,height*sy,width/f.atlas_width,height/f.atlas_height};}
        if(!metrics.eof())throw std::runtime_error("Truncated original font metric record");fonts_.insert_or_assign(t[0],std::move(f));
    }
}
float Interface::text_width(std::string_view name,std::string_view value,Vec2 scale)const {
    const Font* font=nullptr;for(const auto& [key,f]:fonts_)if(key==name){font=&f;break;}if(!font)throw std::runtime_error("Unknown original UI font "+std::string(name));
    float width=0,maximum=0;for(unsigned char c:value){if(c=='\n'){maximum=std::max(maximum,width-font->spacing);width=0;}else width=std::trunc(width+font->glyphs[c].width*scale.x+font->spacing);}return value.empty()?0:std::trunc(std::max(maximum,width-font->spacing));
}
void Interface::text(std::string_view name,std::string_view value,Vec2 pos,std::array<float,4> color,Vec2 scale,bool legacy_bottom_inclusive) {
    for(std::size_t i=0;i<3;++i)if(color[i]>1)color[i]/=255;
    const Font* font=nullptr;for(const auto& [key,f]:fonts_)if(key==name){font=&f;break;}if(!font)throw std::runtime_error("Unknown original UI font "+std::string(name));
    const float left=pos.x;for(unsigned char c:value){if(c=='\n'){pos.x=left;pos.y+=font->height*scale.y;continue;}const auto& g=font->glyphs[c];if(g.width>0){quads_.push_back({{pos.x,pos.y,g.width*scale.x,g.height*scale.y-(legacy_bottom_inclusive?1.0f:0.0f)},{g.u,g.v,g.source_width,g.source_height},color,font->shader});quads_.back().font_atlas=true;}pos.x=std::trunc(pos.x+g.width*scale.x+font->spacing);}
}
void Interface::console_warning(std::string_view line,std::uint32_t) {
    console_lines_.push_back({std::string(line),std::nullopt});
}
void Interface::set_drawable_size(unsigned width,unsigned height) {
    if(width&&height){drawable_width_=width;drawable_height_=height;}
}
void Interface::console_presented(std::uint32_t tick) {
    // Only the last normal build's traversed prefix participated in this swap.
    const auto count=std::min(console_pending_count_,console_lines_.size());
    for(std::size_t i=0;i<count;++i)if(!console_lines_[i].first_render_tick)console_lines_[i].first_render_tick=tick;
    console_pending_count_=0;
}
void Interface::rebuild_console(std::uint32_t tick) {
    if(!console_lines_.empty()){
        const Font* font=nullptr;for(const auto& [name,f]:fonts_)if(name=="notification_font"){font=&f;break;}
        if(!font)throw std::runtime_error("Unknown original UI font notification_font");
        float y=0;
        for(auto line=console_lines_.begin();line!=console_lines_.end();){
            if(line->first_render_tick&&static_cast<std::uint32_t>(tick-*line->first_render_tick)>30000u){line=console_lines_.erase(line);continue;}
            if(y<=drawable_height_){
                float x=5;
                for(unsigned char c:line->text){
                    // 00442210 checks the current pen relative to the console origin.
                    if(x-5>drawable_width_)break;
                    const auto& g=font->glyphs[c];
                    // 004423d2 subtracts one from the bottom, not the row advance or UV.
                    if(g.width>0){quads_.push_back({{x,y,g.width,g.height-1},{g.u,g.v,g.source_width,g.source_height},{1,1,0,1},font->shader});quads_.back().font_atlas=true;quads_.back().full_viewport=true;quads_.back().drawable_pixel_coordinates=true;}
                    x=std::trunc(x+g.width+font->spacing);
                }
            }
            y=std::trunc(y+font->height);++line;
        }
    }
    console_pending_count_=console_lines_.size();
}
void Interface::load_page(std::string_view name) {
    Page page;page.name=name;Control* control=nullptr;float previous_y{};
    std::istringstream file(assets_.text("interface/menu_"+std::string(name)+".txt"));std::string line;
    while(std::getline(file,line)) {
        const auto t=tokens(line);if(t.empty())continue;const auto& cmd=t[0];
        auto count=[&](std::size_t n){if(t.size()!=n)throw std::runtime_error("Original UI arity: "+cmd);};
        if(cmd=="page_new"||cmd=="page_set_active"){count(2);if(t[1]!=name)throw std::runtime_error("Original UI page identity mismatch");continue;}
        if(cmd=="page_set_back_page"){count(2);page.back=t[1];continue;}
        if(cmd=="page_add_button"||cmd=="page_add_list"||cmd=="page_add_list_can_focus"){count(2);page.controls.emplace_back();control=&page.controls.back();control->name=t[1];control->list=cmd!="page_add_button";control->focus=cmd=="page_add_list_can_focus";continue;}
        if(cmd=="page_set_active_control"){count(2);auto found=std::find_if(page.controls.begin(),page.controls.end(),[&](const Control& c){return c.name==t[1];});if(found==page.controls.end())throw std::runtime_error("Unknown original active control");control=&*found;continue;}
        if(!control)throw std::runtime_error("UI property before active control");auto& c=*control;
        if(cmd=="control_set_xy"){count(3);c.rect.x=number(t[1]);c.rect.y=number(t[2])+(t[2].starts_with('+')?previous_y:0);previous_y=c.rect.y;}
        else if(cmd=="control_set_width"){count(2);c.rect.width=t[1]=="text_width"?text_width(c.font,c.text):number(t[1]);}
        else if(cmd=="control_set_height"){count(2);c.rect.height=number(t[1]);}
        else if(cmd=="control_set_shader"){count(2);c.shader=t[1];}
        else if(cmd=="control_set_z"){count(2);c.z=number(t[1]);}
        else if(cmd=="control_set_font"){count(2);c.font=t[1];}
        else if(cmd=="control_set_text"){count(2);c.text=t[1];}
        else if(cmd=="control_set_activate_page"){count(2);c.target=t[1];}
        else if(cmd=="control_set_help_text"){count(2);c.help=t[1];}
        else if(cmd=="control_cant_scroll"){count(1);c.scroll=false;}
        else if(cmd=="control_set_frame"){count(5);c.frame=true;c.frame_shader=t[1];c.frame_rect={number(t[4]),0,number(t[2]),number(t[3])};}
        else if(cmd=="control_set_color_normal"||cmd=="control_set_color_over"||cmd=="control_set_color_disabled"){if(t.size()!=4&&t.size()!=5)throw std::runtime_error("Invalid UI color");auto& color=cmd=="control_set_color_normal"?c.normal:cmd=="control_set_color_over"?c.over:c.disabled;color={number(t[1]),number(t[2]),number(t[3]),t.size()==5?number(t[4]):1};}
        else if(cmd=="list_add_item"){for(std::size_t i=1;i<t.size();++i)c.items.push_back({t[i]});}
        else if(cmd=="list_set_item_volumes"){if(t.size()!=c.items.size()+1)throw std::runtime_error("List volume count mismatch");for(std::size_t i=0;i<c.items.size();++i)c.items[i].volume=static_cast<int>(number(t[i+1]));}
        else if(cmd=="list_set_active_item"){count(2);c.selected=static_cast<int>(number(t[1]));}
        else throw std::runtime_error("Unsupported original UI command "+cmd);
    }
    for(const auto& c:page.controls)if(!c.items.empty()&&(c.selected<0||c.selected>=static_cast<int>(c.items.size())))throw std::runtime_error("Original UI selection out of bounds");
    pages_.emplace(page.name,std::move(page));
}
void Interface::load_hud() {
    std::istringstream file(assets_.text("interface/interface.txt"));std::string line,root_name;std::vector<std::size_t> stack;
    const InterfaceRect canvas{0,0,1024,768};
    const auto layout=[](Panel& p,InterfaceRect parent){
        const auto edge=[&](const Edge& e){switch(e.anchor){case 0:return parent.x+(e.spring?parent.width*e.amount*.01f:e.amount);case 1:return parent.x+parent.width-(e.spring?parent.width*e.amount*.01f:e.amount)-1;case 2:return parent.y+(e.spring?parent.height*e.amount*.01f:e.amount);case 3:return parent.y+parent.height-(e.spring?parent.height*e.amount*.01f:e.amount)-1;default:throw std::runtime_error("Invalid original HUD anchor");}};
        const float left=edge(p.edges[0]),right=edge(p.edges[1]),top=edge(p.edges[2]),bottom=edge(p.edges[3]);p.rect={left,top,std::abs(right-left)+1,std::abs(bottom-top)+1};
    };
    while(std::getline(file,line)){
        const auto t=tokens(line);if(t.empty())continue;
        if(t[0]=="{"){Panel panel;if(stack.empty())panel.name=root_name;layout(panel,stack.empty()?canvas:panels_[stack.back()].rect);stack.push_back(panels_.size());panels_.push_back(std::move(panel));continue;}
        if(t[0]=="}"){if(stack.empty())throw std::runtime_error("Unexpected HUD closing brace");stack.pop_back();continue;}
        if(stack.empty()){if(t.size()!=1)throw std::runtime_error("Invalid HUD root name");root_name=t[0];continue;}
        auto& p=panels_[stack.back()];if(t[0]=="shader"){p.shader=t.at(1);continue;}if(t[0]=="name"){p.name=t.at(1);continue;}if(t[0]=="font"){p.font=t.at(1);continue;}
        if(t[0]=="color"){p.color={number(t.at(1)),number(t.at(2)),number(t.at(3)),t.size()==5?number(t[4]):1};continue;}
        if(t.size()!=4)throw std::runtime_error("Unsupported HUD property "+t[0]);const auto parent=stack.size()==1?canvas:panels_[stack[stack.size()-2]].rect;
        int side;if(t[0]=="leftside")side=0;else if(t[0]=="rightside")side=1;else if(t[0]=="topside")side=2;else if(t[0]=="bottomside")side=3;else throw std::runtime_error("Unsupported HUD edge "+t[0]);
        auto& e=p.edges[side];e.amount=number(t[3]);e.spring=t[2]=="spring";if(!e.spring&&t[2]!="bar")throw std::runtime_error("Unsupported HUD edge mode");
        if(t[1]=="leftedge")e.anchor=0;else if(t[1]=="rightedge")e.anchor=1;else if(t[1]=="topedge")e.anchor=2;else if(t[1]=="bottomedge")e.anchor=3;
        // Original lefttedge typo deliberately leaves the constructor's anchor untouched.
        layout(p,parent);
    }
    if(!stack.empty())throw std::runtime_error("Unbalanced original HUD panels");
}
void Interface::set_action_handler(std::function<void(const MenuAction&)> handler){handler_=std::move(handler);}
void Interface::set_media(Media& value){media_=&value;media_->sound("sound/menu_music.ogg",1,true,{},false,4,true);media_->pause_game(menu_visible_);}
void Interface::show_menu(std::string_view page){auto found=pages_.find(std::string(page));if(found==pages_.end())throw std::runtime_error("Unknown original menu page "+std::string(page));if(!menu_visible_&&media_){media_->pause_game(true);media_->sound("sound/menu_music.ogg",1,true,{},false,4,true);}active_page_=page;menu_visible_=true;focus_=-1;scroll_=0;binding_=nullptr;if(page=="emegi_gecenler"){credits_interval_=33;credits_paused_=false;}if(handler_)handler_({std::string(page)+"_on_activate",{},-1});}
void Interface::hide_menu(){if(menu_visible_&&media_)media_->pause_game(false);menu_visible_=false;binding_=nullptr;focus_=-1;}
void Interface::set_text(std::string_view name,std::string_view value){for(auto& p:panels_)if(p.name==name)p.text=value;for(auto& [page,definition]:pages_)for(auto& c:definition.controls)if(c.name==name)c.text=value;}
void Interface::show_panel(std::string_view name,bool visible){for(auto& p:panels_)if(p.name==name)p.visible=visible;}
void Interface::set_save_slots(std::span<const std::string> labels){set_list_items("oyun_yukle","list_oyun_yukle_checkpoint",labels);}
void Interface::set_list_items(std::string_view page,std::string_view name,std::span<const std::string> labels) {
    for(auto& c:pages_.at(std::string(page)).controls)if(c.name==name){c.items.clear();for(const auto& label:labels)c.items.push_back({label});c.selected=std::clamp(c.selected,0,std::max(0,static_cast<int>(c.items.size())-1));return;}
    throw std::runtime_error("Unknown original list "+std::string(name));
}
int Interface::selected_item(std::string_view page,std::string_view name) const {
    for(const auto& c:pages_.at(std::string(page)).controls)if(c.name==name)return c.items.empty()?-1:c.selected;
    throw std::runtime_error("Unknown original list "+std::string(name));
}
void Interface::set_selected_item(std::string_view page,std::string_view name,int index){for(auto& c:pages_.at(std::string(page)).controls)if(c.name==name){if(index<0||index>=static_cast<int>(c.items.size()))throw std::runtime_error("Original list selection outside bounds");c.selected=index;if(c.focus)update_binding(c);if(page=="ayarlar_goruntu")update_graphics_options();return;}throw std::runtime_error("Unknown original list "+std::string(name));}
void Interface::set_control_enabled(std::string_view name,bool enabled){for(auto& [page,definition]:pages_)for(auto& c:definition.controls)if(c.name==name){c.enabled=enabled;c.enabled_explicit=true;}}
void Interface::set_control_shader(std::string_view name,std::string_view shader) {
    for(auto& [page,definition]:pages_)for(auto& c:definition.controls)if(c.name==name&&c.shader!=shader)c.shader=shader;
}
void Interface::set_fullscreen_quad(std::string_view shader,bool visible){fullscreen_shader_=shader;fullscreen_visible_=visible;}
void Interface::set_loading(std::string_view shader,float progress,bool visible){if(!std::isfinite(progress)||progress<0||progress>1||(visible&&shader.empty()))throw std::runtime_error("Invalid original loading overlay");loading_shader_=shader;loading_progress_=progress;loading_visible_=visible;if(visible)binding_=nullptr;}
void Interface::set_subtitle_quad(std::string_view shader,bool visible){subtitle_shader_=shader;subtitle_visible_=visible;if(visible){subtitle_row_=subtitle_character_=0;subtitle_row_finished_=false;subtitle_clock_=elapsed_;}}
void Interface::set_subtitle_text(int line,std::string_view value){if(line<0||line>=4||value.size()>255)throw std::runtime_error("Original subtitle row exceeds its256-byte buffer");subtitle_lines_[line]=value;}
void Interface::set_fade(bool fade_in,float duration){set_fade_time(duration);fade_in_=fade_in;fade_elapsed_=0;fade_alpha_=fade_in?0:1;fading_=true;}
void Interface::set_fade_time(float duration){if(!std::isfinite(duration)||duration<=0)throw std::runtime_error("Invalid original fade duration");fade_duration_=duration;}
void Interface::update_graphics_options() {
    const auto index=[&](std::string_view name){return selected_item("ayarlar_goruntu",name);};OriginalGraphicsOptions value;
    value.motion_blur=index("list_bulanik_hareket")==0;value.motion_blur_size=128<<index("list_bulanik_hareket_doku_boyutu");value.motion_blur_frames=2+index("list_bulanik_hareket_kare_sayisi");
    value.texture_divisor=1<<(4-index("list_materyal_boyut"));value.lightmap_divisor=1<<(4-index("list_isik_boyut"));const int reflection=index("list_yansima_boyut");value.reflections=reflection!=0;value.reflection_divisor=1<<(5-reflection);
    value.texture_compression=index("list_materyal_sikistirma")!=0;value.lightmap_compression=index("list_isik_sikistirma")!=0;value.reflection_compression=index("list_yansima_sikistirma")!=0;
    value.texture_filter=index("list_materyal_suzme")+1;value.lightmap_filter=index("list_isik_suzme")+1;value.reflection_filter=index("list_yansima_suzme")+1;graphics_options_=value;blood_enabled_=index("list_kan")!=0;
}
InterfaceSnapshot Interface::snapshot()const{return {fullscreen_shader_,subtitle_shader_,subtitle_lines_,fade_duration_,fade_elapsed_,fade_alpha_,subtitle_clock_-elapsed_,subtitle_row_,subtitle_character_,fullscreen_visible_,subtitle_visible_,fade_in_,fading_,subtitle_row_finished_,hud_visible_};}
void Interface::validate_state(const InterfaceSnapshot& value)const {
    if(value.subtitle_row>4||value.subtitle_character>255||!std::isfinite(value.fade_duration)||value.fade_duration<0||!std::isfinite(value.fade_elapsed)||value.fade_elapsed<0||!std::isfinite(value.fade_alpha)||value.fade_alpha<0||value.fade_alpha>1||!std::isfinite(value.subtitle_delay)||(value.fading&&value.fade_duration==0))throw std::runtime_error("Invalid saved original interface state");
    for(const auto& line:value.subtitle_lines)if(line.size()>255)throw std::runtime_error("Saved original subtitle exceeds256-byte row");
}
void Interface::restore_state(const InterfaceSnapshot& value){validate_state(value);fullscreen_shader_=value.fullscreen_shader;subtitle_shader_=value.subtitle_shader;subtitle_lines_=value.subtitle_lines;fade_duration_=value.fade_duration;fade_elapsed_=value.fade_elapsed;fade_alpha_=value.fade_alpha;subtitle_clock_=elapsed_+value.subtitle_delay;subtitle_row_=value.subtitle_row;subtitle_character_=value.subtitle_character;fullscreen_visible_=value.fullscreen_visible;subtitle_visible_=value.subtitle_visible;fade_in_=value.fade_in;fading_=value.fading;subtitle_row_finished_=value.subtitle_row_finished;hud_visible_=value.hud_visible;}
void Interface::update_binding(Control& c) {
    if(c.selected<0||c.selected>=std::max(1,static_cast<int>(c.items.size())))throw std::runtime_error("Invalid control binding");const auto name=std::string_view(c.name).substr(std::string_view("list_kontroller_klavye_").size());Binding binding;
    if(c.selected<3)binding.mouse=SDL_BUTTON(c.selected==0?SDL_BUTTON_LEFT:c.selected==1?SDL_BUTTON_MIDDLE:SDL_BUTTON_RIGHT);else binding.key=key_for(c.selected);
    bindings_.insert_or_assign(std::string(canonical(name)),binding);
}
void Interface::poll_binding() {
    auto& current=*binding_;const int old=current.selected;unsigned first=256;
    for(std::size_t i=3;i<selected_dik.size();++i)if(key_state_[key_for(i)])first=std::min(first,static_cast<unsigned>(selected_dik[i]));
    // Unlisted PC keys still win the original ascending physical DIK scan and block keyboard capture.
    constexpr std::pair<SDL_Scancode,unsigned> unlisted[]={{SDL_SCANCODE_ESCAPE,1},{SDL_SCANCODE_CAPSLOCK,0x3a},{SDL_SCANCODE_NUMLOCKCLEAR,0x45},{SDL_SCANCODE_NONUSBACKSLASH,0x56},{SDL_SCANCODE_F13,0x64},{SDL_SCANCODE_F14,0x65},{SDL_SCANCODE_F15,0x66},{SDL_SCANCODE_INTERNATIONAL1,0x73},{SDL_SCANCODE_INTERNATIONAL2,0x70},{SDL_SCANCODE_INTERNATIONAL3,0x7d},{SDL_SCANCODE_INTERNATIONAL4,0x79},{SDL_SCANCODE_INTERNATIONAL5,0x7b},{SDL_SCANCODE_LANG1,0xf2},{SDL_SCANCODE_LANG2,0xf1},{SDL_SCANCODE_APPLICATION,0xdd},{SDL_SCANCODE_POWER,0xde},{SDL_SCANCODE_SLEEP,0xdf},{SDL_SCANCODE_AUDIOPREV,0x90},{SDL_SCANCODE_AUDIONEXT,0x99},{SDL_SCANCODE_MUTE,0xa0},{SDL_SCANCODE_AUDIOMUTE,0xa0},{SDL_SCANCODE_CALCULATOR,0xa1},{SDL_SCANCODE_AUDIOPLAY,0xa2},{SDL_SCANCODE_AUDIOSTOP,0xa4},{SDL_SCANCODE_VOLUMEDOWN,0xae},{SDL_SCANCODE_VOLUMEUP,0xb0},{SDL_SCANCODE_AC_HOME,0xb2},{SDL_SCANCODE_AC_SEARCH,0xe5},{SDL_SCANCODE_AC_BOOKMARKS,0xe6},{SDL_SCANCODE_AC_REFRESH,0xe7},{SDL_SCANCODE_AC_STOP,0xe8},{SDL_SCANCODE_AC_FORWARD,0xe9},{SDL_SCANCODE_AC_BACK,0xea},{SDL_SCANCODE_COMPUTER,0xeb},{SDL_SCANCODE_MAIL,0xec},{SDL_SCANCODE_MEDIASELECT,0xed}};
    for(const auto [key,dik]:unlisted)if(key_state_[key])first=std::min(first,dik);
    bool changed=false;const auto apply=[&](int ordinal){current.selected=std::min(ordinal,std::max(0,static_cast<int>(current.items.size())-1));for(auto& c:pages_.at("ayarlar_kontroller").controls)if(c.focus)update_binding(c);if(media_)media_->environment_sound("effect","mouse_click","menu",1,{},false,false);binding_=nullptr;changed=true;};
    for(std::size_t i=3;i<selected_dik.size();++i)if(selected_dik[i]==first){apply(static_cast<int>(i));break;}
    constexpr Uint32 mouse_bits[]={SDL_BUTTON_LMASK,SDL_BUTTON_MMASK,SDL_BUTTON_RMASK};for(int i=0;i<3;++i)if(buttons_&mouse_bits[i])apply(i);
    for(auto& c:pages_.at("ayarlar_kontroller").controls)if(c.focus&& &c!=&current&&c.selected==current.selected){c.selected=std::min(old,std::max(0,static_cast<int>(c.items.size())-1));update_binding(c);}
    if(changed)persist_controls();
}
void Interface::update_hover() {
    auto& controls=pages_.at(active_page_).controls;const int old=focus_;focus_=-1;float z=-1;
    if(!outside_)for(std::size_t i=0;i<controls.size();++i){const auto& c=controls[i];auto rect=c.rect;if(c.scroll&&active_page_=="emegi_gecenler")rect.y=std::trunc(rect.y+scroll_+.5f);if(c.enabled&&c.z>=0&&c.z>z&&inside(mouse_,rect)){focus_=static_cast<int>(i);z=c.z;}}
    if(focus_!=old&&focus_>=0&&media_){const auto& c=controls[focus_];if(!c.target.empty()||c.list||c.name=="yeni_oyun"||c.name=="oyuna_devam"||c.name.ends_with("_evet")||c.name=="default"||c.name=="oyun_yukle_yukle")media_->environment_sound("effect","mouse_over","menu",1,{},false,false);}
}
SDL_Scancode Interface::binding(std::string_view action)const{for(const auto& [name,b]:bindings_)if(name==action)return b.key;return SDL_SCANCODE_UNKNOWN;}
bool Interface::action_down(std::string_view action,std::span<const Uint8> keyboard,Uint32 buttons)const{for(const auto& [name,b]:bindings_)if(name==action)return b.mouse?(buttons&b.mouse)!=0:b.key!=SDL_SCANCODE_UNKNOWN&&static_cast<std::size_t>(b.key)<keyboard.size()&&keyboard[b.key];return false;}
void Interface::persist_controls(){const auto directory=user_data_directory();std::filesystem::create_directories(directory);const auto path=directory/"controls.cfg",temporary=directory/"controls.cfg.tmp";std::ofstream out(temporary,std::ios::trunc);if(!out)throw std::runtime_error("Cannot persist original menu settings");for(const auto page:{"ayarlar_kontroller","ayarlar_goruntu"})for(const auto& c:pages_.at(page).controls)if(!c.items.empty()&&c.name!="list_ekran_boyutu"&&c.name!="list_parlaklik")out<<c.name<<' '<<c.selected<<'\n';out.close();if(!out)throw std::runtime_error("Cannot persist original menu settings");std::filesystem::rename(temporary,path);}
void Interface::activate(Control& c) {
    if(!c.enabled)return;
    if(c.focus){binding_=&c;binding_wait_=true;return;}
    if(c.name=="yeni_oyun"&&game_active_){if(media_)media_->environment_sound("effect","alert","menu",1,{},false,false);show_menu("yeni_oyun_onay");return;}
    if(!c.target.empty()){const std::string target=c.target;show_menu(target);return;}
    if(c.name=="default") {
        const auto source=assets_.text("interface/menu_ayarlar_kontroller.txt");std::istringstream file(source);std::string line,name;
        while(std::getline(file,line)){const auto t=tokens(line);if(t.empty())continue;if(t[0]=="page_set_active_control")name=t[1];if(t[0]=="list_set_active_item")for(auto& control:pages_.at("ayarlar_kontroller").controls)if(control.name==name){control.selected=static_cast<int>(number(t[1]));if(control.focus)update_binding(control);}}
        invert_mouse_=false;settings_.mouse_sensitivity=0;persist_controls();write_settings(settings_file_,settings_);
    }
    if(!c.items.empty()) {
        const auto& item=c.items[c.selected];
        if(c.name=="list_efekt"){settings_.sfx_volume=item.volume/100.0f;if(media_)media_->apply_settings();}
        else if(c.name=="list_muzik"){settings_.music_volume=item.volume/100.0f;if(media_)media_->apply_settings();}
        else if(c.name=="list_parlaklik")settings_.gamma=1+number(item.text)/100;
        else if(c.name=="list_fare_dikey_hareket"){invert_mouse_=c.selected!=0;persist_controls();}
        else if(c.name=="list_fare_duyarlilik"){settings_.mouse_sensitivity=number(item.text);persist_controls();}
        else if(c.name=="list_ekran_boyutu"){const auto split=item.text.find('x');settings_.width=static_cast<int>(number(std::string_view(item.text).substr(0,split)));settings_.height=static_cast<int>(number(std::string_view(item.text).substr(split+1)));}
        if(active_page_=="ayarlar_goruntu"){update_graphics_options();persist_controls();}
        write_settings(settings_file_,settings_);if(handler_)handler_({c.name,item.text,c.selected});
    }else if(handler_) {
        int index=-1;if(c.name=="oyun_yukle_yukle")for(const auto& list:pages_.at("oyun_yukle").controls)if(list.name=="list_oyun_yukle_checkpoint")index=list.items.empty()?-1:list.selected;
        handler_({c.name,{},index});
    }
}
bool Interface::input(const SDL_Event& event,Vec2 mouse,bool outside) {
    mouse_=mouse;outside_=outside;
    if(event.type==SDL_KEYDOWN||event.type==SDL_KEYUP){const int code=event.key.keysym.scancode;if(code>=0&&code<SDL_NUM_SCANCODES)key_state_[code]=event.type==SDL_KEYDOWN;}
    if(event.type==SDL_MOUSEBUTTONDOWN||event.type==SDL_MOUSEBUTTONUP){const Uint32 bit=event.button.button>=1&&event.button.button<=32?Uint32{1}<<(event.button.button-1):0;if(event.type==SDL_MOUSEBUTTONDOWN)buttons_|=bit;else buttons_&=~bit;}
    if(event.type==SDL_WINDOWEVENT&&event.window.event==SDL_WINDOWEVENT_FOCUS_LOST){key_state_.fill(0);buttons_=0;}
    if(loading_visible_)return true;
    if(event.type==SDL_KEYUP&&event.key.keysym.scancode==SDL_SCANCODE_ESCAPE) {
        if(binding_)return true;
        if(!menu_visible_)return false;
        const auto back=pages_.at(active_page_).back;if(back=="#RESUME"){if(game_active_){hide_menu();if(handler_)handler_({"oyuna_devam",{},-1});}}else show_menu(back);return true;
    }
    if(!menu_visible_)return false;
    auto& controls=pages_.at(active_page_).controls;
    if(binding_)return true;
    if(event.type==SDL_MOUSEMOTION||event.type==SDL_MOUSEBUTTONUP)update_hover();
    if(event.type==SDL_MOUSEBUTTONUP&&!outside&&event.button.button==SDL_BUTTON_LEFT&&focus_>=0) {
        auto& c=controls[focus_];
        if(!c.items.empty()&&!c.focus){const bool right=mouse.x>=c.rect.x+c.rect.width/2;c.selected=std::clamp(c.selected+(right?1:-1),0,static_cast<int>(c.items.size())-1);if(media_)media_->environment_sound("effect",right?"mouse_right":"mouse_left","menu",1,{},false,false);}
        else if(media_)media_->environment_sound("effect",c.focus?"mouse_listclick":"mouse_click","menu",1,{},false,false);
        activate(c);return true;
    }
    if(active_page_=="emegi_gecenler"){if(event.type==SDL_KEYUP&&event.key.keysym.scancode==SDL_SCANCODE_SPACE){credits_paused_=!credits_paused_;return true;}if(event.type==SDL_KEYDOWN&&(event.key.keysym.scancode==SDL_SCANCODE_DOWN||event.key.keysym.scancode==SDL_SCANCODE_UP)){credits_interval_=event.key.keysym.scancode==SDL_SCANCODE_DOWN?33:-33;credits_paused_=false;return true;}}
    return event.type==SDL_MOUSEMOTION||event.type==SDL_MOUSEBUTTONUP||event.type==SDL_MOUSEBUTTONDOWN||event.type==SDL_KEYDOWN||event.type==SDL_KEYUP||event.type==SDL_TEXTINPUT;
}
void Interface::update(float seconds,const HudState& hud){
    if(!std::isfinite(seconds)||seconds<0)throw std::runtime_error("Invalid interface frame interval");elapsed_+=seconds;
    if(binding_){if(binding_wait_)binding_wait_=false;else poll_binding();}
    if(menu_visible_&&!loading_visible_&&active_page_=="emegi_gecenler"&&!credits_paused_)scroll_=std::clamp(scroll_-seconds*1000/credits_interval_,-5575.419921875f,0.0f);
    if(menu_visible_&&!loading_visible_&&!binding_)update_hover();
    if(fade_duration_>0){fade_elapsed_+=seconds;const float fraction=fade_elapsed_/fade_duration_;fade_alpha_=fade_in_?std::min(fraction,1.0f):1-fraction;fading_=fade_alpha_>=0;if(!fading_)fade_alpha_=0;}
    if(subtitle_visible_&&subtitle_row_<4&&elapsed_-subtitle_clock_>0.05f){if(subtitle_row_finished_){++subtitle_row_;subtitle_character_=0;subtitle_row_finished_=false;}subtitle_clock_=elapsed_;if(subtitle_row_<4){++subtitle_character_;const auto size=subtitle_lines_[subtitle_row_].size();if(subtitle_character_==size||size==0){subtitle_clock_+=.5f;subtitle_row_finished_=true;}}}
    const float h=std::clamp(hud.health,0.0f,1.0f),now_ms=elapsed_*1000;
    if(h>.4f){health_low_=false;health_yellow_=true;}else{const float interval=1000/(3+7*(1-2.5f*h));if(!health_low_){health_low_=true;health_deadline_ms_=std::trunc(now_ms+interval);}if(health_deadline_ms_<now_ms){health_yellow_=!health_yellow_;const float step=std::trunc(interval);health_deadline_ms_+=std::ceil((now_ms-health_deadline_ms_)/step)*step;}}
    rebuild(hud);
}
void Interface::rebuild(const HudState& hud) {
    quads_.clear();
    console_pending_count_=0;
    if(loading_visible_){const float left=(1024-1024*.8f)*.5f,top=768-768*.06f;quads_.push_back({{0,0,1024,768},{0,0,1,1},{1,1,1,1},loading_shader_,0,{},false,true});quads_.push_back({{left,top,1024*.8f,768*.01f},{0,0,1,1},{1,1,1,1},"progress_bar_transparent",0,{},false,true});quads_.push_back({{left,top,1024*.8f*loading_progress_,768*.01f},{0,0,loading_progress_,1},{1,1,1,1},"progress_bar",0,{},false,true});finalize_resources();return;}
    rebuild_console(hud.game_tick);
    if(!menu_visible_&&hud_visible_){
        const auto panel=[&](std::string_view name)->const Panel*{for(const auto& p:panels_)if(p.name==name)return &p;return nullptr;};
        const auto draw=[&](const Panel* p){if(p&&p->visible&&!p->shader.empty())quads_.push_back({p->rect,{0,0,1,1},p->color,p->shader});};
        const auto* fill=panel(health_yellow_?"health_panel_health":"health_panel_health_red");if(fill&&fill->visible){auto rect=fill->rect;const float fraction=std::clamp(hud.health,0.0f,1.0f);rect.y+=rect.height*(1-fraction);rect.height*=fraction;quads_.push_back({rect,{0,1-fraction,1,fraction},fill->color,fill->shader});}
        draw(panel(health_yellow_?"health_panel_base_yellow":"health_panel_base_red"));
        if(hud.damage_direction>=0&&hud.damage_direction<10&&hud.damage_elapsed<=.5588235259056091f){
            constexpr std::string_view masks[]={"take_hit_top","take_hit_bottom","take_hit_right","take_hit_left","take_hit_topleft","take_hit_topright","take_hit_bottomleft","take_hit_bottomright","take_hit_all","take_hit_all"};
            const float alpha=std::trunc(255*std::clamp(1-2*std::max(0.0f,hud.damage_elapsed-.05882352963089943f),0.0f,1.0f))/255;
            quads_.push_back({{0,0,1024,768},{.01f,.01f,.98f,.98f},{1,1,1,alpha},masks[hud.damage_direction],0,{},false,true});
        }
        draw(panel("weapon_panel_base"));const Panel* icon=nullptr;for(const auto& p:panels_)if(p.name.starts_with("gun_")&&p.shader==hud.weapon_shader){icon=&p;break;}
        if(icon){draw(icon);char magazine[24],ammunition[24];const auto m=std::to_chars(magazine,magazine+sizeof magazine,hud.magazine),a=std::to_chars(ammunition,ammunition+sizeof ammunition,hud.ammunition);const Vec2 scale{1.28f,1.28f};
            const auto label=[&](std::string_view name,std::string_view value,int align){const auto* p=panel(name);if(!p||!p->visible)return;const float width=text_width(p->font,value,scale),height=fonts_.at(p->font).height*scale.y;float x=p->rect.x;if(align==1)x+=p->rect.width*.5f-static_cast<int>(width)/2;else if(align==2)x+=p->rect.width-width;text(p->font,value,{std::trunc(x),std::trunc(p->rect.y+p->rect.height*.5f-height*.5f)},p->color,scale);};
            label("weapon_ammo_in_magazine",{magazine,static_cast<std::size_t>(m.ptr-magazine)},2);label("weapon_ammo_slash","/",1);label("weapon_ammo_total",{ammunition,static_cast<std::size_t>(a.ptr-ammunition)},0);label("weapon_name",hud.weapon_name,0);
        }
        if(hud.crosshair&&!hud.camera_blocked&&!hud.scope_visible){const float side=desktop_width_/160.0f;quads_.push_back({{512-side*.5f,384-side*.5f,side,side},{0,0,1,1},{1,1,1,1},"crosshair_single"});}
        if(hud.scope_visible){
            const auto scope=[&](std::string_view shader,float x0,float y0,float x1,float y1,float rotation=0,bool mask_margins=false){quads_.push_back({{x0,y0,x1-x0,y1-y0},{.01f,.01f,.98f,.98f},{1,1,1,1},shader,rotation,{512,384},mask_margins});};
            scope("scope_leftline_thick",1024*.16f,383,1024*.36f,386);scope("scope_leftline_slim",1024*.335f,384,1024*.485f,385);scope("scope_rightline_thick",1024*.64f,383,1024*.84f,386);scope("scope_rightline_slim",1024*.515f,384,1024*.665f,385);
            scope("scope_topline_thick",511,768*-.105f,514,768*.345f);scope("scope_topline_slim",512,768*.135f,513,768*.48f);scope("scope_bottomline_thick",511,768*.655f,514,768*1.105f);scope("scope_bottomline_slim",512,768*.52f,513,768*.865f);
            const float left=512-1024*.30000001192092896f,right=512+1024*.30000001192092896f,top=384-768*.40000003576278687f,bottom=384+768*.40000003576278687f;
            scope("scope_circle",left,top,right,bottom);scope("scope_circle_outer",0,0,1024,top,0,true);scope("scope_circle_outer",0,bottom,1024,768);scope("scope_circle_outer",0,top,left,bottom);scope("scope_circle_outer",right,top,1024,bottom);
            scope("scope_inner_left_highlight",1024*.01f,768*.07f,1024*.41f,768*.93f,hud.scope_angle_degrees);scope("scope_inner_right_highlight",1024*.59f,768*.07f,1024*.99f,768*.93f,hud.scope_angle_degrees);
            scope("scope_glass_left_highlight",1024*.17f,768*.2f,1024*.37f,768*.8f,-hud.scope_angle_degrees);scope("scope_glass_right_highlight",1024*.63f,768*.2f,1024*.83f,768*.8f,-hud.scope_angle_degrees);
        }
        if(!hud.notification.empty())text("notification_font",hud.notification,{5,5},{1,0,0,1});
    }
    if(menu_visible_) {
        auto& page=pages_.at(active_page_);const Control* help=nullptr;
        for(std::size_t i=0;i<page.controls.size();++i){auto& c=page.controls[i];if(!c.enabled_explicit){if(c.name=="oyuna_devam")c.enabled=game_active_;if(c.name=="oyun_yukle_yukle"){c.enabled=false;for(const auto& slot:page.controls)if(slot.name=="list_oyun_yukle_checkpoint"&&!slot.items.empty())c.enabled=true;}}
            auto rect=c.rect;if(c.scroll&&active_page_=="emegi_gecenler")rect.y=std::trunc(rect.y+scroll_+.5f);
            const auto color=!c.enabled?c.disabled:&c==binding_?std::array<float,4>{0,0,0,1}:!binding_&&!outside_&&c.z>=0&&inside(mouse_,rect)?c.over:c.normal;
            if(c.frame){auto frame=c.frame_rect;frame.x+=rect.x;frame.y=rect.y+std::trunc((rect.height-frame.height)*.5f+.5f);quads_.push_back({frame,{0,0,1,1},color,c.frame_shader});}
            if(!c.shader.empty())quads_.push_back({rect,{0,0,1,1},color,c.shader,0,{},rect.x==0&&rect.y==0&&rect.width==1024&&rect.height==768});
            if(!c.font.empty()){const float y=rect.y+std::trunc((rect.height-fonts_.at(c.font).height)*.5f+.5f);if(c.list){const std::string_view value=c.items.empty()?std::string_view(" "):std::string_view(c.items[c.selected].text);text(c.font,"<",{rect.x,y},color,Vec2{1,1},true);text(c.font,value,{rect.x+std::trunc((rect.width-text_width(c.font,value))*.5f+.5f),y},color,Vec2{1,1},true);text(c.font,">",{rect.x+rect.width-text_width(c.font,">"),y},color,Vec2{1,1},true);}else text(c.font,c.text,{rect.x,y},color,Vec2{1,1},true);}
            if(!binding_&&static_cast<int>(i)==focus_&&!c.help.empty())help=&c;
        }
        if(!outside_)quads_.push_back({{std::trunc(std::trunc(mouse_.x+.5f)-16),std::trunc(std::trunc(mouse_.y+.5f)-1.5994880199432373f),51,51},{0,0,1,1},{1,1,1,1},"mouse_icon"});
        if(help)text("menu_secenekler",help->help,{10,700},{1,1,1,1},Vec2{1,1},true);
    }
    if(fullscreen_visible_&&!fullscreen_shader_.empty())quads_.push_back({{0,0,1024,768},{0,0,1,1},{1,1,1,1},fullscreen_shader_,0,{},false,true});
    const bool credits=menu_visible_&&active_page_=="emegi_gecenler";const float camera_age=static_cast<float>(hud.game_tick-hud.camera_transition_tick)*.001f;const float bar_fraction=credits?1:std::clamp(hud.camera_blocked?camera_age:1-camera_age,0.0f,1.0f);const float bar_height=std::trunc((768-1024*.5404999852180481f)*.5f*bar_fraction);
    if(bar_fraction>0){quads_.push_back({{0,0,1024,bar_height},{0,0,1,1},{0,0,0,1},{}});if(!credits)quads_.back().native_bar_fraction=bar_fraction;quads_.push_back({{0,768-bar_height,1024,bar_height},{0,0,1,1},{0,0,0,1},{}});if(!credits)quads_.back().native_bar_fraction=bar_fraction;}
    // Original 00426ba0 stores its shader, but 0042a220 renders subtitle text only.
    if(subtitle_visible_)for(unsigned row=0;row<4&&row<=subtitle_row_;++row){const std::string_view value=subtitle_lines_[row];text("menu_secenekler",row<subtitle_row_?value:value.substr(0,std::min<std::size_t>(value.size(),subtitle_character_+1)),{10,672+18.0f*row},{1,1,1,1},Vec2{1,1},true);}
    if(fading_&&!menu_visible_){quads_.push_back({{0,0,1024,768},{0,0,1,1},{0,0,0,fade_alpha_},{},0,{},false,true});quads_.back().history_capture_visible=true;}
    finalize_resources();
}
void Interface::finalize_resources() {
    auto previous_epoch=resource_epoch_++;
    // Keep epoch zero reserved for identities never emitted; preserve membership on wrap.
    if(resource_epoch_==0){
        for(auto& names:resource_names_)for(auto& [name,seen]:names)seen=seen==previous_epoch?1:0;
        previous_epoch=1;resource_epoch_=2;
    }
    std::size_t count{};bool changed=false,last_font=false;std::string_view last_shader;
    for(const auto& quad:quads_){
        if(quad.shader.empty()){last_shader={};continue;}
        // Glyph runs share an atlas; no hash or owned key allocation per glyph/frame.
        if(quad.shader==last_shader&&quad.font_atlas==last_font)continue;
        last_shader=quad.shader;last_font=quad.font_atlas;
        auto& names=resource_names_[quad.font_atlas?1:0];
        auto found=names.find(quad.shader);
        if(found==names.end())found=names.emplace(std::string(quad.shader),0).first;
        auto& seen=found->second;
        if(seen==resource_epoch_)continue;
        // 004593a0/00459410 construct at the nonfont drawing consumer, not parsing.
        if(!quad.font_atlas)materials_.construct(quad.shader,0,0,false);
        if(seen!=previous_epoch||seen==0)changed=true;
        seen=resource_epoch_;++count;
    }
    // A new active identity detects replacements; cardinality also detects pure removals.
    if(changed||count!=resource_count_)++resource_revision_;
    resource_count_=count;
}
} // namespace pusu
