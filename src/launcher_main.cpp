#include "importer.hpp"
#include "settings.hpp"
#include "setup_art.hpp"

#include <gtk/gtk.h>
#include <json-c/json.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct Backend {
    std::string name, error, hardware;
    bool available{}, ray{}, msaa{}, anisotropy{}, bloom{}, gamma{}, vsync{};
    bool ray_shadows{}, ray_reflections{}, hdr_tonemapping{};
    int max_msaa{}, max_anisotropy{1};
    std::vector<int> msaa_values, anisotropy_values;
    std::set<std::string> present_modes;
};

std::string query(const fs::path& executable, const char* option) {
    std::string binary = executable.string();
    gchar* arguments[] = {binary.data(), const_cast<gchar*>(option), nullptr};
    gchar *output = nullptr, *diagnostic = nullptr;
    gint status = 0;
    GError* error = nullptr;
    if (!g_spawn_sync(nullptr, arguments, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                      &output, &diagnostic, &status, &error)) {
        std::string message = error->message;
        g_error_free(error);
        throw std::runtime_error("Cannot query native pusu-game: " + message);
    }
    std::unique_ptr<gchar, decltype(&g_free)> owned_output(output, g_free), owned_diagnostic(diagnostic, g_free);
    if (!g_spawn_check_wait_status(status, &error)) {
        std::string message = error->message;
        g_error_free(error);
        if (diagnostic && *diagnostic) message += "\n" + std::string(diagnostic);
        throw std::runtime_error(std::string("pusu-game ") + option + " failed: " + message);
    }
    if (!output || !*output) throw std::runtime_error(std::string("pusu-game ") + option + " returned no information.");
    return output;
}

json_object* member(json_object* object, const char* name, json_type type) {
    json_object* value = nullptr;
    if (!object || !json_object_object_get_ex(object, name, &value) || !json_object_is_type(value, type))
        throw std::runtime_error(std::string("Invalid native renderer capabilities field: ") + name);
    return value;
}

std::string text_value(json_object* value) {
    std::string result(json_object_get_string(value), json_object_get_string_len(value));
    if (result.find('\0') != std::string::npos)
        throw std::runtime_error("NUL byte in native renderer capabilities.");
    return result;
}

