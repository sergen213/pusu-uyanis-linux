#include "importer.hpp"

#include <libmsi.h>
#include <glib.h>
#include <sys/stat.h>
#include <utility>
#include <openssl/evp.h>
#include <array>
#include <memory>
#include <cassert>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct PrivateDirectory {
    fs::path root;
    PrivateDirectory() {
        auto pattern = (fs::temp_directory_path() / "pusu-importer-boundary-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const auto created = ::mkdtemp(buffer.data());
        if (!created) throw std::runtime_error("mkdtemp failed");
        root = created;
    }
    ~PrivateDirectory() {
        std::error_code error;
        fs::remove_all(root, error); // Only this executable's private mkdtemp tree.
    }
};

static void put(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot write private fixture");
}

// Include directories, contents, and symlink targets: rejection may not publish
// output, erase a sentinel, alter a link, or leave staging debris anywhere here.
static std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto key = entry.path().lexically_relative(root).generic_string();
        const auto status = entry.symlink_status();
        if (fs::is_symlink(status)) {
            result.emplace(key, "link:" + fs::read_symlink(entry.path()).generic_string());
        } else if (fs::is_directory(status)) {
            result.emplace(key, "directory");
        } else if (fs::is_regular_file(status)) {
            std::ifstream stream(entry.path(), std::ios::binary);
            if (!stream) throw std::runtime_error("cannot read private fixture");
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
            if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
                throw std::runtime_error("cannot initialize snapshot SHA256");
            std::array<char, 64 * 1024> buffer;
            std::uintmax_t size = 0;
            while (stream) {
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = stream.gcount();
                if (count > 0) {
                    size += static_cast<std::uintmax_t>(count);
                    if (EVP_DigestUpdate(hash.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
                        throw std::runtime_error("cannot update snapshot SHA256");
                }
            }
            if (!stream.eof() || stream.bad()) throw std::runtime_error("cannot read complete private fixture");
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
            unsigned int digest_size = 0;
            if (EVP_DigestFinal_ex(hash.get(), digest.data(), &digest_size) != 1)
                throw std::runtime_error("cannot finalize snapshot SHA256");
            result.emplace(key, "file:" + std::to_string(size) + ":" +
                                std::string(reinterpret_cast<const char*>(digest.data()), digest_size));
        } else {
            throw std::runtime_error("unexpected private fixture entry");
        }
    }
    return result;
}
static void reserve_msi_filename(const fs::path& msi, const char* reserved) {
    GError* error = nullptr;
    const auto checked = [&](bool success) {
        if (success && !error) return;
        const std::string message = error ? error->message : "native MSI operation failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
    };
    const auto release = [](auto* object) { if (object) g_object_unref(object); };
    std::unique_ptr<LibmsiDatabase, decltype(release)> database(
        libmsi_database_new(msi.c_str(), LIBMSI_DB_FLAGS_TRANSACT, nullptr, &error), release);
    checked(database != nullptr);
    std::unique_ptr<LibmsiQuery, decltype(release)> select(
        libmsi_query_new(database.get(), "SELECT `File`, `FileName` FROM `File`", &error), release);
    checked(select != nullptr);
    checked(libmsi_query_execute(select.get(), nullptr, &error));
    std::string file_key;
    for (;;) {
        std::unique_ptr<LibmsiRecord, decltype(release)> record(libmsi_query_fetch(select.get(), &error), release);
        checked(error == nullptr);
        if (!record) break;
        std::unique_ptr<gchar, decltype(&g_free)> name(libmsi_record_get_string(record.get(), 2), g_free);
        if (!name) throw std::runtime_error("missing MSI filename");
        std::string long_name = name.get();
        const auto separator = long_name.find('|');
        if (separator != std::string::npos) long_name.erase(0, separator + 1);
        if (long_name == "langs.txt" || long_name == "langt.txt") {
            std::unique_ptr<gchar, decltype(&g_free)> key(libmsi_record_get_string(record.get(), 1), g_free);
            if (!key) throw std::runtime_error("missing MSI file key");
            file_key = key.get();
            break;
        }
    }
    if (file_key.empty()) throw std::runtime_error("genuine MSI lacks reserved-name fixture candidate");
    checked(libmsi_query_close(select.get(), &error));
    std::unique_ptr<LibmsiQuery, decltype(release)> update(
        libmsi_query_new(database.get(), "UPDATE `File` SET `FileName` = ? WHERE `File` = ?", &error), release);
    checked(update != nullptr);
    std::unique_ptr<LibmsiRecord, decltype(release)> values(libmsi_record_new(2), release);
    checked(values != nullptr);
    checked(libmsi_record_set_string(values.get(), 1, reserved));
    checked(libmsi_record_set_string(values.get(), 2, file_key.c_str()));
    checked(libmsi_query_execute(update.get(), values.get(), &error));
    checked(libmsi_query_close(update.get(), &error));
    checked(libmsi_database_commit(database.get(), &error));
}


