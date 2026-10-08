#pragma once

#include <filesystem>
#include <string>

namespace pusu {

struct Settings {
    std::string renderer{"opengl"};
    int width{1280}, height{720};
    bool fullscreen{}, vsync{true};
    // Horizontal FOV at 4:3; preserve its vertical FOV as the viewport widens.
    float reference_fov{90.0f};
    // Original -100..100 slider; the controller applies its piecewise mapping.
    float mouse_sensitivity{0.0f};
    int msaa{4}, anisotropy{8};
    float gamma{1.0f};
    bool bloom{true}, ray_tracing{true};
    // Vulkan-only preferences survive OpenGL launches without affecting GL.
    bool ray_shadows{true}, ray_reflections{true}, hdr_tonemapping{true};
    float exposure_ev{0.0f};
    // Explicit enhancement; original baked lighting is never a runtime sun.
    bool virtual_light_enabled{};
    float virtual_light_azimuth{0.0f}, virtual_light_elevation{60.0f};
    float virtual_light_red{1.0f}, virtual_light_green{1.0f}, virtual_light_blue{1.0f};
    float virtual_light_strength{0.25f};
    float master_volume{1.0f}, sfx_volume{0.75f}, music_volume{0.75f};
};

// Strict key=value native configuration. Missing file uses these defaults;
// malformed or out-of-range values produce a useful error, not silent repair.
Settings read_settings(const std::filesystem::path& path);
void write_settings(const std::filesystem::path& path, const Settings& settings);
std::filesystem::path user_data_directory();
std::filesystem::path user_settings_file();

} // namespace pusu