std::vector<Backend> discover(const fs::path& executable) {
    std::set<std::string> names;
    std::istringstream list(query(executable, "--list-renderers"));
    std::string name;
    while (std::getline(list, name)) {
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.empty()) continue;
        if (name != "opengl" && name != "vulkan") throw std::runtime_error("Native game reported an unknown settings backend: " + name);
        if (!names.insert(name).second) throw std::runtime_error("Native game reported a duplicate renderer: " + name);
    }
    if (names.empty()) throw std::runtime_error("The native game reports no implemented renderers.");
    const auto text = query(executable, "--capabilities");
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Native renderer capability JSON is too large.");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> tokener(json_tokener_new(), json_tokener_free);
    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT);
    std::unique_ptr<json_object, decltype(&json_object_put)> document(
        json_tokener_parse_ex(tokener.get(), text.c_str(), static_cast<int>(text.size())), json_object_put);
    if (json_tokener_get_error(tokener.get()) != json_tokener_success || !document)
        throw std::runtime_error("The native game returned malformed renderer capability JSON.");
    const auto consumed = json_tokener_get_parse_end(tokener.get());
    if (text.find_first_not_of(" \r\n\t", consumed) != std::string::npos)
        throw std::runtime_error("Unexpected text after native renderer capability JSON.");
    auto* entries = member(document.get(), "renderers", json_type_array);
    std::vector<Backend> result;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < json_object_array_length(entries); ++i) {
        auto* entry = json_object_array_get_idx(entries, i);
        Backend backend;
        backend.name = text_value(member(entry, "name", json_type_string));
        if (!names.contains(backend.name) || !seen.insert(backend.name).second)
            throw std::runtime_error("Renderer capabilities disagree with --list-renderers.");
        backend.available = json_object_get_boolean(member(entry, "available", json_type_boolean));
        auto* features = member(entry, "features", json_type_object);
        auto boolean = [&](const char* field) { return json_object_get_boolean(member(features, field, json_type_boolean)) != 0; };
        backend.ray = boolean("hardware_ray_tracing");
        backend.msaa = boolean("msaa");
        backend.anisotropy = boolean("anisotropy");
        backend.bloom = boolean("bloom");
        backend.gamma = boolean("gamma");
        backend.vsync = boolean("vsync");
        backend.ray_shadows = boolean("ray_shadows");
        backend.ray_reflections = boolean("ray_reflections");
        backend.hdr_tonemapping = boolean("hdr_tonemapping");
        auto* hardware = member(entry, "hardware", json_type_object);
        const auto max_msaa = json_object_get_int64(member(hardware, "max_msaa", json_type_int));
        const auto max_anisotropy = json_object_get_int64(member(hardware, "max_anisotropy", json_type_int));
        if (max_msaa < 0 || max_msaa > std::numeric_limits<int>::max() ||
            max_anisotropy < 1 || max_anisotropy > std::numeric_limits<int>::max())
            throw std::runtime_error("Invalid native hardware sampling limits.");
        backend.max_msaa = static_cast<int>(max_msaa);
        backend.max_anisotropy = static_cast<int>(max_anisotropy);
        auto values = [&](const char* field, bool msaa, int maximum) {
            std::vector<int> result;
            auto* array = member(hardware, field, json_type_array);
            for (std::size_t n = 0; n < json_object_array_length(array); ++n) {
                auto* item = json_object_array_get_idx(array, n);
                if (!json_object_is_type(item, json_type_int))
                    throw std::runtime_error(std::string("Invalid native sampling array: ") + field);
                const auto value = json_object_get_int64(item);
                if (!(value == (msaa ? 0 : 1) || value == 2 || value == 4 || value == 8 || value == 16) ||
                    value > maximum || std::find(result.begin(), result.end(), value) != result.end())
                    throw std::runtime_error(std::string("Invalid native sampling value: ") + field);
                result.push_back(static_cast<int>(value));
            }
            std::sort(result.begin(), result.end());
            if (backend.available && (result.empty() || result.front() != (msaa ? 0 : 1)))
                throw std::runtime_error(std::string("Missing native sampling baseline: ") + field);
            return result;
        };
        backend.msaa_values = values("msaa_values", true, backend.max_msaa);
        backend.anisotropy_values = values("anisotropy_values", false, backend.max_anisotropy);
        auto* modes = member(hardware, "present_modes", json_type_array);
        for (std::size_t n = 0; n < json_object_array_length(modes); ++n) {
            auto* item = json_object_array_get_idx(modes, n);
            if (!json_object_is_type(item, json_type_string))
                throw std::runtime_error("Invalid native presentation mode type.");
            const std::string mode = text_value(item);
            if ((mode != "fifo" && mode != "immediate" && mode != "mailbox" && mode != "fifo_relaxed") ||
                !backend.present_modes.insert(mode).second)
                throw std::runtime_error("Invalid or duplicate native presentation mode.");
        }
        backend.hardware = text_value(member(hardware, "vendor", json_type_string)) + " — " +
            text_value(member(hardware, "renderer", json_type_string)) + " (" +
            text_value(member(hardware, "version", json_type_string)) + ")";
        if (backend.available) {
            if (backend.present_modes.empty() ||
                backend.vsync != backend.present_modes.contains("fifo") ||
                backend.msaa != (backend.msaa_values.size() > 1) ||
                backend.anisotropy != (backend.anisotropy_values.size() > 1) ||
                ((backend.ray_shadows || backend.ray_reflections) && !backend.ray) ||
                (backend.name == "opengl" && (backend.ray || backend.ray_shadows || backend.ray_reflections || backend.hdr_tonemapping)))
                throw std::runtime_error("Inconsistent native renderer feature capabilities.");
        } else {
            backend.error = text_value(member(entry, "error", json_type_string));
            if (backend.error.empty() || backend.ray || backend.msaa || backend.anisotropy || backend.bloom ||
                backend.gamma || backend.vsync || backend.ray_shadows || backend.ray_reflections || backend.hdr_tonemapping ||
                !backend.msaa_values.empty() || !backend.anisotropy_values.empty() || !backend.present_modes.empty())
                throw std::runtime_error("Unavailable native renderer must report an error and no supported features.");
        }
        result.push_back(std::move(backend));
    }
    if (seen != names) throw std::runtime_error("Renderer capabilities omit an implemented backend.");
    return result;
}

