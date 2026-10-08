#include "settings.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <type_traits>

namespace pusu {
namespace {
struct Descriptor {
    int value{-1};
    explicit Descriptor(int fd) : value(fd) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};
[[noreturn]] void io_error(const std::filesystem::path& path, std::string_view action) {
    const int error = errno;
    throw std::runtime_error(std::string(action) + " " + path.string() + ": " + std::strerror(error));
}
std::string_view trim(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\r");
    if (start == text.npos) return {};
    return text.substr(start, text.find_last_not_of(" \t\r") - start + 1);
}
template<class T> T number(std::string_view text, std::string_view key) {
    T result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid numeric setting '" + std::string(key) + "'");
    return result;
}
void validate(const Settings& s) {
    auto range = [](auto value, auto low, auto high, const char* key) {
        if (!std::isfinite(static_cast<double>(value)) || value < low || value > high)
            throw std::runtime_error(std::string("Setting '") + key + "' is outside " +
                                     std::to_string(low) + ".." + std::to_string(high));
    };
    if (s.renderer != "opengl" && s.renderer != "vulkan")
        throw std::runtime_error("Setting 'renderer' must be opengl or vulkan");
    range(s.width, 320, 16384, "width");
    range(s.height, 200, 16384, "height");
    range(s.reference_fov, 45.0f, 130.0f, "reference_fov");
    range(s.mouse_sensitivity, -100.0f, 100.0f, "mouse_sensitivity");
    if (s.msaa != 0 && s.msaa != 2 && s.msaa != 4 && s.msaa != 8 && s.msaa != 16)
        throw std::runtime_error("Setting 'msaa' must be 0, 2, 4, 8 or 16");
    if (s.anisotropy != 1 && s.anisotropy != 2 && s.anisotropy != 4 && s.anisotropy != 8 && s.anisotropy != 16)
        throw std::runtime_error("Setting 'anisotropy' must be 1, 2, 4, 8 or 16");
    range(s.gamma, 0.25f, 4.0f, "gamma");
    range(s.exposure_ev, -8.0f, 8.0f, "exposure_ev");
    range(s.virtual_light_azimuth, 0.0f, 360.0f, "virtual_light_azimuth");
    range(s.virtual_light_elevation, -90.0f, 90.0f, "virtual_light_elevation");
    range(s.virtual_light_red, 0.0f, 1.0f, "virtual_light_red");
    range(s.virtual_light_green, 0.0f, 1.0f, "virtual_light_green");
    range(s.virtual_light_blue, 0.0f, 1.0f, "virtual_light_blue");
    range(s.virtual_light_strength, 0.0f, 16.0f, "virtual_light_strength");
    range(s.master_volume, 0.0f, 1.0f, "master_volume");
    range(s.sfx_volume, 0.0f, 1.0f, "sfx_volume");
    range(s.music_volume, 0.0f, 1.0f, "music_volume");
}
void assign(Settings& s, std::string_view key, std::string_view value) {
    auto boolean = [&] {
        if (value != "0" && value != "1")
            throw std::runtime_error("Setting '" + std::string(key) + "' must be 0 or 1");
        return value == "1";
    };
    if (key == "renderer") s.renderer = value;
    else if (key == "width") s.width = number<int>(value, key);
    else if (key == "height") s.height = number<int>(value, key);
    else if (key == "fullscreen") s.fullscreen = boolean();
    else if (key == "vsync") s.vsync = boolean();
    else if (key == "reference_fov") s.reference_fov = number<float>(value, key);
    else if (key == "mouse_sensitivity") s.mouse_sensitivity = number<float>(value, key);
    else if (key == "msaa") s.msaa = number<int>(value, key);
    else if (key == "anisotropy") s.anisotropy = number<int>(value, key);
    else if (key == "gamma") s.gamma = number<float>(value, key);
    else if (key == "bloom") s.bloom = boolean();
    else if (key == "ray_tracing") s.ray_tracing = boolean();
    else if (key == "ray_shadows") s.ray_shadows = boolean();
    else if (key == "ray_reflections") s.ray_reflections = boolean();
    else if (key == "hdr_tonemapping") s.hdr_tonemapping = boolean();
    else if (key == "exposure_ev") s.exposure_ev = number<float>(value, key);
    else if (key == "virtual_light_enabled") s.virtual_light_enabled = boolean();
    else if (key == "virtual_light_azimuth") s.virtual_light_azimuth = number<float>(value, key);
    else if (key == "virtual_light_elevation") s.virtual_light_elevation = number<float>(value, key);
    else if (key == "virtual_light_red") s.virtual_light_red = number<float>(value, key);
    else if (key == "virtual_light_green") s.virtual_light_green = number<float>(value, key);
    else if (key == "virtual_light_blue") s.virtual_light_blue = number<float>(value, key);
    else if (key == "virtual_light_strength") s.virtual_light_strength = number<float>(value, key);
    else if (key == "master_volume") s.master_volume = number<float>(value, key);
    else if (key == "sfx_volume") s.sfx_volume = number<float>(value, key);
    else if (key == "music_volume") s.music_volume = number<float>(value, key);
    else throw std::runtime_error("Unknown setting '" + std::string(key) + "'");
}
template<class T> std::string digits(T value) {
    std::array<char, 64> buffer{};
    auto result = [&] {
        if constexpr (std::is_floating_point_v<T>)
            return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                 std::chars_format::general, std::numeric_limits<T>::max_digits10);
        else return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    }();
    if (result.ec != std::errc{}) throw std::runtime_error("Cannot serialize setting");
    return {buffer.data(), result.ptr};
}
std::filesystem::path xdg(const char* name, const char* fallback) {
    if (const char* value = std::getenv(name); value && *value) {
        std::filesystem::path root(value);
        if (!root.is_absolute()) throw std::runtime_error(std::string(name) + " must be an absolute path");
        return root / "pusu";
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home || !std::filesystem::path(home).is_absolute())
        throw std::runtime_error(std::string("HOME or ") + name + " must specify an absolute user directory");
    return std::filesystem::path(home) / fallback / "pusu";
}
} // namespace