int main(int argc, char** argv) {
#ifdef NDEBUG
#error Compile this assert-based check without NDEBUG (or pass -UNDEBUG).
#endif
    if (argc != 1 && argc != 3 && argc != 4) {
        std::cerr << "Usage: importer-boundary-check [REAL_SOURCE REAL_RUNTIME [REAL_MSI]]\n";
        return 2;
    }
    PrivateDirectory private_directory;
    const auto& root = private_directory.root;
    for (const auto* name : {"home", "config", "data", "cache", "runtime", "applications", "desktop", "existing", "outside", "source-directory"})
        fs::create_directory(root / name);
    // Prevent even incidental settings access from touching the real home.
    for (const auto& [name, directory] : std::map<std::string, std::string>{
             {"HOME", "home"}, {"XDG_CONFIG_HOME", "config"},
             {"XDG_DATA_HOME", "data"}, {"XDG_CACHE_HOME", "cache"}}) {
        if (::setenv(name.c_str(), (root / directory).c_str(), 1) != 0)
            throw std::runtime_error("cannot isolate environment");
    }
    put(root / "existing" / "unrelated-sentinel", std::string("unrelated destination contents") + '\0' + "remain intact");
    put(root / "outside" / "unrelated-sentinel", "symlink target must remain intact");
    put(root / "malformed.iso", std::string(4096, '\0'));
    put(root / "malformed.cue", "THIS IS NOT A CUE SHEET\n");
    put(root / "outside.bin", std::string(4096, '\0'));
    put(root / "unsafe.cue", "FILE \"../outside.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    fs::create_symlink(root / "malformed.iso", root / "source-link.iso");
    fs::create_directory_symlink(root / "source-directory", root / "source-directory-link");
    fs::create_directory_symlink(root / "outside", root / "destination-link");

    const auto reject = [&](const fs::path& source, const fs::path& destination, bool install) {
        const auto before = snapshot(root);
        bool rejected = false;
        try {
            if (install) {
                pusu::install_source({source, destination, root / "runtime",
                                      root / "applications", root / "desktop"});
            } else {
                pusu::extract_source(source, destination);
            }
        } catch (const std::exception&) {
            rejected = true;
        }
        if (!rejected) std::cerr << "Unexpected acceptance: " << source << " -> " << destination
                                 << (install ? " (install)\n" : " (extract)\n");
        assert(rejected);
        assert(snapshot(root) == before);
    };

    for (const bool install : {false, true}) {
        for (const auto* name : {"missing.iso", "malformed.iso", "malformed.cue", "unsafe.cue",
                                 "source-link.iso", "source-directory-link"}) {
            reject(root / name, root / "unpublished", install);
            assert(!fs::exists(root / "unpublished"));
            reject(root / name, root / "existing", install);
        }
        // Unsafe destination itself and an unsafe ancestor must never be followed.
        reject(root / "source-directory", root / "destination-link", install);
        reject(root / "source-directory", root / "destination-link" / "unpublished", install);
        assert(!fs::exists(root / "outside" / "unpublished"));
        // A destination which is a regular file must also remain byte-for-byte intact.
        reject(root / "source-directory", root / "existing" / "unrelated-sentinel", install);
    }
    if (argc >= 3) {
        const auto source = fs::absolute(argv[1]);
        const auto runtime = fs::absolute(argv[2]);
        const auto extracted = root / "real-extracted";
        pusu::extract_source(source, extracted);
        assert(fs::file_size(extracted / "font" / "console.fnt") == 1376);
        for (const auto& entry : fs::recursive_directory_iterator(extracted)) {
            assert(entry.path().filename() != "Pusu.exe");
            assert(entry.path().filename() != "Engine.dll");
        }
        std::size_t asset_files = 0;
        std::map<std::string, std::size_t> extensions;
        for (const auto& entry : fs::recursive_directory_iterator(extracted)) {
            if (!entry.is_regular_file()) continue;
            const auto relative = entry.path().lexically_relative(extracted);
            if (*relative.begin() == ".pusu-media-notices" || relative == ".pusu-native-owned") continue;
            ++asset_files;
            auto extension = entry.path().extension().string();
            for (auto& character : extension)
                if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
            ++extensions[extension];
        }
        assert(asset_files == 9486);
        for (const auto& [extension, count] : std::map<std::string, std::size_t>{
                 {".pl", 20}, {".pcs", 30}, {".pm", 2544}, {".po", 1375},
                 {".pa", 360}, {".pka", 228}, {".ogg", 1542}, {".avi", 10},
                 {".pa---", 1}, {".pa1", 1}, {".pka-camra_circle", 1}, {".pka-", 1}, {"", 18}})
            assert(extensions[extension] == count);
        const auto shader_times = [](const fs::path& directory) {
            std::map<std::string, fs::file_time_type> times;
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (!entry.is_regular_file()) continue;
                auto extension = entry.path().extension().string();
                for (auto& character : extension)
                    if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
                if (extension == ".shader")
                    times.emplace(entry.path().lexically_relative(directory).generic_string(),
                                  entry.last_write_time());
            }
            return times;
        };
        const auto extracted_shader_times = shader_times(extracted);
        assert(!extracted_shader_times.empty());
        assert(extracted_shader_times.at("script/obj_covered_objects_02.shader") -
                   extracted_shader_times.at("script/3te_stock_machine.shader") ==
               std::chrono::seconds(2));
        const auto before_race = snapshot(root);
        const auto raced_destination = root / "appearing-destination";
        bool destination_appeared = false;
        bool race_rejected = false;
        std::map<std::string, std::string> raced_contents;
        try {
            pusu::extract_source(source, raced_destination, [&](double fraction, std::string_view) {
                if (fraction != 0.35 || destination_appeared) return;
                fs::create_directory(raced_destination);
                put(raced_destination / "unrelated-sentinel", "concurrent unrelated destination");
                raced_contents = snapshot(raced_destination);
                destination_appeared = true;
            });
        } catch (const std::exception&) {
            race_rejected = true;
        }
        assert(destination_appeared);
        assert(race_rejected);
        assert(snapshot(raced_destination) == raced_contents);
        fs::remove_all(raced_destination); // Only this callback's private fixture.
        assert(snapshot(root) == before_race);
        struct CabinetProgressCancellation final : std::runtime_error {
            CabinetProgressCancellation() : std::runtime_error("distinctive CAB callback cancellation") {}
        };
        const auto before_cancel = snapshot(root);
        bool cabinet_callback_reached = false;
        bool exact_cancellation_recovered = false;
        try {
            pusu::extract_source(source, root / "cancelled-extraction",
                                 [&](double fraction, std::string_view) {
                if (fraction > 0.35 && fraction < 0.9) {
                    cabinet_callback_reached = true;
                    throw CabinetProgressCancellation{};
                }
            });
        } catch (const CabinetProgressCancellation& error) {
            exact_cancellation_recovered =
                std::string(error.what()) == "distinctive CAB callback cancellation";
        }
        assert(cabinet_callback_reached);
        assert(exact_cancellation_recovered);
        assert(!fs::exists(root / "cancelled-extraction"));
        assert(snapshot(root) == before_cancel);
        const auto installed = root / "Pusu: Uyanış % $ \" ` \\ installed";
        const pusu::InstallRequest request{source, installed, runtime,
                                          root / "applications", root / "desktop"};
        pusu::install_source(request);
        assert(fs::is_regular_file(installed / "bin" / "pusu-game"));
        assert(fs::is_regular_file(installed / "bin" / "pusu-launcher"));
        assert(fs::is_directory(installed / "lib"));
        assert(fs::is_directory(installed / "share"));
        assert(fs::file_size(installed / "data" / "font" / "console.fnt") == 1376);
        const auto runtime_bin = fs::is_regular_file(runtime / "pusu-game") ? runtime : runtime / "bin";
        const auto runtime_lib = fs::is_directory(runtime / "lib") ? runtime / "lib" : runtime_bin.parent_path() / "lib";
        const auto runtime_share = fs::is_directory(runtime / "share") ? runtime / "share" : runtime_bin.parent_path() / "share";
        assert(snapshot(installed / "lib") == snapshot(runtime_lib));
        assert(snapshot(installed / "share") == snapshot(runtime_share));
        const auto application_file = root / "applications" / "pusu-native.desktop";
        std::unique_ptr<GKeyFile, decltype(&g_key_file_free)> desktop_entry(g_key_file_new(), g_key_file_free);
        GError* desktop_error = nullptr;
        assert(g_key_file_load_from_file(desktop_entry.get(), application_file.c_str(), G_KEY_FILE_NONE, &desktop_error));
        assert(!desktop_error);
        std::unique_ptr<gchar, decltype(&g_free)> command(
            g_key_file_get_string(desktop_entry.get(), "Desktop Entry", "Exec", &desktop_error), g_free);
        assert(command && !desktop_error);
        gint argument_count = 0;
        gchar** arguments = nullptr;
        assert(g_shell_parse_argv(command.get(), &argument_count, &arguments, &desktop_error));
        std::unique_ptr<gchar*, decltype(&g_strfreev)> owned_arguments(arguments, g_strfreev);
        assert(!desktop_error && argument_count == 5);
        // Desktop Exec expands %% to a literal % after key-file and argv decoding.
        const auto escaped_percent = [](const fs::path& path) {
            std::string value;
            for (char character : path.string()) {
                value += character;
                if (character == '%') value += '%';
            }
            return value;
        };
        assert(arguments[0] == escaped_percent(installed / "bin/pusu-launcher"));
        assert(std::string(arguments[1]) == "--data");
        assert(arguments[2] == escaped_percent(installed / "data"));
        assert(std::string(arguments[3]) == "--settings");
        assert(std::string(arguments[4]) == (root / "config/pusu/settings.ini").string());
        std::unique_ptr<gchar, decltype(&g_free)> desktop_icon(
            g_key_file_get_string(desktop_entry.get(), "Desktop Entry", "Icon", &desktop_error), g_free);
        assert(desktop_icon && !desktop_error);
        assert(fs::is_regular_file(desktop_icon.get()));
        assert(snapshot(root / "applications") == snapshot(root / "desktop"));
        assert(!fs::exists(root / "config/pusu/settings.ini"));
        // "..name" is a descendant component, not the ".." parent component.
        for (unsigned inside = 0; inside < 3; ++inside) {
            auto unsafe_request = request;
            if (inside == 2) {
                assert(::setenv("XDG_CONFIG_HOME", (installed / "..config").c_str(), 1) == 0);
            } else if (inside == 1) {
                unsafe_request.applications_directory = installed / "..applications";
            } else {
                unsafe_request.desktop_directory = installed / "..desktop";
            }
            const auto before_containment = snapshot(root);
            bool containment_rejected = false;
            try { pusu::install_source(unsafe_request); }
            catch (const std::exception&) { containment_rejected = true; }
            assert(containment_rejected);
            assert(snapshot(root) == before_containment);
            assert(::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1) == 0);
        }
        auto extracted_assets = snapshot(extracted);
        extracted_assets.erase(".pusu-native-owned");
        assert(extracted_assets == snapshot(installed / "data"));
        assert(shader_times(installed / "data") == extracted_shader_times);
        put(installed / "data" / "unknown-save.sav", std::string("saved") + '\0' + "game");
        put(installed / "unknown-settings.ini", "unknown settings bytes\n");
        const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(48);
        fs::last_write_time(installed / "data" / "unknown-save.sav", old_time);
        fs::last_write_time(installed / "unknown-settings.ini", old_time - std::chrono::hours(1));
        const auto save_time = fs::last_write_time(installed / "data" / "unknown-save.sav");
        const auto settings_time = fs::last_write_time(installed / "unknown-settings.ini");
        const auto identity = [](const fs::path& path) {
            struct stat status {};
            if (::stat(path.c_str(), &status) != 0) throw std::runtime_error("cannot stat private fixture");
            return std::pair{status.st_dev, status.st_ino};
        };
        const auto save_identity = identity(installed / "data" / "unknown-save.sav");
        const auto settings_identity = identity(installed / "unknown-settings.ini");
        auto saved_install = snapshot(installed);
        saved_install.erase(".pusu-native-owned");
        pusu::install_source(request);
        auto reinstalled = snapshot(installed);
        reinstalled.erase(".pusu-native-owned");
        assert(reinstalled == saved_install);
        assert(shader_times(installed / "data") == extracted_shader_times);
        assert(fs::last_write_time(installed / "data" / "unknown-save.sav") == save_time);
        assert(fs::last_write_time(installed / "unknown-settings.ini") == settings_time);
        assert(identity(installed / "data" / "unknown-save.sav") == save_identity);
        assert(identity(installed / "unknown-settings.ini") == settings_identity);
        std::fstream live_save(installed / "data" / "unknown-save.sav",
                               std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
        if (!live_save) throw std::runtime_error("cannot hold private save open");
        bool live_append = false;
        std::map<std::string, std::string> expected_live_install;
        pusu::install_source(request, [&](double fraction, std::string_view) {
            if (fraction != 0.98 || live_append) return;
            live_save << "appended through original open handle";
            live_save.flush();
            if (!live_save) throw std::runtime_error("cannot append private save");
            expected_live_install = snapshot(installed);
            expected_live_install.erase(".pusu-native-owned");
            live_append = true;
        });
        assert(live_append);
        auto live_install = snapshot(installed);
        live_install.erase(".pusu-native-owned");
        assert(live_install == expected_live_install);
        assert(identity(installed / "data" / "unknown-save.sav") == save_identity);
        assert(identity(installed / "unknown-settings.ini") == settings_identity);
        assert(fs::last_write_time(installed / "unknown-settings.ini") == settings_time);
        assert(shader_times(installed / "data") == extracted_shader_times);
        live_save.close();
        const auto copied_runtime = root / "extended-runtime";
        fs::copy(runtime, copied_runtime, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        fs::create_directories(copied_runtime / "share" / "pusu" / "new-assets");
        put(copied_runtime / "share" / "pusu" / "new-assets" / "actualfile", "genuine runtime extension");
        auto extended_request = request;
        extended_request.runtime_directory = copied_runtime;
        pusu::install_source(extended_request);
        assert(fs::is_regular_file(installed / "share" / "pusu" / "new-assets" / "actualfile"));
        assert(identity(installed / "data" / "unknown-save.sav") == save_identity);
        assert(identity(installed / "unknown-settings.ini") == settings_identity);
        const auto recovery_directory = [&](const std::string& message) {
            fs::path recovery;
            for (const auto& entry : fs::directory_iterator(root)) {
                if (!entry.is_directory() || !entry.path().filename().string().starts_with(".pusu-stage")) continue;
                assert(recovery.empty());
                recovery = entry.path();
            }
            assert(!recovery.empty());
            assert(message.find(recovery.string()) != std::string::npos);
            return recovery;
        };
        const auto before_checkpoint = snapshot(root);
        const auto checkpoint_install = snapshot(installed);
        const auto checkpoint_identity = identity(installed);
        bool checkpoint_written = false;
        std::string checkpoint_error;
        try {
            pusu::install_source(request, [&](double fraction, std::string_view) {
                assert(fs::is_directory(installed));
                if (fraction != 0.99 || checkpoint_written) return;
                assert(identity(installed) != checkpoint_identity);
                put(root / "replacement-save", "new live save bytes");
                fs::rename(root / "replacement-save", installed / "data" / "unknown-save.sav");
                put(installed / "data" / "new-checkpoint.sav", "new live checkpoint bytes");
                checkpoint_written = true;
            });
        } catch (const std::exception& error) {
            checkpoint_error = error.what();
        }
        assert(checkpoint_written);
        assert(!checkpoint_error.empty());
        assert(identity(installed) == checkpoint_identity);
        assert(snapshot(installed) == checkpoint_install);
        const auto checkpoint_recovery = recovery_directory(checkpoint_error);
        const auto recovered_files = snapshot(checkpoint_recovery / "data");
        const auto expected_recovery = root / "expected-recovery";
        fs::create_directory(expected_recovery);
        put(expected_recovery / "unknown-save.sav", "new live save bytes");
        put(expected_recovery / "new-checkpoint.sav", "new live checkpoint bytes");
        for (const auto& [name, fingerprint] : snapshot(expected_recovery))
            assert(recovered_files.at(name) == fingerprint);
        fs::remove_all(expected_recovery);
        fs::remove_all(checkpoint_recovery); // Only named recovery inside our private root.
        assert(snapshot(root) == before_checkpoint);
        const auto before_root_swap = snapshot(root);
        const auto prior_root_contents = snapshot(installed);
        const auto prior_root_identity = identity(installed);
        const auto swapped_new_root = root / "swapped-new-root";
        bool root_swapped = false;
        std::string root_swap_error;
        std::map<std::string, std::string> unrelated_root_contents;
        try {
            pusu::install_source(request, [&](double fraction, std::string_view) {
                if (fraction != 0.99 || root_swapped) return;
                assert(identity(installed) != prior_root_identity);
                put(installed / "data" / "swap-checkpoint.sav", "checkpoint in displaced new root");
                fs::rename(installed, swapped_new_root);
                fs::create_directory(installed);
                put(installed / "unrelated-sentinel", "unrelated concurrent root");
                unrelated_root_contents = snapshot(installed);
                root_swapped = true;
            });
        } catch (const std::exception& error) {
            root_swap_error = error.what();
        }
        assert(root_swapped);
        assert(!root_swap_error.empty());
        assert(snapshot(installed) == unrelated_root_contents);
        const auto old_root_recovery = recovery_directory(root_swap_error);
        assert(identity(old_root_recovery) == prior_root_identity);
        assert(snapshot(old_root_recovery) == prior_root_contents);
        std::ifstream checkpoint_stream(swapped_new_root / "data" / "swap-checkpoint.sav", std::ios::binary);
        if (!checkpoint_stream) throw std::runtime_error("missing displaced checkpoint");
        assert(std::string(std::istreambuf_iterator<char>(checkpoint_stream), {}) ==
               "checkpoint in displaced new root");
        checkpoint_stream.close();
        fs::remove_all(installed); // Only the unrelated root created by this callback.
        fs::rename(old_root_recovery, installed);
        fs::remove_all(swapped_new_root); // Only this callback's displaced staged tree.
        assert(snapshot(root) == before_root_swap);

        const auto reject_install = [&] {
            const auto before = snapshot(root);
            bool rejected = false;
            try {
                pusu::install_source(request);
            } catch (const std::exception&) {
                rejected = true;
            }
            assert(rejected);
            assert(snapshot(root) == before);
        };
        // A foreign desktop file must block publication, including reinstall.
        const auto desktop_file = root / "desktop" / "pusu-native.desktop";
        std::ifstream desktop_stream(desktop_file, std::ios::binary);
        if (!desktop_stream) throw std::runtime_error("missing installed desktop file");
        const std::string owned_desktop(std::istreambuf_iterator<char>(desktop_stream), {});
        desktop_stream.close();
        put(desktop_file, "[Desktop Entry]\nName=Unrelated application\n");
        reject_install();
        put(desktop_file, owned_desktop);
        // Inject a real filesystem failure after destination state is captured.
        // The progress observer alters only our private desktop fixture; the
        // installer's actual rename/publication path must roll itself back.
        const auto before_fault = snapshot(root);
        const auto prior_install = snapshot(installed);
        const auto prior_applications = snapshot(root / "applications");
        const auto stashed_desktop = root / "rollback-original.desktop";
        bool fault_injected = false;
        bool publication_rejected = false;
        std::string publication_error;
        try {
            pusu::install_source(request, [&](double fraction, std::string_view) {
                if (fraction != 0.99 || fault_injected) return;
                fs::rename(desktop_file, stashed_desktop);
                fs::create_directory(desktop_file);
                put(desktop_file / "block-rename", "private rollback fault");
                fault_injected = true;
            });
        } catch (const std::exception& error) {
            publication_error = error.what();
            publication_rejected = true;
        }
        assert(fault_injected);
        assert(publication_rejected);
        assert(snapshot(installed) == prior_install);
        assert(snapshot(root / "applications") == prior_applications);
        assert(fs::is_directory(desktop_file));
        fs::remove_all(desktop_file); // Only the directory created by this fault.
        fs::rename(stashed_desktop, desktop_file);
        const auto desktop_recovery = recovery_directory(publication_error);
        assert(fs::is_regular_file(desktop_recovery / "data" / "font" / "console.fnt"));
        fs::remove_all(desktop_recovery);
        assert(snapshot(root) == before_fault);
        // Altering an owned extracted asset must refuse reinstall, not overwrite it.
        put(installed / "data" / "font" / "console.fnt", "user-modified owned font\n");
        reject_install();
        if (argc == 4) {
            const auto genuine_msi = fs::absolute(argv[3]);
            for (const auto* reserved : {".pusu-native-owned", ".PUSU-NATIVE-OWNED"}) {
                const auto fixture = root / (std::string("reserved-msi-") + reserved);
                fs::create_directory(fixture);
                fs::copy_file(genuine_msi, fixture / "Pusu.msi");
                fs::copy_file(genuine_msi.parent_path() / "Data1.cab", fixture / "Data1.cab");
                reserve_msi_filename(fixture / "Pusu.msi", reserved);
                const auto before_reserved = snapshot(root);
                bool reserved_rejected = false;
                bool cabinet_decoding_started = false;
                try {
                    pusu::extract_source(fixture / "Pusu.msi", root / "reserved-unpublished",
                                         [&](double fraction, std::string_view) {
                        if (fraction > 0.35 && fraction < 0.9) cabinet_decoding_started = true;
                    });
                } catch (const std::exception&) {
                    reserved_rejected = true;
                }
                assert(reserved_rejected);
                assert(!cabinet_decoding_started);
                assert(snapshot(root) == before_reserved);
                assert(!fs::exists(root / "reserved-unpublished"));
            }
        }
    }
    std::cout << "Importer boundary checks passed\n";
}