struct Launcher {
    fs::path data, settings_path, executable;
    pusu::Settings settings;
    std::vector<Backend> backends;
    bool querying{true};
    GtkWidget *window{}, *grid{}, *status{}, *renderer{}, *width{}, *height{}, *fullscreen{}, *vsync{};
    GtkWidget *fov{}, *sensitivity{}, *msaa{}, *anisotropy{}, *gamma{}, *bloom{}, *ray{};
    GtkWidget *ray_shadows{}, *ray_reflections{}, *hdr{}, *exposure{}, *light{};
    GtkWidget *azimuth{}, *elevation{}, *red{}, *green{}, *blue{}, *strength{};
    GtkWidget *master{}, *sfx{}, *music{}, *save{}, *play{};
    std::string sampled_backend;
    bool updating{};
};

void show_error(Launcher& ui, const std::string& message) {
    auto* dialog = gtk_message_dialog_new(GTK_WINDOW(ui.window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void attach(Launcher& ui, int row, const char* text, GtkWidget* control, const char* help) {
    auto* label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), control);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_tooltip_text(control, help);
    gtk_grid_attach(GTK_GRID(ui.grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(ui.grid), control, 1, row, 1, 1);
}

GtkWidget* number(Launcher& ui, int row, const char* label, double minimum, double maximum,
                  double step, guint digits, double initial, const char* help) {
    auto* spin = gtk_spin_button_new_with_range(minimum, maximum, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), initial);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    attach(ui, row, label, spin, help);
    // Do not round untouched saved floats through the shorter displayed text.
    g_signal_connect(spin, "changed", G_CALLBACK(+[](GtkEditable* control, gpointer) {
        g_object_set_data(G_OBJECT(control), "edited", GINT_TO_POINTER(1));
    }), nullptr);
    return spin;
}

GtkWidget* toggle(Launcher& ui, int row, const char* label, bool initial, const char* help) {
    auto* control = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control), initial);
    attach(ui, row, label, control, help);
    return control;
}

const Backend* selected(const Launcher& ui) {
    const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui.renderer));
    if (!id) return nullptr;
    for (const auto& backend : ui.backends) if (backend.name == id) return &backend;
    return nullptr;
}

void samples(GtkWidget* combo, const std::vector<int>& values, int initial) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (int value : values) {
        const auto id = std::to_string(value);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id.c_str(), value == 0 ? "Off" : id.c_str());
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), std::to_string(initial).c_str());
}

