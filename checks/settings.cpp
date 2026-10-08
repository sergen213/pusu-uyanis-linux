#include "settings.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    char pattern[] = "/tmp/pusu-settings-XXXXXX";
    const char* directory = ::mkdtemp(pattern);
    assert(directory);
    const std::filesystem::path root(directory), file = root / "config/settings.ini";
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    auto rejects = [](auto operation) {
        bool rejected = false;
        try { operation(); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    };
    const auto missing = pusu::read_settings(file);
    assert(missing.renderer == "opengl");
    auto expected = missing;
    expected.renderer = "vulkan";
    expected.width = 1920; expected.height = 1080;
    expected.fullscreen = true; expected.vsync = false;
    expected.reference_fov = 105.75f; expected.mouse_sensitivity = -27.5f;
    expected.msaa = 8; expected.anisotropy = 16; expected.gamma = 1.25f;
    expected.bloom = false; expected.ray_tracing = false;
    expected.master_volume = .823456f; expected.sfx_volume = .15f; expected.music_volume = .25f;
    expected.ray_shadows = false; expected.ray_reflections = true;
    expected.hdr_tonemapping = false; expected.exposure_ev = -3.125f;
    expected.virtual_light_enabled = true;
    expected.virtual_light_azimuth = 273.25f; expected.virtual_light_elevation = -17.5f;
    expected.virtual_light_red = .123456f; expected.virtual_light_green = .5f;
    expected.virtual_light_blue = .75f; expected.virtual_light_strength = 7.625f;
    pusu::write_settings(file, expected);
    const auto actual = pusu::read_settings(file);
    assert(actual.renderer == expected.renderer && actual.width == expected.width && actual.height == expected.height);
    assert(actual.fullscreen == expected.fullscreen && actual.vsync == expected.vsync);
    assert(actual.reference_fov == expected.reference_fov && actual.mouse_sensitivity == expected.mouse_sensitivity);
    assert(actual.msaa == expected.msaa && actual.anisotropy == expected.anisotropy && actual.gamma == expected.gamma);
    assert(actual.bloom == expected.bloom && actual.ray_tracing == expected.ray_tracing);
    assert(actual.master_volume == expected.master_volume && actual.sfx_volume == expected.sfx_volume && actual.music_volume == expected.music_volume);
    auto same_enhancements = [](const pusu::Settings& a, const pusu::Settings& b) {
        assert(a.ray_shadows == b.ray_shadows && a.ray_reflections == b.ray_reflections);
        assert(a.hdr_tonemapping == b.hdr_tonemapping && a.exposure_ev == b.exposure_ev);
        assert(a.virtual_light_enabled == b.virtual_light_enabled);
        assert(a.virtual_light_azimuth == b.virtual_light_azimuth && a.virtual_light_elevation == b.virtual_light_elevation);
        assert(a.virtual_light_red == b.virtual_light_red && a.virtual_light_green == b.virtual_light_green);
        assert(a.virtual_light_blue == b.virtual_light_blue && a.virtual_light_strength == b.virtual_light_strength);
    };
    same_enhancements(actual, expected);
    auto ray_preference = expected;
    ray_preference.renderer = "opengl";
    ray_preference.ray_tracing = true;
    pusu::write_settings(file, ray_preference);
    const auto opengl = pusu::read_settings(file);
    assert(opengl.renderer == "opengl" && opengl.ray_tracing);
    same_enhancements(opengl, ray_preference);
    ray_preference.renderer = "vulkan";
    pusu::write_settings(file, ray_preference);
    const auto vulkan = pusu::read_settings(file);
    assert(vulkan.renderer == "vulkan" && vulkan.ray_tracing);
    same_enhancements(vulkan, ray_preference);
    auto inverse = ray_preference;
    inverse.ray_shadows = !inverse.ray_shadows;
    inverse.ray_reflections = !inverse.ray_reflections;
    inverse.hdr_tonemapping = !inverse.hdr_tonemapping;
    inverse.virtual_light_enabled = !inverse.virtual_light_enabled;
    pusu::write_settings(file, inverse);
    same_enhancements(pusu::read_settings(file), inverse);
    pusu::write_settings(file, expected);
    struct stat status{};
    assert(::stat(file.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600);
    for (const auto& bad : {"width=319\n", "height=not-a-number\n", "fullscreen=true\n", "gamma=nan\n",
                            "msaa=3\n", "anisotropy=0\n", "master_volume=1.1\n", "renderer=pretend\n",
                            "mouse_sensitivity=-100.01\n", "mouse_sensitivity=100.01\n",
                            "gamma=1\ngamma=2\n", "unknown=0\n", "width\n", "music_volume=0.1 trailing\n"}) {
        { std::ofstream stream(file); stream << bad; }
        rejects([&] { (void)pusu::read_settings(file); });
    }
    // Existing GL configurations gain inactive Vulkan preferences, not a sun.
    { std::ofstream stream(file); stream << "renderer=opengl\nray_tracing=0\n"; }
    const auto legacy = pusu::read_settings(file);
    assert(legacy.renderer == "opengl" && !legacy.ray_tracing);
    assert(legacy.ray_shadows && legacy.ray_reflections && legacy.hdr_tonemapping && legacy.exposure_ev == 0);
    assert(!legacy.virtual_light_enabled && legacy.virtual_light_azimuth == 0 && legacy.virtual_light_elevation == 60);
    assert(legacy.virtual_light_red == 1 && legacy.virtual_light_green == 1 && legacy.virtual_light_blue == 1 && legacy.virtual_light_strength == .25f);
    for (const auto& key : {"ray_shadows", "ray_reflections", "hdr_tonemapping", "virtual_light_enabled"}) {
        for (const auto& value : {"true", "-1", "2"}) {
            { std::ofstream stream(file); stream << key << '=' << value << '\n'; }
            rejects([&] { (void)pusu::read_settings(file); });
        }
        { std::ofstream stream(file); stream << key << "=0\n" << key << "=1\n"; }
        rejects([&] { (void)pusu::read_settings(file); });
    }
    struct Range { const char* key; float pusu::Settings::*field; float low, high; };
    for (const auto& range : {
        Range{"exposure_ev", &pusu::Settings::exposure_ev, -8, 8},
        Range{"virtual_light_azimuth", &pusu::Settings::virtual_light_azimuth, 0, 360},
        Range{"virtual_light_elevation", &pusu::Settings::virtual_light_elevation, -90, 90},
        Range{"virtual_light_red", &pusu::Settings::virtual_light_red, 0, 1},
        Range{"virtual_light_green", &pusu::Settings::virtual_light_green, 0, 1},
        Range{"virtual_light_blue", &pusu::Settings::virtual_light_blue, 0, 1},
        Range{"virtual_light_strength", &pusu::Settings::virtual_light_strength, 0, 16}}) {
        for (float endpoint : {range.low, range.high}) {
            auto boundary = expected;
            boundary.*range.field = endpoint;
            pusu::write_settings(file, boundary);
            assert(pusu::read_settings(file).*range.field == endpoint);
        }
        for (const auto& value : {"nan", "inf", "-inf", "0 trailing"}) {
            { std::ofstream stream(file); stream << range.key << '=' << value << '\n'; }
            rejects([&] { (void)pusu::read_settings(file); });
        }
        for (float invalid : {range.low - .01f, range.high + .01f,
                              std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            pusu::write_settings(file, expected);
            auto bad = expected;
            bad.*range.field = invalid;
            rejects([&] { pusu::write_settings(file, bad); });
            // A rejected save must leave every published enhancement preference intact.
            same_enhancements(pusu::read_settings(file), expected);
            { std::ofstream stream(file); stream << range.key << '=' << invalid << '\n'; }
            rejects([&] { (void)pusu::read_settings(file); });
        }
    }
    { std::ofstream stream(file); stream << "mouse_sensitivity=-100\n"; }
    assert(pusu::read_settings(file).mouse_sensitivity == -100.0f);
    { std::ofstream stream(file); stream << "mouse_sensitivity=100\n"; }
    assert(pusu::read_settings(file).mouse_sensitivity == 100.0f);
    { std::ofstream stream(file); stream << "# partial settings use defaults\r\nwidth = 1600\r\nvsync = 0\n"; }
    assert(pusu::read_settings(file).width == 1600 && !pusu::read_settings(file).vsync);
    { std::ofstream stream(file, std::ios::binary); stream.write("width=1600\0", 11); }
    rejects([&] { (void)pusu::read_settings(file); });
    const auto link = root / "linked.ini";
    std::filesystem::create_symlink(file, link);
    rejects([&] { (void)pusu::read_settings(link); });
    rejects([&] { pusu::write_settings(link, expected); });
    expected.gamma = std::numeric_limits<float>::infinity();
    rejects([&] { pusu::write_settings(file, expected); });
    assert(::setenv("XDG_DATA_HOME", root.c_str(), 1) == 0);
    assert(::setenv("XDG_CONFIG_HOME", root.c_str(), 1) == 0);
    assert(pusu::user_data_directory() == root / "pusu");
    assert(pusu::user_settings_file() == root / "pusu/settings.ini");
    assert(::setenv("XDG_CONFIG_HOME", "relative", 1) == 0);
    rejects([&] { (void)pusu::user_settings_file(); });
}
