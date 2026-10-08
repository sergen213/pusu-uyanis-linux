#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

namespace pusu {

using ImportProgress = std::function<void(double fraction, std::string_view message)>;
struct InstallRequest {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path runtime_directory; // Native distribution: bin siblings, lib/, share/.
    std::filesystem::path applications_directory;
    std::filesystem::path desktop_directory; // Empty disables the optional desktop copy.
};

// Both operations publish only after complete validated extraction. Existing
// unrelated destinations are refused; owned reinstall must preserve user data.
// Settings and shortcut directories stay outside the owned root, including
// descendants whose names start with ".." without being the parent component.
void extract_source(const std::filesystem::path& source,
                    const std::filesystem::path& destination,
                    const ImportProgress& progress = {});
void install_source(const InstallRequest& request, const ImportProgress& progress = {});
std::filesystem::path executable_directory();

} // namespace pusu