pusu::Settings collect(Launcher& ui, bool commit_text = true) {
    const auto* backend = selected(ui);
    if (!backend) throw std::runtime_error("Saved renderer '" + ui.settings.renderer + "' is not implemented. Select an implemented renderer explicitly.");
    if (!backend->available) throw std::runtime_error(backend->name + " is unavailable: " + backend->error);
    pusu::Settings settings = ui.settings;
    settings.renderer = backend->name;
    auto value = [&](GtkWidget* widget) {
        if (commit_text && gtk_widget_get_sensitive(widget) && g_object_get_data(G_OBJECT(widget), "edited")) {
            gtk_spin_button_update(GTK_SPIN_BUTTON(widget));
            g_object_set_data(G_OBJECT(widget), "edited", nullptr);
        }
        return gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    };
    auto checked = [](GtkWidget* widget) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) != 0; };
    auto sample = [](GtkWidget* widget, const std::vector<int>& values, const char* name) {
        const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget));
        if (!id) throw std::runtime_error(std::string("Saved ") + name + " is unsupported. Select an exact hardware-supported value.");
        const auto result = std::stoi(id);
        if (std::find(values.begin(), values.end(), result) == values.end())
            throw std::runtime_error(std::string("Unsupported ") + name + " selection.");
        return result;
    };
    settings.width = static_cast<int>(value(ui.width));
    settings.height = static_cast<int>(value(ui.height));
    settings.fullscreen = checked(ui.fullscreen);
    settings.vsync = checked(ui.vsync);
    settings.reference_fov = static_cast<float>(value(ui.fov));
    settings.mouse_sensitivity = static_cast<float>(value(ui.sensitivity));
    settings.master_volume = static_cast<float>(value(ui.master));
    settings.sfx_volume = static_cast<float>(value(ui.sfx));
    settings.music_volume = static_cast<float>(value(ui.music));
    settings.msaa = sample(ui.msaa, backend->msaa_values, "MSAA");
    settings.anisotropy = sample(ui.anisotropy, backend->anisotropy_values, "anisotropy");
    settings.gamma = static_cast<float>(value(ui.gamma));
    settings.bloom = checked(ui.bloom);
    // Disabled OpenGL controls retain their displayed Vulkan preferences unchanged.
    settings.ray_tracing = checked(ui.ray);
    settings.ray_shadows = checked(ui.ray_shadows);
    settings.ray_reflections = checked(ui.ray_reflections);
    settings.hdr_tonemapping = checked(ui.hdr);
    settings.exposure_ev = static_cast<float>(value(ui.exposure));
    settings.virtual_light_enabled = checked(ui.light);
    settings.virtual_light_azimuth = static_cast<float>(value(ui.azimuth));
    settings.virtual_light_elevation = static_cast<float>(value(ui.elevation));
    settings.virtual_light_red = static_cast<float>(value(ui.red));
    settings.virtual_light_green = static_cast<float>(value(ui.green));
    settings.virtual_light_blue = static_cast<float>(value(ui.blue));
    settings.virtual_light_strength = static_cast<float>(value(ui.strength));
    if (settings.vsync ? !backend->present_modes.contains("fifo") :
        !(backend->present_modes.contains("immediate") || backend->present_modes.contains("mailbox") || backend->present_modes.contains("fifo_relaxed")))
        throw std::runtime_error(settings.vsync ? "VSync On is unsupported. Explicitly turn it off." : "VSync Off is unsupported. Explicitly turn it on.");
    if (settings.bloom && !backend->bloom)
        throw std::runtime_error("Bloom is unsupported. Explicitly disable it before saving or playing.");
    if (settings.gamma != 1.0f && !backend->gamma)
        throw std::runtime_error("Gamma correction is unsupported. Explicitly select gamma 1.");
    if (backend->name == "vulkan") {
        if (settings.ray_tracing && !backend->ray)
            throw std::runtime_error("Hardware ray tracing is unsupported. Explicitly disable the Vulkan master preference.");
        if (settings.ray_tracing && settings.ray_shadows && settings.virtual_light_enabled && !backend->ray_shadows)
            throw std::runtime_error("Hardware ray shadows are unsupported. Explicitly disable ray shadows or the enhanced directional light.");
        if (settings.ray_tracing && settings.ray_reflections && !backend->ray_reflections)
            throw std::runtime_error("Hardware ray reflections are unsupported. Explicitly disable ray reflections.");
        if (settings.hdr_tonemapping && !backend->hdr_tonemapping)
            throw std::runtime_error("HDR tone mapping is unsupported. Explicitly disable it.");
    }
    return settings;
}

