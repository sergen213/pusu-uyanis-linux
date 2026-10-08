#pragma once
#include "assets.hpp"
#include "settings.hpp"
#include "renderer.hpp"
#include <SDL.h>
#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pusu {
class AssetStore;
class Media;
class MaterialLibrary;
using InterfaceRect = RenderRect;
struct HudState {
    float health{1};
    int ammunition{}, magazine{};
    std::string weapon_shader, weapon_name, notification;
    int damage_direction{10};float damage_elapsed{},scope_angle_degrees{};
    bool crosshair{},scope_visible{},camera_blocked{};
    std::uint32_t game_tick{},camera_transition_tick{};
};
struct MenuAction { std::string name, value; int index{-1}; };
struct InterfaceSnapshot {
    std::string fullscreen_shader,subtitle_shader;std::array<std::string,4> subtitle_lines;
    float fade_duration{},fade_elapsed{},fade_alpha{},subtitle_delay{};
    unsigned subtitle_row{},subtitle_character{};
    bool fullscreen_visible{},subtitle_visible{},fade_in{},fading{},subtitle_row_finished{},hud_visible{true};
};
class Interface {
public:
    Interface(const AssetStore&, Settings&, const MaterialLibrary&);
    void set_action_handler(std::function<void(const MenuAction&)> handler);
    void set_media(Media& value);
    // Plain CP1254 WARNING line; the native console applies severity-two styling.
    void console_warning(std::string_view line,std::uint32_t tick);
    void set_drawable_size(unsigned width,unsigned height);
    void console_presented(std::uint32_t tick);
    // Mouse coordinates already mapped by the window owner into authored 1024x768.
    bool input(const SDL_Event&, Vec2 mouse, bool outside_bars);
    void update(float seconds, const HudState&);
    std::span<const InterfaceQuad> draw_data() const noexcept { return quads_; }
    // Identity set of the last completed build, including removals; read after update().
    std::uint64_t resource_revision() const noexcept { return resource_revision_; }
    void show_menu(std::string_view page = "ana_sayfa");
    void hide_menu();
    bool captures_input() const noexcept { return menu_visible_ || binding_ != nullptr || loading_visible_; }
    bool wants_relative_mouse() const noexcept { return !captures_input(); }
    void show_hud(bool value) noexcept { hud_visible_=value; }
    SDL_Scancode binding(std::string_view action) const;
    bool action_down(std::string_view action, std::span<const Uint8> keyboard, Uint32 mouse_buttons) const;
    bool invert_mouse() const noexcept { return invert_mouse_; }
    void set_game_active(bool value) noexcept { game_active_ = value; }
    void set_settings_file(std::filesystem::path value) { settings_file_ = std::move(value); }
    void set_text(std::string_view name, std::string_view value);
    void show_panel(std::string_view name, bool visible);
    void set_save_slots(std::span<const std::string> labels);
    void set_control_shader(std::string_view name,std::string_view shader);
    void set_list_items(std::string_view page,std::string_view name,std::span<const std::string> labels);
    int selected_item(std::string_view page,std::string_view name) const;
    void set_selected_item(std::string_view page,std::string_view name,int index);
    void set_control_enabled(std::string_view name,bool enabled);
    void set_fullscreen_quad(std::string_view shader,bool visible);
    void set_loading(std::string_view shader,float progress,bool visible);
    void set_subtitle_quad(std::string_view shader,bool visible);
    void set_subtitle_text(int line,std::string_view translated_text);
    void set_fade(bool fade_in,float duration);
    void set_fade_time(float duration);
    OriginalGraphicsOptions graphics_options() const noexcept { return graphics_options_; }
    bool blood_enabled() const noexcept { return blood_enabled_; }
    InterfaceSnapshot snapshot() const;
    void validate_state(const InterfaceSnapshot&) const;
    void restore_state(const InterfaceSnapshot&);
private:
    // Original scope initialization constructs these resources before visibility.
    void initialize_materials() const;
    struct Glyph { float u{},v{},width{},height{},source_width{},source_height{}; };
    struct Font { std::string shader; float atlas_width{},atlas_height{},height{},spacing{}; std::array<Glyph,256> glyphs{}; };
    struct Item { std::string text; int volume{100}; };
    struct Control {
        std::string name,shader,font,text,target,help,frame_shader;
        InterfaceRect rect,frame_rect; float z{};
        std::array<float,4> normal{1,1,1,1},over{1,1,1,1},disabled{1,1,1,1};
        std::vector<Item> items; int selected{};
        bool list{},focus{},frame{},scroll{true},enabled{true},enabled_explicit{};
    };
    struct Page { std::string name,back; std::vector<Control> controls; };
    struct Edge {float amount{};bool spring{};int anchor{};};
    struct Panel {
        std::string name,shader,font,text;InterfaceRect rect;std::array<float,4> color{1,1,1,1};bool visible{true};
        std::array<Edge,4> edges{{{10,false,0},{100,false,0},{100,false,3},{10,false,3}}};
    };
    struct ConsoleLine { std::string text; std::optional<std::uint32_t> first_render_tick; };
    const AssetStore& assets_; Settings& settings_; const MaterialLibrary& materials_;
    std::unordered_map<std::string,Font> fonts_;
    std::unordered_map<std::string,Page> pages_;
    struct Binding { SDL_Scancode key{SDL_SCANCODE_UNKNOWN}; Uint32 mouse{}; };
    std::unordered_map<std::string,Binding> bindings_;
    std::vector<Panel> panels_; std::vector<InterfaceQuad> quads_;
    std::deque<ConsoleLine> console_lines_;
    std::size_t console_pending_count_{};
    unsigned drawable_width_{1024},drawable_height_{768};
    std::function<void(const MenuAction&)> handler_;
    Media* media_{};
    std::string active_page_; Control* binding_{};
    std::array<Uint8,SDL_NUM_SCANCODES> key_state_{};
    Uint32 buttons_{};bool binding_wait_{};
    std::string fullscreen_shader_,subtitle_shader_;
    std::string loading_shader_;float loading_progress_{};bool loading_visible_{};
    std::array<std::string,4> subtitle_lines_;
    std::uint64_t resource_revision_{1};
    struct ResourceNameHash {
        using is_transparent=void;
        std::size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
    };
    using ResourceNames=std::unordered_map<std::string,std::uint64_t,ResourceNameHash,std::equal_to<>>;
    // Raw spellings stay distinct: font atlases and material names have separate consumers.
    std::array<ResourceNames,2> resource_names_;
    std::uint64_t resource_epoch_{};
    std::size_t resource_count_{};
    float fade_duration_{},fade_elapsed_{},fade_alpha_{};
    bool fullscreen_visible_{},subtitle_visible_{},fade_in_{},fading_{};
    unsigned subtitle_row_{},subtitle_character_{};
    float subtitle_clock_{};
    int credits_interval_{33};
    bool subtitle_row_finished_{},credits_paused_{};
    bool health_low_{},health_yellow_{true};
    float health_deadline_ms_{};int desktop_width_{};
    std::filesystem::path settings_file_{user_settings_file()};
    Vec2 mouse_{}; int focus_{-1}; float scroll_{},elapsed_{};
    bool menu_visible_{true},hud_visible_{true},outside_{},invert_mouse_{},game_active_{};
    void load_fonts(); void load_page(std::string_view); void load_hud();
    void activate(Control&); void rebuild(const HudState&);
    void finalize_resources();
    void rebuild_console(std::uint32_t tick);
    // 00441830 uses height-1; HUD 004547f0 keeps the full glyph height.
    void text(std::string_view font,std::string_view text,Vec2,std::array<float,4>,Vec2 scale=Vec2{1.0f,1.0f},bool legacy_bottom_inclusive=false);
    float text_width(std::string_view font,std::string_view value,Vec2 scale=Vec2{1.0f,1.0f}) const;
    void update_binding(Control&);
    void poll_binding();
    void update_hover();
    void persist_controls();
    void update_graphics_options();
    OriginalGraphicsOptions graphics_options_;
    bool blood_enabled_{true};
};
} // namespace pusu