Settings read_settings(const std::filesystem::path& path) {
    Descriptor input(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (input.value < 0) {
        if (errno == ENOENT) return {};
        io_error(path, "Cannot read settings");
    }
    struct stat status{};
    if (::fstat(input.value, &status) < 0) io_error(path, "Cannot inspect settings");
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || status.st_size > 65536)
        throw std::runtime_error("Settings must be a regular file no larger than 64 KiB: " + path.string());
    std::string text;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto size = ::read(input.value, buffer.data(), buffer.size());
        if (size < 0) { if (errno == EINTR) continue; io_error(path, "Cannot read settings"); }
        if (!size) break;
        if (text.size() + static_cast<std::size_t>(size) > 65536)
            throw std::runtime_error("Settings exceed 64 KiB: " + path.string());
        text.append(buffer.data(), static_cast<std::size_t>(size));
    }
    if (text.find('\0') != text.npos) throw std::runtime_error("NUL byte in settings: " + path.string());
    Settings settings;
    std::set<std::string_view> seen;
    std::size_t offset = 0, line_number = 0;
    while (offset < text.size()) {
        ++line_number;
        const auto end = text.find('\n', offset);
        auto line = trim(std::string_view(text).substr(offset, end == text.npos ? text.size() - offset : end - offset));
        offset = end == text.npos ? text.size() : end + 1;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        try {
            const auto equal = line.find('=');
            if (equal == line.npos) throw std::runtime_error("Expected key=value");
            const auto key = trim(line.substr(0, equal));
            const auto value = trim(line.substr(equal + 1));
            if (!seen.insert(key).second) throw std::runtime_error("Duplicate setting '" + std::string(key) + "'");
            assign(settings, key, value);
        } catch (const std::exception& error) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_number) + ": " + error.what());
        }
    }
    validate(settings);
    return settings;
}

void write_settings(const std::filesystem::path& path, const Settings& settings) {
    validate(settings);
    if (path.filename().empty() || path.filename() == "." || path.filename() == "..")
        throw std::runtime_error("Invalid settings filename: " + path.string());
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::filesystem::create_directories(parent);
    Descriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0) io_error(parent, "Cannot open settings directory");
    const auto filename = path.filename().string();
    struct stat existing{};
    if (::fstatat(directory.value, filename.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode)) throw std::runtime_error("Refusing nonregular settings file: " + path.string());
    } else if (errno != ENOENT) io_error(path, "Cannot inspect settings");
    std::string text;
    auto field = [&](const char* key, const auto& value) {
        text += key;
        text += '=';
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) text += value;
        else if constexpr (std::is_same_v<std::decay_t<decltype(value)>, bool>) text += value ? '1' : '0';
        else text += digits(value);
        text += '\n';
    };
    field("renderer", settings.renderer);
    field("width", settings.width); field("height", settings.height);
    field("fullscreen", settings.fullscreen); field("vsync", settings.vsync);
    field("reference_fov", settings.reference_fov); field("mouse_sensitivity", settings.mouse_sensitivity);
    field("msaa", settings.msaa); field("anisotropy", settings.anisotropy);
    field("gamma", settings.gamma); field("bloom", settings.bloom); field("ray_tracing", settings.ray_tracing);
    field("ray_shadows", settings.ray_shadows); field("ray_reflections", settings.ray_reflections);
    field("hdr_tonemapping", settings.hdr_tonemapping); field("exposure_ev", settings.exposure_ev);
    field("virtual_light_enabled", settings.virtual_light_enabled);
    field("virtual_light_azimuth", settings.virtual_light_azimuth); field("virtual_light_elevation", settings.virtual_light_elevation);
    field("virtual_light_red", settings.virtual_light_red); field("virtual_light_green", settings.virtual_light_green);
    field("virtual_light_blue", settings.virtual_light_blue); field("virtual_light_strength", settings.virtual_light_strength);
    field("master_volume", settings.master_volume); field("sfx_volume", settings.sfx_volume); field("music_volume", settings.music_volume);
    std::string temporary;
    Descriptor output(-1);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temporary = "." + filename + ".tmp-" + std::to_string(::getpid()) + "-" + std::to_string(attempt);
        output.value = ::openat(directory.value, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output.value >= 0) break;
        if (errno != EEXIST) io_error(path, "Cannot create temporary settings");
    }
    if (output.value < 0) throw std::runtime_error("No free temporary settings filename: " + path.string());
    bool renamed = false;
    try {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto count = ::write(output.value, text.data() + offset, text.size() - offset);
            if (count < 0) { if (errno == EINTR) continue; io_error(path, "Cannot write settings"); }
            if (!count) throw std::runtime_error("Zero-length write while saving settings");
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(output.value) < 0) io_error(path, "Cannot synchronize settings");
        if (::renameat(directory.value, temporary.c_str(), directory.value, filename.c_str()) < 0)
            io_error(path, "Cannot publish settings");
        renamed = true;
        if (::fsync(directory.value) < 0) io_error(parent, "Cannot synchronize settings directory");
    } catch (...) {
        if (!renamed) ::unlinkat(directory.value, temporary.c_str(), 0);
        throw;
    }
}

std::filesystem::path user_data_directory() { return xdg("XDG_DATA_HOME", ".local/share"); }
std::filesystem::path user_settings_file() { return xdg("XDG_CONFIG_HOME", ".config") / "settings.ini"; }

} // namespace pusu
