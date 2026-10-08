#include "importer.hpp"
#include "settings.hpp"
#include "setup_art.hpp"

#include <gtk/gtk.h>
#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;

void error_dialog(GtkWindow* parent, const std::string& message) {
    auto* dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

bool spawn(const fs::path& executable, const fs::path& data,
           const fs::path& settings, std::string& error) {
    std::string binary = executable.string(), root = data.string(), config = settings.string();
    gchar* args[] = {binary.data(), const_cast<gchar*>("--data"), root.data(),
        const_cast<gchar*>("--settings"), config.data(), nullptr};
    GError* detail = nullptr;
    bool ok = g_spawn_async(nullptr, args, nullptr, G_SPAWN_DEFAULT,
                           nullptr, nullptr, nullptr, &detail);
    if (!ok) { error = detail->message; g_error_free(detail); }
    return ok;
}

struct Installer {
    GtkWidget *window{}, *source{}, *destination{}, *desktop{}, *desktop_enabled{};
    GtkWidget *install{}, *launch{}, *progress{}, *status{};
    GtkWidget* input_grid{};
    bool busy{}, installed{};
    fs::path installed_directory;
    std::mutex mutex;
    double fraction{};
    std::string message;
};

GtkWidget* labelled_entry(GtkWidget* grid, int row, const char* label_text) {
    auto* label = gtk_label_new_with_mnemonic(label_text);
    auto* entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), entry);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    return entry;
}

void choose(GtkButton* button, gpointer value) {
    auto* state = static_cast<Installer*>(value);
    const bool source = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "source"));
    auto* entry = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(button), "entry"));
    auto* dialog = gtk_file_chooser_dialog_new(source ? "Select original Pusu disc image" : "Select destination folder",
        GTK_WINDOW(state->window), source ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, nullptr);
    if (source) {
        auto* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "Pusu disc images (CUE, BIN, ISO)");
        gtk_file_filter_add_custom(filter, GTK_FILE_FILTER_FILENAME,
            +[](const GtkFileFilterInfo* info, gpointer) -> gboolean {
                const char* extension = info->filename ? g_strrstr(info->filename, ".") : nullptr;
                return extension && (g_ascii_strcasecmp(extension, ".cue") == 0 ||
                    g_ascii_strcasecmp(extension, ".bin") == 0 || g_ascii_strcasecmp(extension, ".iso") == 0);
            }, nullptr, nullptr);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    }
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) { gtk_entry_set_text(GTK_ENTRY(entry), path); g_free(path); }
    }
    gtk_widget_destroy(dialog);
}

void browse(GtkWidget* grid, int row, GtkWidget* entry, bool source, Installer& state) {
    auto* button = gtk_button_new_with_mnemonic(source ? "_Browse image…" : "Choose _folder…");
    g_object_set_data(G_OBJECT(button), "source", GINT_TO_POINTER(source));
    g_object_set_data(G_OBJECT(button), "entry", entry);
    g_signal_connect(button, "clicked", G_CALLBACK(choose), &state);
    gtk_grid_attach(GTK_GRID(grid), button, 2, row, 1, 1);
}

struct ImportJob { Installer* ui; pusu::InstallRequest request; };

void install_worker(GTask* task, gpointer, gpointer data, GCancellable*) {
    auto* job = static_cast<ImportJob*>(data);
    try {
        pusu::install_source(job->request, [ui = job->ui](double fraction, std::string_view message) {
            std::lock_guard lock(ui->mutex);
            ui->fraction = std::clamp(fraction, 0.0, 1.0);
            ui->message = message;
        });
        g_task_return_boolean(task, TRUE);
    } catch (const std::exception& error) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.what());
    } catch (...) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Unknown installation failure");
    }
}

void install_complete(GObject*, GAsyncResult* result, gpointer data) {
    auto& ui = *static_cast<Installer*>(data);
    GError* error = nullptr;
    bool ok = g_task_propagate_boolean(G_TASK(result), &error);
    ui.busy = false;
    gtk_widget_set_sensitive(ui.input_grid, TRUE);
    gtk_widget_set_sensitive(ui.source, TRUE);
    gtk_widget_set_sensitive(ui.destination, TRUE);
    gtk_widget_set_sensitive(ui.desktop_enabled, TRUE);
    gtk_widget_set_sensitive(ui.desktop, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.desktop_enabled)));
    gtk_widget_set_sensitive(ui.install, TRUE);
    ui.installed = ok;
    gtk_widget_set_sensitive(ui.launch, ok);
    if (ok) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.progress), 1);
        gtk_label_set_text(GTK_LABEL(ui.status), "Installation complete. Open the launcher to configure and play Pusu.");
        gtk_widget_grab_focus(ui.launch);
    } else {
        gtk_label_set_text(GTK_LABEL(ui.status), error->message);
        error_dialog(GTK_WINDOW(ui.window), error->message);
        g_error_free(error);
        gtk_widget_grab_focus(ui.source);
    }
}