void update_backend(Launcher& ui) {
    if (ui.updating) return;
    ui.updating = true;
    const Backend* backend = selected(ui);
    const bool available = backend && backend->available;
    const bool vulkan = available && backend->name == "vulkan";
    const bool ray = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.ray));
    const bool hdr = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.hdr));
    const bool light = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.light));
    // Keep unsupported saved values unselected, never replace them with a baseline.
    const std::string name = backend ? backend->name : "";
    if (ui.sampled_backend != name) {
        ui.sampled_backend = name;
        samples(ui.msaa, available ? backend->msaa_values : std::vector<int>{}, ui.settings.msaa);
        samples(ui.anisotropy, available ? backend->anisotropy_values : std::vector<int>{}, ui.settings.anisotropy);
    }
    for (auto* widget : {ui.msaa, ui.anisotropy, ui.gamma, ui.bloom, ui.vsync})
        gtk_widget_set_sensitive(widget, available);
    for (auto* widget : {ui.ray, ui.hdr, ui.light}) gtk_widget_set_sensitive(widget, vulkan);
    gtk_widget_set_sensitive(ui.ray_shadows, vulkan && ray);
    gtk_widget_set_sensitive(ui.ray_reflections, vulkan && ray);
    gtk_widget_set_sensitive(ui.exposure, vulkan && hdr);
    for (auto* widget : {ui.azimuth, ui.elevation, ui.red, ui.green, ui.blue, ui.strength})
        gtk_widget_set_sensitive(widget, vulkan && light);
    std::string message = available ? backend->hardware : "";
    if (available && !vulkan)
        message += "\nVulkan-only preferences are preserved but inactive in OpenGL.";
    if (vulkan) {
        if (!backend->ray) message += "\nThis hardware cannot provide ray queries; disable the saved master preference explicitly.";
        if (!ray) message += "\nHardware ray shadows and reflections are inactive: the ray tracing master is off.";
        if (!light) message += "\nRay shadows are inactive until Enhanced directional light is explicitly enabled.";
        message += "\nShadows affect ONLY the added directional light. Original baked shadows remain unchanged.";
        if (!hdr) message += "\nExposure is inactive: HDR tone mapping is off.";
        else message += "\nHDR is tone mapped to SDR; this is not HDR10 display output.";
    }
    bool valid = false;
    try { (void)collect(ui, false); valid = true; }
    catch (const std::exception& error) { message += "\n" + std::string(error.what()); }
    gtk_widget_set_sensitive(ui.save, valid);
    gtk_widget_set_sensitive(ui.play, valid);
    gtk_label_set_text(GTK_LABEL(ui.status), message.c_str());
    ui.updating = false;
}

void persist(Launcher& ui, bool play) {
    try {
        auto settings = collect(ui);
        if (!fs::is_directory(ui.data)) throw std::runtime_error("Game data directory does not exist: " + ui.data.string());
        pusu::write_settings(ui.settings_path, settings);
        ui.settings = settings;
        if (play) {
            std::string binary = ui.executable.string(), data = ui.data.string(), config = ui.settings_path.string();
            gchar* arguments[] = {binary.data(), const_cast<gchar*>("--data"), data.data(),
                const_cast<gchar*>("--settings"), config.data(), const_cast<gchar*>("--renderer"), settings.renderer.data(), nullptr};
            GError* error = nullptr;
            if (!g_spawn_async(nullptr, arguments, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &error)) {
                std::string message = error->message;
                g_error_free(error);
                throw std::runtime_error("Cannot start the native game: " + message);
            }
            gtk_label_set_text(GTK_LABEL(ui.status), "Settings saved. Native Pusu game launched.");
        } else gtk_label_set_text(GTK_LABEL(ui.status), "Settings saved to the selected settings file.");
    } catch (const std::exception& error) { show_error(ui, error.what()); }
}

void discover_worker(GTask* task, gpointer, gpointer data, GCancellable*) {
    auto& ui = *static_cast<Launcher*>(data);
    try {
        auto result = std::make_unique<std::vector<Backend>>(discover(ui.executable));
        g_task_return_pointer(task, result.release(), [](gpointer pointer) { delete static_cast<std::vector<Backend>*>(pointer); });
    } catch (const std::exception& error) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.what());
    } catch (...) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Unknown native renderer discovery failure");
    }
}

