#include "game.hpp"
#include "interface.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>

namespace pusu {
namespace {
bool host_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
Vec3 pop_vector(script::Runtime& vm) {
    const float x = vm.pop_float();
    const float y = vm.pop_float();
    const float z = vm.pop_float();
    return {x, y, z};
}
Vec3 read_vector(script::Runtime& vm, std::uint32_t address) {
    if (address > UINT32_MAX - 8) throw script::Error("Vector VM address overflow");
    return {std::bit_cast<float>(vm.read_word(address)),
            std::bit_cast<float>(vm.read_word(address + 4)),
            std::bit_cast<float>(vm.read_word(address + 8))};
}
}

void Game::register_hosts() { register_hosts(runtime_); }
void Game::register_hosts(script::Runtime& target) {
    // Scratch save-load registrations deliberately capture the same live Game.
    target.set_missing_call_diagnostic([this](std::string_view name) { warn_missing_call(name); });
    using VM = script::Runtime;
    auto add = [&target](const char* name, VM::Host callback) { target.register_host(name, std::move(callback)); };
    add("load_level", [this](VM& vm) { queue_level(vm.pop_string(), true); });
    add("load_level_without_image", [this](VM& vm) { queue_level(vm.pop_string(), false); });
    add("checkpoint_load", [this](VM& vm) { queue_level(vm.pop_string(), true, true); });
    add("checkpoint_save", [this](VM& vm) { save_checkpoint(vm.pop_string()); });
    add("playerstat_load", [this](VM&) { load_player_stats(); });
    add("playerstat_save", [this](VM&) { save_player_stats(); });
    add("load_bsp", [this](VM& vm) { load_bsp(vm.pop_string()); });
    add("entity_create_simple", [this](VM& vm) { vm.push_word(host_create_entity(vm.pop_string(), 0)); });
    add("entity_create_bot", [this](VM& vm) {
        auto name = vm.pop_string(); auto definition = vm.pop_string();
        vm.push_word(host_create_entity(std::move(name), 1, std::move(definition)));
    });
    constexpr std::array weapon_names{"entity_bot_take_cz75", "entity_bot_take_magnum", "entity_bot_take_uzi", "entity_bot_take_ak101", "entity_bot_take_m4", "entity_bot_take_baba"};
    for (std::size_t i = 0; i < weapon_names.size(); ++i) add(weapon_names[i], [this, i](VM& vm) {
        const auto actor = vm.pop_word(); const auto unused = vm.pop_word();
        host_bot_weapon(actor, unused, static_cast<int>(i + 1));
    });
    add("entity_create_player", [this](VM& vm) {
        auto name = vm.pop_string(); auto definition = vm.pop_string();
        vm.push_word(host_create_entity(std::move(name), 2, std::move(definition)));
    });
    add("entity_set_mesh", [this](VM& vm) { const auto handle = vm.pop_word(); auto mesh = vm.pop_string(); host_set_mesh(handle, mesh); });
    add("entity_get_pos", [this](VM& vm) { vm.push_word(host_position_address(vm.pop_word())); });
    add("entity_set_pos", [this](VM& vm) {
        const auto handle = vm.pop_word(); const auto address = vm.pop_word();
        host_set_position(handle, read_vector(vm, address));
    });
    add("entity_get_pos_x", [this](VM& vm) { vm.push_float(host_position(vm.pop_word()).x); });
    add("entity_get_pos_y", [this](VM& vm) { vm.push_float(host_position(vm.pop_word()).y); });
    add("entity_get_pos_z", [this](VM& vm) { vm.push_float(host_position(vm.pop_word()).z); });
    add("entity_set_pos_manual", [this](VM& vm) { const auto handle = vm.pop_word(); const auto position = pop_vector(vm); host_set_position(handle, position); });
    constexpr std::array align_names{"entity_alignx", "entity_aligny", "entity_alignz"};
    for (int axis = 0; axis != 3; ++axis) add(align_names[axis], [this, axis](VM& vm) {
        const auto handle = vm.pop_word(); const auto side = vm.pop_word(); host_align(handle, axis, side);
    });
    constexpr std::array rotate_names{"entity_rotate_yaw", "entity_rotate_roll", "entity_rotate_pitch", "entity_rotate_yaw_world", "entity_rotate_roll_world", "entity_rotate_pitch_world"};
    for (int i = 0; i != 6; ++i) add(rotate_names[i], [this, i](VM& vm) {
        const auto handle = vm.pop_word(); const auto degrees = vm.pop_float(); host_rotate(handle, i % 3, degrees, i >= 3);
    });
    add("entity_set_shader", [this](VM& vm) { auto name = vm.pop_string(); auto part = vm.pop_string(); auto shader = vm.pop_string(); host_shader(name, part, shader, false); });
    add("entity_set_shader_all", [this](VM& vm) { auto name = vm.pop_string(); auto part = vm.pop_string(); auto shader = vm.pop_string(); host_shader(name, part, shader, true); });
    add("entity_delete", [this](VM& vm) { host_delete_entity(vm.pop_word()); });
    add("entity_hide", [this](VM& vm) { host_entity_visible(vm.pop_string(), false); });
    add("entity_show", [this](VM& vm) { host_entity_visible(vm.pop_string(), true); });
    add("entity_show_linked_item", [this](VM& vm) { auto name = vm.pop_string(); auto linked = vm.pop_string(); host_linked_visible(name, linked, true); });
    add("entity_hide_linked_item", [this](VM& vm) { auto name = vm.pop_string(); auto linked = vm.pop_string(); host_linked_visible(name, linked, false); });
    add("create_entities_from_file", [this](VM& vm) { create_entities(vm.pop_string()); });
    add("create_sound_sources_from_file", [this](VM& vm) { create_sound_sources(vm.pop_string()); });
    add("play_keyframe_anim", [this](VM& vm) {
        auto name = vm.pop_string(); auto mode_name = vm.pop_string();
        int mode = 0, count = 1;
        if (host_equal(mode_name, "once_backward")) mode = 1;
        else if (host_equal(mode_name, "loop")) { mode = 2; count = vm.pop_int(); }
        else if (host_equal(mode_name, "pingpong")) { mode = 3; count = vm.pop_int(); }
        auto linked = vm.pop_string(); host_keyframe(name, mode, count, linked);
    });
    add("pause_keyframe_anim", [this](VM& vm) { host_pause_keyframe(vm.pop_string(), 0); });
    add("pause_keyframe_anim_timed", [this](VM& vm) { auto name = vm.pop_string(); const auto seconds = vm.pop_float(); host_pause_keyframe(name, seconds); });
    add("pause_keyframe_anim_timed_named", [this](VM& vm) { auto name = vm.pop_string(); const auto seconds = vm.pop_float(); auto linked = vm.pop_string(); host_pause_keyframe(name, seconds, linked); });
    add("resume_keyframe_anim", [this](VM& vm) { host_resume_keyframe(vm.pop_string()); });
    add("resume_keyframe_anim_named", [this](VM& vm) { auto name = vm.pop_string(); auto linked = vm.pop_string(); host_resume_keyframe(name, linked); });
    add("play_character_anim", [this](VM& vm) { auto name = vm.pop_string(); auto animation = vm.pop_string(); auto mode = vm.pop_string(); host_character(name, animation, host_equal(mode, "loop")); });
    add("play_sound", [this](VM& vm) {
        auto file = vm.pop_string(); const bool loop = vm.pop_word() != 0;
        auto storage = vm.pop_string(); auto category = vm.pop_string();
        host_sound(file, loop, host_equal(storage, "stream"), host_equal(category, "music") ? 0 : 3);
    });
    add("play_sound_by_name", [this](VM& vm) {
        auto name = vm.pop_string(); auto category = vm.pop_string();
        const int type = host_equal(category, "music") ? 0 : host_equal(category, "2d") ? 3 : host_equal(category, "3d_env") ? 1 : host_equal(category, "3d") ? 2 : -1;
        host_named_sound(name, type);
    });
    add("stop_sound_by_name", [this](VM& vm) { host_stop_sound(vm.pop_string()); });
    add("set_sound_volume_by_name", [this](VM& vm) { auto name = vm.pop_string(); const auto volume = vm.pop_word(); host_sound_volume(name, volume); });
    add("add_timed_event", [this](VM& vm) { auto name = vm.pop_string(); const auto delay = vm.pop_float(); host_timed_event(std::move(name), delay); });
    add("explosion", [this](VM&) { host_explosion(); });
    add("player_no_weapon_mode", [this](VM& vm) { host_no_weapon(vm.pop_string().find("true") != std::string::npos); });
    add("shake_camera", [this](VM& vm) { const auto amount = pop_vector(vm); host_shake(amount.x, amount.y, amount.z); });
    add("rand_int", [this](VM& vm) {
        const auto low = vm.pop_word(); const auto high = vm.pop_word(); const auto random = host_random();
        const auto span = high - low;
        if (!span) throw script::Error("rand_int original divisor is zero");
        vm.push_word(random % span + low);
    });
    add("rand_float", [this](VM& vm) {
        const float low = vm.pop_float(); const float high = vm.pop_float();
        // 00427a10 keeps intermediates on the x87 stack, rounding only the pushed result.
        constexpr float reciprocal = std::bit_cast<float>(std::uint32_t{0x38000100}); // original DWORD at 00476450
        const long double random_fraction = static_cast<long double>(host_random()) * reciprocal;
        vm.push_float(static_cast<float>((static_cast<long double>(high) - low) * random_fraction + low));
    });
    add("is_svar", [this](VM& vm) { vm.push_int(host_svar_exists(vm.pop_string()) ? 1 : 0); });
    for (const char* name : {"get_svar_int", "get_svar_float", "get_svar_string"}) add(name, [this](VM& vm) { vm.push_word(host_svar_value(vm.pop_string())); });
    add("set_svar_int", [this](VM& vm) { auto name = vm.pop_string(); const auto value = vm.pop_word(); host_svar_word(std::move(name), value, 0); });
    add("set_svar_float", [this](VM& vm) { auto name = vm.pop_string(); const auto value = vm.pop_float(); host_svar_word(std::move(name), std::bit_cast<std::uint32_t>(value), 1); });
    add("set_svar_str", [this](VM& vm) { auto name = vm.pop_string(); auto value = vm.pop_string(); host_svar_string(std::move(name), std::move(value)); });
    // These are intentionally inert in the shipped executable, not missing native backends:
    // 004267d0/e0 are RET; 004267f0/800 consume one WORD; 00426430/450 consume/free one string.
    add("motion_blur_enable", [](VM&) {});
    add("motion_blur_disable", [](VM&) {});
    add("motion_blur_set_wait_time", [](VM& vm) { (void)vm.pop_word(); });
    add("motion_blur_set_max_alpha", [](VM& vm) { (void)vm.pop_word(); });
    add("fog_enable", [this](VM&) { host_fog_enabled(true); });
    add("fog_disable", [this](VM&) { host_fog_enabled(false); });
    add("fog_set_type", [this](VM& vm) { auto type = vm.pop_string(); host_fog_type(host_equal(type, "exp") ? 0x800 : host_equal(type, "exp2") ? 0x801 : 0x2601); });
    add("fog_set_color", [this](VM& vm) {
        const auto r = vm.pop_float(); const auto g = vm.pop_float(); const auto b = vm.pop_float(); const auto a = vm.pop_float(); host_fog_color({r, g, b, a});
    });
    add("fog_set_start_end", [this](VM& vm) { const auto start = vm.pop_word(); const auto end = vm.pop_word(); host_fog_range(start, end); });
    add("fog_set_density", [this](VM& vm) { host_fog_density(vm.pop_float()); });
    add("set_music", [this](VM& vm) { host_music(vm.pop_string()); });
    add("is_item_in_inventory", [this](VM& vm) { vm.push_int(host_inventory_contains(vm.pop_string()) ? 1 : 0); });
    add("remove_item_from_inventory", [this](VM& vm) { host_inventory_remove(vm.pop_string()); });
    add("create_particle_system", [this](VM& vm) {
        auto name = vm.pop_string(); auto definition = vm.pop_string();
        const auto position = pop_vector(vm); const auto velocity = pop_vector(vm); const bool active = vm.pop_word() != 0;
        host_particle(name, definition, position, velocity, active);
    });
    add("start_particle_system", [this](VM& vm) { host_particle_active(vm.pop_string(), true); });
    add("stop_particle_system", [this](VM& vm) { host_particle_active(vm.pop_string(), false); });
    add("delete_func", [](VM& vm) { (void)vm.pop_string(); vm.stop_running_script(); });
    add("delete_keyframe_anim", [](VM& vm) { (void)vm.pop_string(); });
    add("delete_character_anim", [](VM& vm) { (void)vm.pop_string(); });
    add("init_player_orientation", [this](VM&) { host_init_orientation(); });
    add("end_game", [this](VM&) { queue_level("end_game", true); });
    add("get_player", [this](VM& vm) { vm.push_word(host_player()); });
    add("get_bot", [this](VM& vm) { vm.push_word(host_find_bot(vm.pop_string())); });
    add("bot_enable", [this](VM& vm) { host_bot_enabled(vm.pop_string(), true); });
    add("bot_disable", [this](VM& vm) { host_bot_enabled(vm.pop_string(), false); });
    add("quit", [this](VM&) { quit_ = true; });
    add("set_camera_fov", [this](VM& vm) { auto name = vm.pop_string(); const auto value = vm.pop_float(); host_camera_fov(name, value); });
    add("waypoint_add_index", [this](VM& vm) { auto name = vm.pop_string(); const auto index = vm.pop_int(); host_waypoint(name, 0, index); });
    add("waypoint_clear_index_list", [this](VM& vm) { host_waypoint(vm.pop_string(), 1); });
    add("waypoint_start", [this](VM& vm) { host_waypoint(vm.pop_string(), 2); });
    add("waypoint_end", [this](VM& vm) { host_waypoint(vm.pop_string(), 3); });
    add("set_environment_type", [this](VM& vm) {
        auto type = vm.pop_string();
        if (type.find("outside") != std::string::npos) host_environment(0);
        else if (type.find("small") != std::string::npos) host_environment(1);
        else if (type.find("large") != std::string::npos) host_environment(2);
    });
    add("interface_hide", [this](VM&) { hud_visible_ = false; interface_.show_hud(false); });
    add("interface_show", [this](VM&) { hud_visible_ = true; interface_.show_hud(true); });
    add("enter_cinematic", [this](VM&) { host_cinematic(true); });
    add("exit_cinematic", [this](VM&) { host_cinematic(false); });
    add("fade_in", [this](VM&) { host_fade(true); });
    add("fade_out", [this](VM&) { host_fade(false); });
    add("set_fade_time", [this](VM& vm) { host_fade_time(vm.pop_float()); });
    add("player_hurt", [this](VM& vm) { host_hurt(vm.pop_float()); });
    add("disable_ingame_processing", [this](VM&) { processing_ = false; });
    add("disable_ingame_inputprocessing", [this](VM&) { input_processing_ = false; use_only_ = false; });
    add("disable_ingame_inputprocessing_except_use", [this](VM&) { input_processing_ = false; use_only_ = true; });
    add("disable_ingame_rendering", [this](VM&) { host_rendering(false); });
    add("enable_ingame_processing", [this](VM&) { processing_ = true; });
    add("enable_ingame_inputprocessing", [this](VM&) { input_processing_ = true; });
    add("enable_ingame_rendering", [this](VM&) { host_rendering(true); });
    add("enable_fullscreen_quad", [this](VM& vm) { host_fullscreen(vm.pop_string(), true); });
    add("disable_fullscreen_quad", [this](VM&) { host_fullscreen({}, false); });
    add("enable_subtitle_quad", [this](VM& vm) { host_subtitle(vm.pop_string(), true); });
    constexpr std::array subtitle_names{"set_subtitle_text_1", "set_subtitle_text_2", "set_subtitle_text_3", "set_subtitle_text_4"};
    for (int i = 0; i != 4; ++i) add(subtitle_names[i], [this, i](VM& vm) { host_subtitle_text(i, vm.pop_string()); });
    add("disable_subtitle_quad", [this](VM&) { host_subtitle({}, false); });
}
} // namespace pusu