void start_install(GtkButton*, gpointer data) {
    auto& ui = *static_cast<Installer*>(data);
    try {
        pusu::InstallRequest request;
        request.source = gtk_entry_get_text(GTK_ENTRY(ui.source));
        request.destination = gtk_entry_get_text(GTK_ENTRY(ui.destination));
        request.runtime_directory = pusu::executable_directory();
        request.applications_directory = fs::path(g_get_user_data_dir()) / "applications";
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.desktop_enabled)))
            request.desktop_directory = gtk_entry_get_text(GTK_ENTRY(ui.desktop));
        if (request.source.empty() || request.destination.empty())
            throw std::runtime_error("Select the original disc image and an installation destination.");
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.desktop_enabled)) && request.desktop_directory.empty())
            throw std::runtime_error("Choose a desktop shortcut directory, or disable the desktop shortcut.");
        ui.installed_directory = request.destination;
        ui.busy = true;
        ui.installed = false;
        gtk_widget_set_sensitive(ui.input_grid, FALSE);
        for (auto* widget : {ui.source, ui.destination, ui.desktop_enabled, ui.desktop, ui.install, ui.launch})
            gtk_widget_set_sensitive(widget, FALSE);
        {
            std::lock_guard lock(ui.mutex);
            ui.fraction = 0;
            ui.message = "Reading original installation media…";
        }
        auto* task = g_task_new(nullptr, nullptr, install_complete, &ui);
        g_task_set_task_data(task, new ImportJob{&ui, std::move(request)},
            [](gpointer job) { delete static_cast<ImportJob*>(job); });
        g_task_run_in_thread(task, install_worker);
        g_object_unref(task);
    } catch (const std::exception& error) { error_dialog(GTK_WINDOW(ui.window), error.what()); }
}

int graphical_install() {
    const auto schemas = pusu::executable_directory() / "../share/glib-2.0/schemas";
    if (fs::is_directory(schemas)) g_setenv("GSETTINGS_SCHEMA_DIR", schemas.c_str(), TRUE);
    if (!gtk_init_check(nullptr, nullptr)) throw std::runtime_error("Cannot open a graphical display. Use --install or --extract-only for command-line installation.");
    Installer ui;
    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.window), "Pusu — Native Linux Installation");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 760, 520);
    gtk_container_set_border_width(GTK_CONTAINER(ui.window), 18);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_add(GTK_CONTAINER(ui.window), box);
    const auto artwork = pusu::executable_directory() / "../share/pusu/installer.png";
    GdkPixbuf* image = nullptr;
    try { image = pusu::load_setup_artwork(artwork, 700, 220); }
    catch (...) { gtk_widget_destroy(ui.window); throw; }
    auto* banner = gtk_image_new_from_pixbuf(image);
    g_object_unref(image);
    atk_object_set_name(gtk_widget_get_accessible(banner), "Original Pusu game artwork");
    gtk_box_pack_start(GTK_BOX(box), banner, FALSE, FALSE, 0);
    auto* instructions = gtk_label_new("Import only original Pusu media you own. This independent native Linux implementation\nimports assets locally and runs no Windows programs. Original artwork and assets retain\ntheir owners' rights; importing does not accept or change any licence terms.");
    gtk_label_set_line_wrap(GTK_LABEL(instructions), TRUE);
    gtk_box_pack_start(GTK_BOX(box), instructions, FALSE, FALSE, 0);
    auto* grid = gtk_grid_new();
    ui.input_grid = grid;
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    ui.source = labelled_entry(grid, 0, "Disc _image:");
    browse(grid, 0, ui.source, true, ui);
    ui.destination = labelled_entry(grid, 1, "_Installation folder:");
    gtk_entry_set_text(GTK_ENTRY(ui.destination), pusu::user_data_directory().c_str());
    browse(grid, 1, ui.destination, false, ui);
    ui.desktop = labelled_entry(grid, 2, "_Desktop folder:");
    if (const char* desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP)) gtk_entry_set_text(GTK_ENTRY(ui.desktop), desktop);
    gtk_widget_set_sensitive(ui.desktop, FALSE);
    browse(grid, 2, ui.desktop, false, ui);
    ui.desktop_enabled = gtk_check_button_new_with_mnemonic("Create a desktop _shortcut (in addition to the applications menu)");
    gtk_grid_attach(GTK_GRID(grid), ui.desktop_enabled, 0, 3, 3, 1);
    g_signal_connect(ui.desktop_enabled, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer value) {
        auto& state = *static_cast<Installer*>(value);
        gtk_widget_set_sensitive(state.desktop, !state.busy && gtk_toggle_button_get_active(button));
    }), &ui);
    ui.progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.progress), TRUE);
    atk_object_set_name(gtk_widget_get_accessible(ui.progress), "Installation progress");
    gtk_box_pack_start(GTK_BOX(box), ui.progress, FALSE, FALSE, 0);
    ui.status = gtk_label_new("Choose an image and destination, then select Install.");
    gtk_label_set_line_wrap(GTK_LABEL(ui.status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.status), 85);
    gtk_label_set_selectable(GTK_LABEL(ui.status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), ui.status, FALSE, FALSE, 0);
    auto* buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    ui.install = gtk_button_new_with_mnemonic("_Install");
    gtk_widget_set_can_default(ui.install, TRUE);
    gtk_window_set_default(GTK_WINDOW(ui.window), ui.install);
    ui.launch = gtk_button_new_with_mnemonic("_Open launcher");
    gtk_widget_set_sensitive(ui.launch, FALSE);
    gtk_container_add(GTK_CONTAINER(buttons), ui.install);
    gtk_container_add(GTK_CONTAINER(buttons), ui.launch);
    gtk_box_pack_end(GTK_BOX(box), buttons, FALSE, FALSE, 0);
    g_signal_connect(ui.install, "clicked", G_CALLBACK(start_install), &ui);
    g_signal_connect(ui.launch, "clicked", G_CALLBACK(+[](GtkButton*, gpointer value) {
        auto& state = *static_cast<Installer*>(value);
        std::string error;
        if (!spawn(state.installed_directory / "bin/pusu-launcher", state.installed_directory / "data", pusu::user_settings_file(), error))
            error_dialog(GTK_WINDOW(state.window), error);
    }), &ui);
    g_signal_connect(ui.window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer value) -> gboolean {
        auto& state = *static_cast<Installer*>(value);
        if (state.busy) {
            error_dialog(GTK_WINDOW(state.window), "Installation is in progress. Wait for it to finish before closing this window.");
            return TRUE;
        }
        return FALSE;
    }), &ui);
    g_signal_connect(ui.window, "key-press-event", G_CALLBACK(+[](GtkWidget* window, GdkEventKey* event, gpointer) -> gboolean {
        if (event->keyval == GDK_KEY_Escape) { gtk_window_close(GTK_WINDOW(window)); return TRUE; }
        return FALSE;
    }), nullptr);
    g_signal_connect(ui.window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { gtk_main_quit(); }), nullptr);
    guint timer = g_timeout_add(100, +[](gpointer value) -> gboolean {
        auto& state = *static_cast<Installer*>(value);
        if (state.busy) {
            std::lock_guard lock(state.mutex);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state.progress), state.fraction);
            gtk_label_set_text(GTK_LABEL(state.status), state.message.c_str());
        }
        return G_SOURCE_CONTINUE;
    }, &ui);
    gtk_widget_show_all(ui.window);
    gtk_widget_grab_focus(ui.source);
    gtk_main();
    g_source_remove(timer);
    return 0;
}