void discover_complete(GObject*, GAsyncResult* result, gpointer data) {
    auto& ui = *static_cast<Launcher*>(data);
    ui.querying = false;
    GError* error = nullptr;
    std::unique_ptr<std::vector<Backend>> backends(static_cast<std::vector<Backend>*>(g_task_propagate_pointer(G_TASK(result), &error)));
    if (!backends) {
        gtk_label_set_text(GTK_LABEL(ui.status), error->message);
        show_error(ui, error->message);
        g_error_free(error);
        return;
    }
    ui.backends = std::move(*backends);
    for (const auto& backend : ui.backends) {
        const auto label = backend.name + (backend.available ? "" : " — unavailable on this hardware");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.renderer), backend.name.c_str(), label.c_str());
    }
    gtk_widget_set_sensitive(ui.grid, TRUE);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.renderer), ui.settings.renderer.c_str());
    update_backend(ui);
    gtk_widget_grab_focus(ui.renderer);
}

int graphical_launcher(Launcher& ui) {
    const auto schemas = pusu::executable_directory() / "../share/glib-2.0/schemas";
    if (fs::is_directory(schemas)) g_setenv("GSETTINGS_SCHEMA_DIR", schemas.c_str(), TRUE);
    if (!gtk_init_check(nullptr, nullptr)) throw std::runtime_error("Cannot open a graphical display for the Pusu settings launcher.");
    ui.settings = pusu::read_settings(ui.settings_path);
    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.window), "Pusu — Native Game Settings");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 700, 820);
    gtk_container_set_border_width(GTK_CONTAINER(ui.window), 16);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(ui.window), box);
    const auto artwork = pusu::executable_directory() / "../share/pusu/installer.png";
    GdkPixbuf* pixels = nullptr;
    try { pixels = pusu::load_setup_artwork(artwork, 640, 150); }
    catch (...) { gtk_widget_destroy(ui.window); throw; }
    auto* banner = gtk_image_new_from_pixbuf(pixels);
    g_object_unref(pixels);
    atk_object_set_name(gtk_widget_get_accessible(banner), "Original Pusu game artwork");
    gtk_box_pack_start(GTK_BOX(box), banner, FALSE, FALSE, 0);
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    ui.grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(ui.grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(ui.grid), 16);
    gtk_container_add(GTK_CONTAINER(scroll), ui.grid);
    ui.renderer = gtk_combo_box_text_new();
    attach(ui, 0, "_Renderer:", ui.renderer, "Only native game backends reported by pusu-game are listed. Hardware errors are reported without fallback.");
    ui.width = number(ui, 1, "Window _width:", 320, 16384, 1, 0, ui.settings.width, "Rendering width in pixels; does not change the desktop display mode.");
    ui.height = number(ui, 2, "Window _height:", 200, 16384, 1, 0, ui.settings.height, "Rendering height in pixels.");
    ui.fullscreen = toggle(ui, 3, "_Fullscreen:", ui.settings.fullscreen, "Use the game's fullscreen window.");
    ui.vsync = toggle(ui, 4, "_Vertical sync:", ui.settings.vsync, "Synchronize presentation if the selected native renderer supports it.");
    ui.fov = number(ui, 5, "Reference field of vie_w:", 45, 130, 1, 1, ui.settings.reference_fov, "Horizontal degrees at 4:3. Widescreen expands horizontally while retaining the vertical field of view.");
    ui.sensitivity = number(ui, 6, "Mouse _sensitivity:", -100, 100, .01, 9, ui.settings.mouse_sensitivity, "Original sensitivity slider units: -100..100, neutral 0; fractional values are shown and editable. This is not an angle coefficient. The original controller applies the sensitivity mapping once.");
    ui.msaa = gtk_combo_box_text_new();
    attach(ui, 7, "_Multisample anti-aliasing:", ui.msaa, "Only sample counts supported by the selected renderer and hardware are selectable.");
    ui.anisotropy = gtk_combo_box_text_new();
    attach(ui, 8, "_Anisotropic filtering:", ui.anisotropy, "Only filtering levels supported by the selected renderer and hardware are selectable.");
    ui.gamma = number(ui, 9, "_Gamma:", .25, 4, .01, 2, ui.settings.gamma, "Native renderer gamma correction, from 0.25 to 4.");
    ui.bloom = toggle(ui, 10, "_Bloom:", ui.settings.bloom, "Enable bloom only if implemented and supported by the selected renderer.");
    ui.ray = toggle(ui, 11, "Vulkan hardware _ray tracing:", ui.settings.ray_tracing, "Vulkan-only master preference. Unsupported saved requests require an explicit disable; OpenGL preserves this preference without applying it.");
    ui.ray_shadows = toggle(ui, 12, "Hardware ray _shadows (enhanced light only):", ui.settings.ray_shadows, "Requires the ray tracing master and explicitly enabled Enhanced directional light. Shadows affect only this new light, never original baked shadows.");
    ui.ray_reflections = toggle(ui, 13, "Hardware ray re_flections:", ui.settings.ray_reflections, "Requires the ray tracing master. Replaces eligible original captured-cube stages with hardware ray-query reflections; authored probes remain on ray misses.");
    ui.hdr = toggle(ui, 14, "HDR _tone mapping to SDR:", ui.settings.hdr_tonemapping, "Vulkan floating-point light, exposure, Reinhard tone mapping and display transfer. This is not HDR10 monitor output. Ordinary UI is unlit after tone mapping.");
    ui.exposure = number(ui, 15, "Exposure (_EV):", -8, 8, .125, 6, ui.settings.exposure_ev, "Vulkan HDR exposure multiplier 2^EV; inactive when HDR tone mapping is disabled.");
    ui.light = toggle(ui, 16, "_Enhanced directional light:", ui.settings.virtual_light_enabled, "Opt-in new directional light, not a recovered original sun. Baked lighting and shadows are preserved. Hardware ray shadows affect only this added light.");
    ui.azimuth = number(ui, 17, "Light a_zimuth:", 0, 360, 1, 6, ui.settings.virtual_light_azimuth, "Degrees around the original Z-up world: 0 toward +X, 90 toward +Y.");
    ui.elevation = number(ui, 18, "Light ele_vation:", -90, 90, 1, 6, ui.settings.virtual_light_elevation, "Degrees toward the light: +90 toward +Z, -90 toward -Z.");
    ui.red = number(ui, 19, "Light _red:", 0, 1, .01, 6, ui.settings.virtual_light_red, "Added light's linear red component, 0 to 1.");
    ui.green = number(ui, 20, "Light _green:", 0, 1, .01, 6, ui.settings.virtual_light_green, "Added light's linear green component, 0 to 1.");
    ui.blue = number(ui, 21, "Light _blue:", 0, 1, .01, 6, ui.settings.virtual_light_blue, "Added light's linear blue component, 0 to 1.");
    ui.strength = number(ui, 22, "Light s_trength:", 0, 16, .05, 6, ui.settings.virtual_light_strength, "Calibrated added radiance strength, 0 to 16; not an original compiler sun unit.");
    ui.master = number(ui, 23, "Master v_olume:", 0, 1, .01, 2, ui.settings.master_volume, "Overall native audio volume, 0 silent to 1 full volume.");
    ui.sfx = number(ui, 24, "Sound _effects volume:", 0, 1, .01, 2, ui.settings.sfx_volume, "Sound effects and dialogue volume, 0 to 1.");
    ui.music = number(ui, 25, "M_usic volume:", 0, 1, .01, 2, ui.settings.music_volume, "Music volume, 0 to 1.");
    gtk_widget_set_sensitive(ui.grid, FALSE);
    ui.status = gtk_label_new("Querying native renderers and actual hardware capabilities…");
    gtk_label_set_line_wrap(GTK_LABEL(ui.status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.status), 80);
    gtk_label_set_selectable(GTK_LABEL(ui.status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), ui.status, FALSE, FALSE, 0);
    auto* buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    ui.save = gtk_button_new_with_mnemonic("_Save settings");
    ui.play = gtk_button_new_with_mnemonic("_Play Pusu");
    gtk_widget_set_sensitive(ui.save, FALSE);
    gtk_widget_set_sensitive(ui.play, FALSE);
    gtk_widget_set_can_default(ui.play, TRUE);
    gtk_window_set_default(GTK_WINDOW(ui.window), ui.play);
    gtk_container_add(GTK_CONTAINER(buttons), ui.save);
    gtk_container_add(GTK_CONTAINER(buttons), ui.play);
    gtk_box_pack_end(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    g_signal_connect(ui.renderer, "changed", G_CALLBACK(+[](GtkComboBox*, gpointer value) { update_backend(*static_cast<Launcher*>(value)); }), &ui);
    for (auto* widget : {ui.ray, ui.ray_shadows, ui.ray_reflections, ui.hdr, ui.light, ui.vsync, ui.bloom})
        g_signal_connect(widget, "toggled", G_CALLBACK(+[](GtkToggleButton*, gpointer value) {
            update_backend(*static_cast<Launcher*>(value));
        }), &ui);
    g_signal_connect(ui.gamma, "value-changed", G_CALLBACK(+[](GtkSpinButton*, gpointer value) {
        update_backend(*static_cast<Launcher*>(value));
    }), &ui);
    for (auto* widget : {ui.msaa, ui.anisotropy})
        g_signal_connect(widget, "changed", G_CALLBACK(+[](GtkComboBox* combo, gpointer value) {
            auto& state = *static_cast<Launcher*>(value);
            if (state.updating) return;
            if (const char* id = gtk_combo_box_get_active_id(combo)) {
                const int sample = std::stoi(id);
                if (GTK_WIDGET(combo) == state.msaa) state.settings.msaa = sample;
                else state.settings.anisotropy = sample;
            }
            update_backend(state);
        }), &ui);
    g_signal_connect(ui.save, "clicked", G_CALLBACK(+[](GtkButton*, gpointer value) { persist(*static_cast<Launcher*>(value), false); }), &ui);
    g_signal_connect(ui.play, "clicked", G_CALLBACK(+[](GtkButton*, gpointer value) { persist(*static_cast<Launcher*>(value), true); }), &ui);
    g_signal_connect(ui.window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer value) -> gboolean {
        auto& state = *static_cast<Launcher*>(value);
        if (state.querying) {
            show_error(state, "Native hardware discovery is still running. Wait for it to finish before closing.");
            return TRUE;
        }
        return FALSE;
    }), &ui);
    g_signal_connect(ui.window, "key-press-event", G_CALLBACK(+[](GtkWidget* window, GdkEventKey* event, gpointer) -> gboolean {
        if (event->keyval == GDK_KEY_Escape) { gtk_window_close(GTK_WINDOW(window)); return TRUE; }
        return FALSE;
    }), nullptr);
    g_signal_connect(ui.window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { gtk_main_quit(); }), nullptr);
    gtk_widget_show_all(ui.window);
    auto* task = g_task_new(nullptr, nullptr, discover_complete, &ui);
    g_task_set_task_data(task, &ui, nullptr);
    g_task_run_in_thread(task, discover_worker);
    g_object_unref(task);
    gtk_main();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        Launcher ui;
        bool data_set = false, settings_set = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "pusu-launcher [--data DIR] [--settings FILE]\nNative GTK3 settings launcher; discovers actual backends from the sibling pusu-game.\n";
                return 0;
            }
            if (argument != "--data" && argument != "--settings") throw std::runtime_error("Unknown launcher argument: " + argument);
            if (++i >= argc || !*argv[i]) throw std::runtime_error("Missing value for " + argument);
            if (argument == "--data") { ui.data = argv[i]; data_set = true; }
            else { ui.settings_path = argv[i]; settings_set = true; }
        }
        if (!data_set) ui.data = pusu::user_data_directory() / "data";
        if (!settings_set) ui.settings_path = pusu::user_settings_file();
        ui.data = fs::absolute(ui.data);
        ui.settings_path = fs::absolute(ui.settings_path);
        ui.executable = pusu::executable_directory() / "pusu-game";
        return graphical_launcher(ui);
    } catch (const std::exception& error) {
        std::cerr << "pusu-launcher: " << error.what() << '\n';
        if (gdk_display_get_default()) {
            auto* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE, "%s", error.what());
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
        return 1;
    }
}