void usage() {
    std::cout << "Pusu native installer\n"
        "  pusu-installer                         graphical installer\n"
        "  pusu-installer --extract-only SOURCE --destination DIR\n"
        "  pusu-installer --install --cue SOURCE --destination DIR --applications-dir DIR [--desktop-dir DIR]\n"
        "SOURCE may be a CUE, BIN, or ISO image. Command-line modes require explicit destinations.\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) return graphical_install();
        pusu::InstallRequest request;
        bool extract = false, install = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            auto value = [&]() -> fs::path {
                if (++i >= argc || !*argv[i]) throw std::runtime_error("Missing value for " + argument);
                return argv[i];
            };
            if (argument == "--help") { usage(); return 0; }
            if (argument == "--extract-only") { extract = true; request.source = value(); }
            else if (argument == "--install") install = true;
            else if (argument == "--cue") request.source = value();
            else if (argument == "--destination") request.destination = value();
            else if (argument == "--applications-dir") request.applications_directory = value();
            else if (argument == "--desktop-dir") request.desktop_directory = value();
            else throw std::runtime_error("Unknown argument: " + argument);
        }
        if (extract == install || request.source.empty() || request.destination.empty())
            throw std::runtime_error("Specify exactly one of --extract-only SOURCE or --install --cue SOURCE, and --destination DIR.");
        if (install && request.applications_directory.empty()) throw std::runtime_error("--install requires --applications-dir DIR.");
        if (extract && (!request.applications_directory.empty() || !request.desktop_directory.empty()))
            throw std::runtime_error("Shortcut directories apply only to --install.");
        auto progress = [](double fraction, std::string_view message) {
            std::cerr << static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100) << "% " << message << '\n';
        };
        if (extract) pusu::extract_source(request.source, request.destination, progress);
        else {
            request.runtime_directory = pusu::executable_directory();
            pusu::install_source(request, progress);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pusu-installer: " << error.what() << '\n';
        if (gdk_display_get_default()) error_dialog(nullptr, error.what());
        return 1;
    }
}
