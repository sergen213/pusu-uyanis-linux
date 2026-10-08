#include "importer.hpp"
#include "settings.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <libmsi.h>
#include <libgcab.h>
#include <gio/gio.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <unistd.h>

namespace pusu {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t max_file = 2ULL << 30, max_total = 32ULL << 30;
constexpr std::string_view manifest_name = ".pusu-native-owned";
constexpr std::string_view manifest_header = "PUSU-NATIVE-INSTALL-1";
[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
std::string lower(std::string s) {
    if (std::any_of(s.begin(), s.end(), [](unsigned char c) { return c >= 128; })) {
        if (!g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr)) fail("Invalid UTF-8 path identity");
        std::unique_ptr<char, decltype(&g_free)> normalized(g_utf8_normalize(s.data(), s.size(), G_NORMALIZE_ALL_COMPOSE), g_free);
        if (!normalized) fail("Cannot normalize path identity");
        std::unique_ptr<char, decltype(&g_free)> folded(g_utf8_casefold(normalized.get(), -1), g_free);
        if (!folded) fail("Cannot case-fold path identity");
        return folded.get();
    }
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
void report(const ImportProgress& cb, double value, std::string_view message) {
    if (cb) cb(value, message);
}
fs::path relative_name(std::string s) {
    if (s.empty() || s.size() > 4096) fail("Invalid empty or oversized media path");
    if (!g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr)) fail("Invalid UTF-8 media path");
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.front() == '/' || s.back() == '/') fail("Absolute or empty media path component");
    fs::path result;
    std::size_t start = 0;
    while (start < s.size()) {
        auto end = s.find('/', start);
        auto part = s.substr(start, end == std::string::npos ? s.size() - start : end - start);
        if (part.empty() || part == "." || part == ".." || part.size() > 255 || part.back() == '.' || part.back() == ' ')
            fail("Unsafe media path: " + s);
        for (unsigned char c : part)
            if (c < 32 || c == 127 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                fail("Unsafe character in media path: " + s);
        result /= part;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
fs::path checked_absolute(const fs::path& input) {
    if (input.empty()) fail("An explicit path is required");
    auto p = fs::absolute(input).lexically_normal();
    fs::path current = p.root_path();
    for (const auto& part : p.relative_path()) {
        current /= part;
        const auto status = fs::symlink_status(current);
        if (fs::is_symlink(status)) fail("Symbolic links are not allowed: " + current.string());
        if (fs::exists(status) && !fs::is_regular_file(status) && !fs::is_directory(status))
            fail("Special files are not allowed: " + current.string());
    }
    return p;
}
void regular(const fs::path& p) {
    if (!fs::is_regular_file(fs::symlink_status(p)) || fs::hard_link_count(p) != 1)
        fail("Expected an unlinked regular file: " + p.string());
}
std::string read_text(const fs::path& p, std::uint64_t limit) {
    regular(p);
    auto size = fs::file_size(p);
    if (size > limit) fail("Text file exceeds limit: " + p.string());
    std::ifstream in(p, std::ios::binary);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!in.read(result.data(), static_cast<std::streamsize>(size))) fail("Cannot read " + p.string());
    if (result.find('\0') != std::string::npos) fail("NUL in text metadata");
    return result;
}
void write_text(const fs::path& p, const std::string& text, bool executable = false) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) fail("Cannot write " + p.string());
    fs::permissions(p, executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                      fs::perms::others_read | fs::perms::others_exec
                                  : fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
}
struct Temporary {
    fs::path path;
    explicit Temporary(const fs::path& parent) {
        std::string pattern = (parent / ".pusu-stage-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) fail("Cannot create same-volume staging directory: " + std::string(std::strerror(errno)));
        path = pattern;
    }
    ~Temporary() { if (!path.empty()) { std::error_code ec; fs::remove_all(path, ec); } }
    Temporary(const Temporary&) = delete;
};
struct Descriptor {
    int value{-1};
    ~Descriptor() { if (value >= 0) ::close(value); }
};
struct stat identity(const fs::path& path) {
    struct stat result{};
    if (::lstat(path.c_str(), &result)) fail("Cannot inspect publication identity: " + path.string());
    return result;
}
bool same_identity(const fs::path& path, const struct stat& expected) {
    struct stat current{};
    return ::lstat(path.c_str(), &current) == 0 && current.st_dev == expected.st_dev &&
           current.st_ino == expected.st_ino && (current.st_mode & S_IFMT) == (expected.st_mode & S_IFMT);
}
void atomic_rename(const fs::path& from, const fs::path& to, unsigned flags) {
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), flags))
        fail("Atomic no-replace/exchange publication failed (no unsafe fallback): " + std::string(std::strerror(errno)));
}
struct Destination {
    fs::path path;
    Descriptor lock;
    struct stat initial{};
    bool existed{};
    explicit Destination(const fs::path& requested) : path(checked_absolute(requested)) {
        if (path == path.root_path() || path == fs::path("/usr") || path == fs::path("/opt") || path == fs::path("/home") ||
            path == fs::path("/tmp") || path == fs::path("/var") || path == fs::path("/etc") ||
            path == fs::path("/bin") || path == fs::path("/lib") || path == fs::path("/srv")) fail("Unsafe installation root");
        if (const char* home = std::getenv("HOME"); home && path == fs::absolute(home).lexically_normal()) fail("Cannot replace the home directory");
        fs::create_directories(path.parent_path());
        lock.value = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        struct stat st{};
        // ponytail: serialize installs sharing a parent; use per-root locks if parallel installs matter.
        if (lock.value < 0 || ::fstat(lock.value, &st) || !S_ISDIR(st.st_mode) ||
            ::flock(lock.value, LOCK_EX | LOCK_NB)) fail("Installation parent is locked or unsafe");
        if (::lstat(path.c_str(), &initial) == 0) existed = true;
        else if (errno != ENOENT) fail("Cannot inspect installation root");
    }
    void validate() const {
        checked_absolute(path);
        struct stat current{};
        bool present = ::lstat(path.c_str(), &current) == 0;
        if ((!present && errno != ENOENT) || present != existed ||
            (present && (initial.st_dev != current.st_dev || initial.st_ino != current.st_ino)))
            fail("Installation destination changed during extraction; nothing was published");
    }
};
void copy_runtime_file(const fs::path& source, const fs::path& destination, bool executable = false) {
    regular(source);
    if (fs::exists(fs::symlink_status(destination))) fail("Refusing file collision: " + destination.string());
    fs::create_directories(destination.parent_path());
    fs::copy_file(source, destination);
    fs::last_write_time(destination, fs::last_write_time(source));
    fs::permissions(destination, executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                              fs::perms::others_read | fs::perms::others_exec
                                          : fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
}
void native_binary(const fs::path& source) {
    regular(source);
    std::ifstream in(source, std::ios::binary);
    std::array<unsigned char, 20> header{};
    if (!in.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::memcmp(header.data(), "\177ELF", 4) || header[4] != 2 || header[5] != 1 || header[6] != 1 ||
        (header[16] != 2 && header[16] != 3) || header[17] != 0)
        fail("Packaged game and launcher must be compiled native ELF executables");
#if defined(__x86_64__)
    if (header[18] != 62 || header[19] != 0) fail("Packaged executable architecture does not match x86-64");
#elif defined(__aarch64__)
    if (header[18] != 183 || header[19] != 0) fail("Packaged executable architecture does not match AArch64");
#endif
}
std::string digest(const fs::path& p) {
    regular(p);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) fail("SHA256 initialization failed");
    std::ifstream in(p, std::ios::binary);
    std::array<char, 65536> buffer{};
    while (in.read(buffer.data(), buffer.size()) || in.gcount())
        if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(in.gcount())) != 1) fail("SHA256 update failed");
    if (!in.eof()) fail("Cannot hash " + p.string());
    std::array<unsigned char, 32> hash{}; unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &length) != 1 || length != hash.size()) fail("SHA256 completion failed");
    std::ostringstream out;
    for (auto c : hash) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    return out.str();
}
using Owned = std::map<std::string, std::string>;
Owned previous_owned(const fs::path& destination) {
    if (!fs::exists(destination)) return {};
    if (!fs::is_directory(destination)) fail("Destination is not a directory");
    std::istringstream in(read_text(destination / manifest_name, 32 << 20));
    std::string line;
    if (!std::getline(in, line) || line != manifest_header) fail("Destination is not an owned Pusu installation");
    Owned owned;
    std::set<std::string> folded;
    while (std::getline(in, line)) {
        if (line.size() < 66 || line[64] != '\t') fail("Invalid ownership manifest");
        const auto hash = line.substr(0, 64), name = relative_name(line.substr(65)).generic_string();
        if (hash.find_first_not_of("0123456789abcdef") != std::string::npos || name == manifest_name ||
            !owned.emplace(name, hash).second || !folded.insert(lower(name)).second) fail("Invalid ownership manifest entry");
        const auto p = checked_absolute(destination / name);
        if (fs::exists(p) && digest(p) != hash) fail("Modified installed file must be preserved or moved before reinstall: " + name);
    }
    return owned;
}
Owned inventory(const fs::path& root) {
    Owned files;
    std::set<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        auto name = fs::relative(entry.path(), root).generic_string();
        relative_name(name);
        if (!names.insert(lower(name)).second) fail("Case collision in installation: " + name);
        if (entry.is_symlink() || (!entry.is_regular_file() && !entry.is_directory())) fail("Unsafe installation entry");
        if (entry.is_regular_file()) files.emplace(name, digest(entry.path()));
    }
    return files;
}
using Preserved = std::map<std::string, decltype(identity(fs::path{}))>;
Preserved preserve_unowned(const fs::path& old, const fs::path& stage, const Owned& owned) {
    Preserved preserved;
    if (!fs::exists(old)) return preserved;
    std::set<std::string> names;
    const auto device = identity(stage).st_dev;
    for (const auto& entry : fs::recursive_directory_iterator(old)) {
        const auto relative = entry.path().lexically_relative(old);
        const auto name = relative.generic_string();
        relative_name(name);
        const auto original = identity(entry.path());
        if (!names.insert(lower(name)).second || (!S_ISDIR(original.st_mode) && !S_ISREG(original.st_mode)))
            fail("Unsafe or case-colliding existing user data");
        if (original.st_dev != device) fail("Existing user data crosses a filesystem boundary");
        if (name == manifest_name || owned.contains(name)) continue;
        auto target = stage / relative;
        if (S_ISREG(original.st_mode)) {
            regular(entry.path());
            if (fs::exists(fs::symlink_status(target))) fail("Unowned user file collides with installed data");
            fs::create_directories(target.parent_path());
            fs::create_hard_link(entry.path(), target);
        } else if (fs::exists(target) && !fs::is_directory(target)) fail("User directory collides with installed file");
        else fs::create_directories(target);
        preserved.emplace(name, original);
    }
    names.clear();
    for (const auto& entry : fs::recursive_directory_iterator(stage))
        if (!names.insert(lower(entry.path().lexically_relative(stage).generic_string())).second)
            fail("Case collision between installed assets and preserved user data");
    return preserved;
}
void validate_unowned(const fs::path& old, const fs::path& stage, const Owned& old_owned, const Preserved& preserved) {
    const auto device = identity(stage).st_dev;
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(old)) {
        const auto relative = entry.path().lexically_relative(old);
        const auto name = relative.generic_string();
        if (name == manifest_name || old_owned.contains(name)) continue;
        auto it = preserved.find(name);
        if (it == preserved.end() || !same_identity(entry.path(), it->second))
            fail("Unowned user entry changed during publication: " + name);
        const auto original = identity(entry.path());
        if (original.st_dev != device || (!S_ISDIR(original.st_mode) && !S_ISREG(original.st_mode)))
            fail("Unsafe or cross-volume user data during publication");
        if (S_ISREG(original.st_mode)) {
            if (!same_identity(stage / relative, original) || original.st_nlink != 2)
                fail("Unowned user file changed during publication; reinstall refused: " + name);
        } else if (!fs::is_directory(fs::symlink_status(stage / relative)))
            fail("Preserved user directory changed during publication: " + name);
        ++count;
    }
    if (count != preserved.size()) fail("Preserved user entries disappeared during publication");
}
void seal(const fs::path& root, const Owned& files) {
    std::string text(manifest_header); text += '\n';
    for (const auto& [name, hash] : files) text += hash + '\t' + name + '\n';
    write_text(root / manifest_name, text);
}
std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
std::uint32_t both32(const unsigned char* p) {
    auto a = le32(p), b = std::uint32_t(p[4]) << 24 | std::uint32_t(p[5]) << 16 | std::uint32_t(p[6]) << 8 | p[7];
    if (a != b) fail("Inconsistent ISO dual-endian field");
    return a;
}
unsigned both16(const unsigned char* p) {
    unsigned a = unsigned(p[0]) | unsigned(p[1]) << 8, b = unsigned(p[2]) << 8 | p[3];
    if (a != b) fail("Inconsistent ISO dual-endian field");
    return a;
}
void validate_iso(const fs::path& p) {
    regular(p);
    auto size = fs::file_size(p);
    if (size < 18 * 2048 || size > max_file || size % 2048) fail("Invalid ISO volume size");
    std::ifstream in(p, std::ios::binary);
    std::array<unsigned char, 2048> sector{};
    bool primary = false, terminated = false;
    for (unsigned n = 16; n < 80 && std::uint64_t(n + 1) * 2048 <= size; ++n) {
        in.seekg(std::uint64_t(n) * 2048);
        if (!in.read(reinterpret_cast<char*>(sector.data()), sector.size()) || std::memcmp(sector.data() + 1, "CD001", 5) || sector[6] != 1)
            fail("Malformed ISO volume descriptor");
        if (sector[0] == 255) { terminated = true; break; }
        if (sector[0] != 0 && sector[0] != 1 && sector[0] != 2 && sector[0] != 3) fail("Unknown ISO descriptor type");
        if (sector[0] == 1) {
            if (primary) fail("Multiple ISO primary volumes");
            primary = true;
            auto blocks = both32(sector.data() + 80);
            if (std::uint64_t(blocks) * 2048 != size || both16(sector.data() + 120) != 1 ||
                both16(sector.data() + 124) != 1 || both16(sector.data() + 128) != 2048 || sector[156] < 34)
                fail("Unsupported or inconsistent ISO volume geometry");
            const auto extent = both32(sector.data() + 158), length = both32(sector.data() + 166);
            if (extent >= blocks || std::uint64_t(extent) * 2048 + length > size || !(sector[181] & 2)) fail("Invalid ISO root directory");
        }
    }
    if (!primary || !terminated) fail("Missing ISO primary volume or terminator");
}
fs::path cue_binary(const fs::path& cue) {
    std::istringstream in(read_text(cue, 65536));
    std::string line, file; unsigned state = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line.erase(0, first);
        if (line.starts_with("REM ")) continue;
        if (state == 0 && line.starts_with("FILE \"")) {
            auto quote = line.find('"', 6);
            if (quote == std::string::npos || line.substr(quote) != "\" BINARY") fail("Only a quoted BINARY CUE file is supported");
            auto name = relative_name(line.substr(6, quote - 6));
            if (name.has_parent_path()) fail("CUE media must be a sibling file");
            file = name.string(); ++state;
        } else if (state == 1 && line == "TRACK 01 MODE1/2352") ++state;
        else if (state == 2 && line == "INDEX 01 00:00:00") ++state;
        else fail("Only one zero-offset MODE1/2352 CUE track is supported");
    }
    if (state != 3) fail("Incomplete MODE1/2352 CUE metadata");
    return checked_absolute(cue.parent_path() / file);
}
unsigned bcd(unsigned c) {
    if ((c & 15) > 9 || (c >> 4) > 9) fail("Invalid BCD sector address");
    return (c >> 4) * 10 + (c & 15);
}
void validate_ecc(const std::array<unsigned char, 2352>& sector,
                  const std::array<unsigned char, 256>& forward,
                  const std::array<unsigned char, 256>& backward,
                  unsigned major_count, unsigned minor_count, unsigned major_mult,
                  unsigned minor_inc, unsigned offset) {
    const unsigned size = major_count * minor_count;
    for (unsigned major = 0; major < major_count; ++major) {
        unsigned index = (major >> 1) * major_mult + (major & 1);
        unsigned char a = 0, b = 0;
        for (unsigned minor = 0; minor < minor_count; ++minor) {
            const auto value = sector[12 + index];
            index += minor_inc;
            if (index >= size) index -= size;
            a ^= value; b ^= value; a = forward[a];
        }
        a = backward[forward[a] ^ b];
        if (sector[offset + major] != a || sector[offset + major_count + major] != (a ^ b))
            fail("Invalid MODE1 error-correction parity");
    }
}
void raw_iso(const fs::path& bin, const fs::path& iso, const ImportProgress& cb) {
    regular(bin);
    auto size = fs::file_size(bin);
    if (!size || size % 2352 || size / 2352 > 450000) fail("Invalid MODE1/2352 image length");
    std::ifstream in(bin, std::ios::binary); std::ofstream out(iso, std::ios::binary);
    std::array<unsigned char, 2352> sector{};
    std::array<std::uint32_t, 256> edc{};
    for (unsigned i = 0; i < 256; ++i) {
        auto v = std::uint32_t(i);
        for (unsigned j = 0; j < 8; ++j) v = (v >> 1) ^ ((v & 1) ? 0xd8018001U : 0);
        edc[i] = v;
    }
    std::array<unsigned char, 256> forward{}, backward{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned j = (i << 1) ^ ((i & 128) ? 0x11d : 0);
        forward[i] = static_cast<unsigned char>(j);
        backward[i ^ j] = static_cast<unsigned char>(i);
    }
    for (std::uint64_t n = 0; n < size / 2352; ++n) {
        if (!in.read(reinterpret_cast<char*>(sector.data()), sector.size())) fail("Truncated raw sector");
        if (sector[0] || sector[11] || !std::all_of(sector.begin() + 1, sector.begin() + 11, [](auto c) { return c == 255; }) || sector[15] != 1)
            fail("Malformed MODE1 sector sync or mode");
        auto minute = bcd(sector[12]), second = bcd(sector[13]), frame = bcd(sector[14]);
        if (second >= 60 || frame >= 75 || (minute * 60ULL + second) * 75 + frame != n + 150)
            fail("Non-contiguous MODE1 sector address");
        std::uint32_t crc = 0;
        for (unsigned j = 0; j < 2064; ++j) crc = (crc >> 8) ^ edc[(crc ^ sector[j]) & 255];
        if (crc != le32(sector.data() + 2064) || !std::all_of(sector.begin() + 2068, sector.begin() + 2076, [](auto c) { return c == 0; }))
            fail("MODE1 sector checksum or reserved bytes invalid");
        validate_ecc(sector, forward, backward, 86, 24, 2, 86, 2076);
        validate_ecc(sector, forward, backward, 52, 43, 86, 88, 2248);
        out.write(reinterpret_cast<char*>(sector.data() + 16), 2048);
        if (!out) fail("Cannot write temporary ISO");
        if (!(n % 4096)) report(cb, 0.2 * double(n) / double(size / 2352), "Validating and decoding CD sectors");
    }
    out.close(); if (!out) fail("Cannot finish temporary ISO");
    validate_iso(iso);
}
struct ArchiveDeleter { void operator()(archive* a) const { archive_read_free(a); } };
using Archive = std::unique_ptr<archive, ArchiveDeleter>;
Archive open_iso_archive(const fs::path& p) {
    regular(p);
    if (fs::file_size(p) > max_file) fail("Native archive exceeds size bound");
    Archive a(archive_read_new());
    if (!a) fail("Cannot allocate archive reader");
    int r = archive_read_support_format_iso9660(a.get());
    if (r != ARCHIVE_OK || archive_read_support_filter_none(a.get()) != ARCHIVE_OK ||
        archive_read_open_filename(a.get(), p.c_str(), 65536) != ARCHIVE_OK)
        fail("Cannot open native archive: " + std::string(archive_error_string(a.get()) ? archive_error_string(a.get()) : "invalid format"));
    return a;
}
struct ArchiveNames {
    std::map<std::string, std::string> seen;
    void add(const fs::path& path, bool directory) {
        fs::path current;
        for (const auto& component : path) {
            current /= component;
            const auto name = current.generic_string(), key = lower(name);
            const bool final = current == path;
            const auto value = name + ((final && !directory) ? "|f" : "|d");
            auto [it, inserted] = seen.emplace(key, value);
            if (!inserted && (it->second != value || (final && !directory))) fail("Duplicate or case/type collision in archive: " + name);
        }
    }
};
fs::path archive_name(archive_entry* entry, ArchiveNames& names) {
    const char* raw = archive_entry_pathname_utf8(entry);
    if (!raw) raw = archive_entry_pathname(entry);
    if (!raw || archive_entry_symlink(entry) || archive_entry_hardlink(entry)) fail("Missing archive path or forbidden archive link");
    auto type = archive_entry_filetype(entry);
    if (type != AE_IFDIR && type != AE_IFREG) fail("Special archive entries are forbidden");
    std::string text(raw);
    while (!text.empty() && text.back() == '/' && type == AE_IFDIR) text.pop_back();
    if ((text.empty() || text == ".") && type == AE_IFDIR) return {};
    auto path = relative_name(text); names.add(path, type == AE_IFDIR);
    return path;
}
void set_original_timestamp(const fs::path& output, std::int64_t seconds, long nanos) {
    if (seconds < 315532800LL || seconds >= 4354819200LL || nanos < 0 || nanos >= 1000000000)
        fail("Malformed original archive modification timestamp");
    Descriptor fd;
    fd.value = ::open(output.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat status{};
    if (fd.value < 0 || ::fstat(fd.value, &status) || !S_ISREG(status.st_mode) || status.st_nlink != 1)
        fail("Unsafe staged asset timestamp target");
    const timespec times[2]{{static_cast<time_t>(seconds), nanos}, {static_cast<time_t>(seconds), nanos}};
    if (::futimens(fd.value, times)) fail("Cannot retain original archive modification timestamp");
}
void archive_file(archive* a, archive_entry* entry, const fs::path& output, std::uint64_t expected) {
    if (!archive_entry_size_is_set(entry) || archive_entry_size(entry) < 0 || std::uint64_t(archive_entry_size(entry)) != expected || expected > max_file)
        fail("Archive file size does not match metadata");
    fs::create_directories(output.parent_path());
    std::ofstream out(output, std::ios::binary);
    std::array<char, 65536> buffer{}; std::uint64_t total = 0;
    for (;;) {
        auto n = archive_read_data(a, buffer.data(), buffer.size());
        if (n < 0) {
            const char* error = archive_error_string(a);
            const char* name = archive_entry_pathname(entry);
            fail("Archive data decode failed: " + std::string(error ? error : "unknown archive error", ::strnlen(error ? error : "unknown archive error", 1024)) +
                 "; entry: " + std::string(name ? name : "(unnamed)", ::strnlen(name ? name : "(unnamed)", 1024)) +
                 "; destination: " + output.string().substr(0, 1024));
        }
        if (!n) break;
        if (std::uint64_t(n) > expected - total) fail("Archive file exceeds declared size");
        total += std::uint64_t(n); out.write(buffer.data(), n);
        if (!out) fail("Cannot write extracted file");
    }
    out.close();
    if (!out || total != expected) fail("Truncated archive file");
    if (!archive_entry_mtime_is_set(entry)) fail("Missing original ISO modification timestamp");
    set_original_timestamp(output, archive_entry_mtime(entry), archive_entry_mtime_nsec(entry));
}
fs::path iso_media(const fs::path& iso, const fs::path& scratch) {
    validate_iso(iso);
    auto a = open_iso_archive(iso); archive_entry* entry = nullptr;
    ArchiveNames names; fs::path msi; unsigned count = 0; std::uint64_t total = 0;
    int status;
    while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        if (++count > 100000) fail("Too many ISO entries");
        auto name = archive_name(entry, names);
        if (archive_entry_filetype(entry) == AE_IFDIR) continue;
        auto length = archive_entry_size(entry);
        if (length < 0 || std::uint64_t(length) > max_file || std::uint64_t(length) > max_total - total) fail("ISO declared size exceeds limit");
        total += std::uint64_t(length);
        auto ext = lower(name.extension().string());
        if (ext == ".msi" || ext == ".cab") {
            auto output = scratch / "media" / name;
            archive_file(a.get(), entry, output, std::uint64_t(length));
            if (ext == ".msi") { if (!msi.empty()) fail("Multiple MSI packages in source volume"); msi = output; }
        } else if (archive_read_data_skip(a.get()) != ARCHIVE_OK) fail("Invalid skipped ISO entry");
    }
    if (status != ARCHIVE_EOF || msi.empty()) fail("Invalid ISO or missing MSI package");
    return msi;
}
struct GObjectDeleter { template<class T> void operator()(T* object) const { if (object) g_object_unref(object); } };
template<class T> using GPointer = std::unique_ptr<T, GObjectDeleter>;
void msi_error(GError*& error, std::string_view operation) {
    if (error) { std::string message = std::string(operation) + ": " + error->message; g_error_free(error); error = nullptr; fail(message); }
}
std::string field(LibmsiRecord* record, unsigned n) {
    std::unique_ptr<char, decltype(&g_free)> text(libmsi_record_get_string(record, n), g_free);
    return text ? text.get() : "";
}
template<class F> void rows(LibmsiDatabase* db, const char* sql, F&& fn) {
    GError* error = nullptr;
    GPointer<LibmsiQuery> query(libmsi_query_new(db, sql, &error)); msi_error(error, "MSI query");
    if (!query || !libmsi_query_execute(query.get(), nullptr, &error)) { msi_error(error, "MSI execute"); fail("MSI query execution failed"); }
    unsigned count = 0;
    for (;;) {
        GPointer<LibmsiRecord> record(libmsi_query_fetch(query.get(), &error)); msi_error(error, "MSI row");
        if (!record) break;
        if (++count > 100000) fail("MSI table exceeds row bound");
        fn(record.get());
    }
    if (!libmsi_query_close(query.get(), &error)) { msi_error(error, "MSI close"); fail("MSI query close failed"); }
}
std::string long_name(std::string name, bool directory) {
    if (directory) name = name.substr(0, name.find(':'));
    auto pipe = name.find('|');
    if (pipe != std::string::npos) name.erase(0, pipe + 1);
    if (name != ".") { auto p = relative_name(name); if (p.has_parent_path()) fail("MSI name is not one component"); }
    return name;
}
fs::path sibling_case(const fs::path& directory, const std::string& name) {
    relative_name(name);
    if (fs::path(name).has_parent_path()) fail("CAB path must be a sibling name");
    fs::path found;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (lower(entry.path().filename().string()) == lower(name)) {
            if (!found.empty()) fail("Case-colliding cabinet names");
            found = checked_absolute(entry.path());
        }
    }
    if (found.empty()) fail("Required MSI cabinet is absent: " + name);
    regular(found); return found;
}
bool windows_payload(const fs::path& p, const fs::path& logical) {
    static const std::set<std::string> extensions{".exe", ".dll", ".sys", ".ocx", ".drv", ".com", ".bat", ".cmd", ".msi", ".cab", ".scr", ".cpl", ".vbs", ".ps1"};
    if (extensions.contains(lower(logical.extension().string()))) return true;
    std::ifstream in(p, std::ios::binary); std::array<unsigned char, 64> header{};
    if (!in.read(reinterpret_cast<char*>(header.data()), header.size()) || header[0] != 'M' || header[1] != 'Z') return false;
    const auto offset = le32(header.data() + 60);
    if (offset > fs::file_size(p) - 4) return false;
    std::array<char, 4> signature{}; in.seekg(offset);
    return bool(in.read(signature.data(), signature.size())) && std::memcmp(signature.data(), "PE\0\0", 4) == 0;
}
void extract_msi(const fs::path& msi, const fs::path& scratch, const fs::path& output, const ImportProgress& cb) {
    regular(msi);
    if (fs::file_size(msi) > (512ULL << 20)) fail("MSI package exceeds size bound");
    GError* error = nullptr;
    GPointer<LibmsiDatabase> db(libmsi_database_new(msi.c_str(), LIBMSI_DB_FLAGS_READONLY, nullptr, &error));
    msi_error(error, "Opening MSI database"); if (!db) fail("Cannot open MSI database");
    struct Directory { std::string parent, name; };
    std::map<std::string, Directory> directories;
    rows(db.get(), "SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`", [&](auto r) {
        auto id = field(r, 1);
        if (id.empty() || !directories.emplace(id, Directory{field(r, 2), long_name(field(r, 3), true)}).second) fail("Duplicate or empty MSI Directory key");
    });
    std::map<std::string, fs::path> resolved; std::set<std::string> resolving;
    std::function<fs::path(const std::string&)> resolve = [&](const std::string& id) -> fs::path {
        if (auto it = resolved.find(id); it != resolved.end()) return it->second;
        auto it = directories.find(id);
        if (it == directories.end() || resolving.size() > 256 || !resolving.insert(id).second) fail("Missing or cyclic MSI Directory mapping");
        fs::path path;
        if (!it->second.parent.empty()) path = resolve(it->second.parent);
        else if (id != "TARGETDIR") fail("Unexpected MSI directory root");
        if (id != "TARGETDIR" && it->second.name != ".") path /= it->second.name;
        resolving.erase(id); resolved.emplace(id, path); return path;
    };
    for (const auto& [id, directory] : directories) { (void)directory; resolve(id); }
    if (!directories.contains("INSTALLDIR")) fail("MSI has no Pusu installation directory");
    const auto root = resolve("INSTALLDIR");
    if (lower(root.filename().string()) != "pusu") fail("MSI installation directory is not Pusu");
    std::map<std::string, std::string> components;
    rows(db.get(), "SELECT `Component`, `Directory_` FROM `Component`", [&](auto r) {
        auto id = field(r, 1), dir = field(r, 2);
        if (id.empty() || !directories.contains(dir) || !components.emplace(id, dir).second) fail("Invalid MSI Component mapping");
    });
    struct File { fs::path path; std::uint64_t size; int sequence; bool seen{}; };
    std::map<std::string, File> files; std::set<int> sequences; ArchiveNames names; std::uint64_t total = 0;
    rows(db.get(), "SELECT `File`, `Component_`, `FileName`, `FileSize`, `Sequence`, `Attributes` FROM `File`", [&](auto r) {
        auto id = field(r, 1), component = field(r, 2), name = long_name(field(r, 3), false);
        auto size = libmsi_record_get_int(r, 4), sequence = libmsi_record_get_int(r, 5), attributes = libmsi_record_get_int(r, 6);
        if (id.empty() || name == "." || !components.contains(component) || size < 0 || sequence <= 0 ||
            !sequences.insert(sequence).second || (attributes != int(LIBMSI_NULL_INT) && (attributes & 8192))) fail("Invalid or uncompressed MSI File mapping");
        auto full = resolve(components.at(component)) / name;
        auto relative = full.lexically_relative(root);
        relative = relative_name(relative.generic_string());
        if (lower(relative.begin()->string()) == manifest_name)
            fail("MSI asset collides with the reserved native ownership manifest");
        names.add(relative, false);
        if (std::uint64_t(size) > max_file || std::uint64_t(size) > max_total - total) fail("MSI file sizes exceed bound");
        total += size;
        if (!files.emplace(id, File{relative, std::uint64_t(size), sequence}).second) fail("Duplicate MSI File key");
    });
    if (files.empty()) fail("MSI has no asset files");
    struct Media { int disk, last; std::string cabinet; };
    std::vector<Media> media;
    rows(db.get(), "SELECT `DiskId`, `LastSequence`, `Cabinet` FROM `Media`", [&](auto r) {
        media.push_back({libmsi_record_get_int(r, 1), libmsi_record_get_int(r, 2), field(r, 3)});
    });
    std::sort(media.begin(), media.end(), [](auto& a, auto& b) { return a.disk < b.disk; });
    int previous = 0, disk = 0; unsigned completed = 0;
    for (const auto& item : media) {
        if (item.disk != ++disk || item.last <= previous || item.cabinet.empty()) fail("Malformed MSI media sequencing");
        fs::path cabinet;
        if (item.cabinet.front() == '#') {
            auto stream_name = item.cabinet.substr(1); relative_name(stream_name);
            cabinet = scratch / ("embedded-" + std::to_string(disk) + ".cab");
            bool found = false;
            rows(db.get(), "SELECT `Name`, `Data` FROM `_Streams`", [&](auto r) {
                if (field(r, 1) != stream_name) return;
                if (found) fail("Duplicate embedded cabinet stream"); found = true;
                GPointer<GInputStream> stream(libmsi_record_get_stream(r, 2));
                if (!stream) fail("Missing embedded cabinet data");
                std::ofstream out(cabinet, std::ios::binary); std::array<char, 65536> buffer{}; std::uint64_t bytes = 0;
                for (;;) {
                    auto n = g_input_stream_read(stream.get(), buffer.data(), buffer.size(), nullptr, &error); msi_error(error, "Embedded CAB read");
                    if (n < 0) fail("Embedded cabinet read failed"); if (!n) break;
                    if (std::uint64_t(n) > max_file - bytes) fail("Embedded cabinet exceeds bound");
                    bytes += n; out.write(buffer.data(), n); if (!out) fail("Cannot write embedded cabinet");
                }
                out.close(); if (!out) fail("Cannot finish embedded cabinet");
            });
            if (!found) fail("Embedded MSI cabinet is absent");
        } else cabinet = sibling_case(msi.parent_path(), item.cabinet);
        regular(cabinet);
        if (fs::file_size(cabinet) > max_file) fail("Native cabinet exceeds size bound");
        GPointer<GFile> cabinet_file(g_file_new_for_path(cabinet.c_str()));
        GPointer<GFileInputStream> input(g_file_read(cabinet_file.get(), nullptr, &error));
        msi_error(error, "Opening native cabinet"); if (!input) fail("Cannot open native cabinet input");
        GPointer<GCabCabinet> cab(gcab_cabinet_new());
        if (!cab) fail("Cannot allocate native cabinet reader");
        gcab_cabinet_add_allowed_compression(cab.get(), GCAB_COMPRESSION_NONE);
        gcab_cabinet_add_allowed_compression(cab.get(), GCAB_COMPRESSION_MSZIP);
        gcab_cabinet_add_allowed_compression(cab.get(), GCAB_COMPRESSION_LZX);
        if (!gcab_cabinet_load(cab.get(), G_INPUT_STREAM(input.get()), nullptr, &error)) {
            msi_error(error, "Loading native cabinet"); fail("Native cabinet load failed");
        }
        if (gcab_cabinet_get_size(cab.get()) != fs::file_size(cabinet)) fail("Cabinet header length differs from physical size");
        struct Member {
            GCabFile* file;
            File* mapping;
            std::string extraction_name;
            std::int64_t seconds;
            long nanos;
            bool callback_seen{};
        };
        std::vector<Member> members;
        std::map<GCabFile*, std::size_t> indices;
        ArchiveNames cabinet_names;
        std::uint64_t cabinet_total = 0;
        auto* folders = gcab_cabinet_get_folders(cab.get());
        if (!folders || !folders->len || folders->len > files.size()) fail("Invalid cabinet folder count");
        for (guint i = 0; i < folders->len; ++i) {
            auto* folder = GCAB_FOLDER(g_ptr_array_index(folders, i));
            auto compression = gcab_folder_get_comptype(folder) & GCAB_COMPRESSION_MASK;
            if (compression != GCAB_COMPRESSION_NONE && compression != GCAB_COMPRESSION_MSZIP && compression != GCAB_COMPRESSION_LZX)
                fail("Unsupported native cabinet compression");
            std::unique_ptr<GSList, decltype(&g_slist_free)> list(gcab_folder_get_files(folder), g_slist_free);
            guint count = 0;
            for (GSList* node = list.get(); node; node = node->next) {
                if (++count > files.size() || members.size() >= files.size()) fail("Cabinet member count exceeds MSI table");
                auto* member = GCAB_FILE(node->data);
                const auto* raw_name = gcab_file_get_name(member);
                if (!raw_name) fail("Missing cabinet member identity");
                auto name = relative_name(raw_name);
                if (name.has_parent_path()) fail("Cabinet member identities must be flat");
                cabinet_names.add(name, false);
                auto it = files.find(name.string());
                if (it == files.end() || it->second.seen || it->second.sequence <= previous || it->second.sequence > item.last)
                    fail("Cabinet member does not match MSI media/File mapping");
                const auto length = gcab_file_get_size(member), attributes = gcab_file_get_attributes(member);
                // FILE_ATTRIBUTE_NOT_CONTENT_INDEXED is only a Windows indexing hint.
                // https://learn.microsoft.com/en-us/windows/win32/fileio/file-attribute-constants
                constexpr guint32 not_content_indexed = 0x2000U;
                constexpr guint32 allowed_attributes = GCAB_FILE_ATTRIBUTE_RDONLY | GCAB_FILE_ATTRIBUTE_HIDDEN |
                    GCAB_FILE_ATTRIBUTE_SYSTEM | GCAB_FILE_ATTRIBUTE_ARCH | GCAB_FILE_ATTRIBUTE_EXEC |
                    GCAB_FILE_ATTRIBUTE_NAME_IS_UTF | not_content_indexed;
                auto context = [&] {
                    std::ostringstream text;
                    text << "cabinet=" << cabinet.string() << "; member=" << raw_name
                         << "; mapped=" << it->second.path.generic_string()
                         << "; expected_size=" << it->second.size << "; actual_size=" << length
                         << "; attributes=0x" << std::hex << attributes
                         << "; allowed_attributes=0x" << allowed_attributes
                         << "; unknown_attributes=0x" << (attributes & ~allowed_attributes) << std::dec
                         << "; previous_total=" << cabinet_total << "; prospective_total=" << (cabinet_total + length)
                         << "; maximum_total=" << max_total;
                    return text.str();
                };
                if (std::uint64_t(length) != it->second.size) fail("Cabinet member size mismatch: " + context());
                if (attributes & ~allowed_attributes) fail("Cabinet member unsafe/unknown attributes: " + context());
                if (std::uint64_t(length) > max_total - cabinet_total) fail("Cabinet uncompressed total exceeds bound: " + context());
                cabinet_total += length;
                std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(gcab_file_get_date_time(member), g_date_time_unref);
                if (!date || g_date_time_get_year(date.get()) < 1980 || g_date_time_get_year(date.get()) > 2107)
                    fail("Missing or invalid original cabinet date");
                auto seconds = g_date_time_to_unix(date.get());
                auto nanos = long(g_date_time_get_microsecond(date.get())) * 1000L;
                if (seconds < 315532800LL || seconds >= 4354819200LL || nanos < 0 || nanos >= 1000000000)
                    fail("Original cabinet timestamp outside valid bounds");
                auto extraction_name = "member-" + std::to_string(members.size());
                if (!indices.emplace(member, members.size()).second) fail("Duplicate native cabinet member object");
                members.push_back({member, &it->second, extraction_name, seconds, nanos, false});
                gcab_file_set_extract_name(member, extraction_name.c_str());
                gcab_file_set_attributes(member, GCAB_FILE_ATTRIBUTE_ARCH); // Never honor run-after-extraction or Windows modes.
            }
            if (count != gcab_folder_get_nfiles(folder)) fail("Cabinet folder file count mismatch");
        }
        const auto expected = std::distance(sequences.upper_bound(previous), sequences.upper_bound(item.last));
        if (members.empty() || members.size() != static_cast<std::size_t>(expected))
            fail("Cabinet member count does not cover its complete MSI media range");
        Temporary extracted(scratch);
        GPointer<GFile> directory(g_file_new_for_path(extracted.path.c_str()));
        GPointer<GCancellable> cancellable(g_cancellable_new());
        if (!cancellable) fail("Cannot allocate native cabinet cancellation state");
        struct Callbacks {
            const ImportProgress& progress;
            GCancellable* cancellable;
            std::vector<Member>& members;
            const std::map<GCabFile*, std::size_t>& indices;
            std::size_t baseline, total, visited{};
            std::exception_ptr failure;
        } callbacks{cb, cancellable.get(), members, indices, completed, files.size(), 0, {}};
        auto callback = [](GCabFile* file, gpointer data) noexcept -> gboolean {
            auto& state = *static_cast<Callbacks*>(data);
            if (state.failure) return FALSE;
            try {
                auto it = state.indices.find(file);
                if (it == state.indices.end() || state.members[it->second].callback_seen)
                    fail("Unexpected or duplicate native cabinet extraction callback");
                auto& member = state.members[it->second];
                const auto* extraction_name = gcab_file_get_extract_name(file);
                if (!extraction_name || member.extraction_name != extraction_name) fail("Cabinet extraction name changed after validation");
                member.callback_seen = true;
                if (!(++state.visited % 64))
                    report(state.progress, 0.35 + 0.55 * double(state.baseline + state.visited) / double(state.total), "Extracting original Pusu assets");
                return TRUE;
            } catch (...) {
                state.failure = std::current_exception();
                g_cancellable_cancel(state.cancellable);
                return FALSE;
            }
        };
        const auto success = gcab_cabinet_extract(cab.get(), directory.get(), callback, nullptr, &callbacks, cancellable.get(), &error);
        if (callbacks.failure) {
            if (error) { g_error_free(error); error = nullptr; }
            std::rethrow_exception(callbacks.failure);
        }
        msi_error(error, "Native cabinet decompression");
        if (!success || callbacks.visited != members.size()) fail("Native cabinet extraction incomplete");
        std::size_t extracted_count = 0;
        for (const auto& entry : fs::directory_iterator(extracted.path)) {
            regular(entry.path());
            if (++extracted_count > members.size() || !entry.is_regular_file()) fail("Unexpected native cabinet output");
        }
        if (extracted_count != members.size()) fail("Native cabinet output count mismatch");
        for (auto& member : members) {
            auto temporary = extracted.path / member.extraction_name;
            regular(temporary);
            if (!member.callback_seen || fs::file_size(temporary) != member.mapping->size)
                fail("Native cabinet output does not match validated MSI size");
            set_original_timestamp(temporary, member.seconds, member.nanos);
            fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
            if (!windows_payload(temporary, member.mapping->path)) {
                auto target = output / member.mapping->path;
                if (fs::exists(target)) fail("Duplicate mapped asset destination");
                fs::create_directories(target.parent_path());
                fs::rename(temporary, target);
            }
            member.mapping->seen = true;
            ++completed;
        }
        previous = item.last;
    }
    for (const auto& [id, file] : files) if (!file.seen) fail("MSI File entry absent from cabinets: " + id);
    if (previous != *sequences.rbegin()) fail("MSI Media last sequence does not match File table");
    rows(db.get(), "SELECT `Dialog_`, `Control`, `Type`, `Text` FROM `Control`", [&](auto r) {
        if (field(r, 3) != "ScrollableText") return;
        auto text = field(r, 4);
        if (text.empty()) return;
        if (text.size() > (4U << 20)) fail("Original installer notice text exceeds bound");
        const auto name = relative_name("original-" + field(r, 1) + "-" + field(r, 2) + ".rtf");
        if (name.has_parent_path()) fail("Invalid original notice control identity");
        const auto target = output / ".pusu-media-notices" / name;
        if (fs::exists(target)) fail("Original installer notice documentation collision");
        write_text(target, text); // Preserve original notice/template bytes; never accept installer actions.
    });
    // These are content roots used by the native resource loader, not optional installer files.
    for (const auto* required : {"font", "galeri", "interface", "level", "material", "object", "particle", "script", "scshot", "sound", "textures"}) {
        bool found = false;
        for (const auto& entry : fs::directory_iterator(output)) if (lower(entry.path().filename().string()) == required && entry.is_directory()) found = true;
        if (!found) fail(std::string("Required Pusu asset directory missing: ") + required);
    }
}
void extract_into(const fs::path& requested, const fs::path& output, const fs::path& scratch, const ImportProgress& cb) {
    auto source = checked_absolute(requested); regular(source);
    fs::create_directories(output);
    auto ext = lower(source.extension().string()); fs::path msi;
    if (ext == ".cue" || ext == ".bin") {
        auto iso = scratch / "disc.iso";
        raw_iso(ext == ".cue" ? cue_binary(source) : source, iso, cb);
        msi = iso_media(iso, scratch);
    } else if (ext == ".iso") msi = iso_media(source, scratch);
    else if (ext == ".msi") msi = source;
    else fail("Supported sources are CUE, MODE1/2352 BIN, ISO, and MSI");
    report(cb, 0.35, "Reading native MSI asset mapping");
    extract_msi(msi, scratch, output, cb);
}
void copy_tree(const fs::path& source, const fs::path& destination) {
    if (!fs::is_directory(fs::symlink_status(source))) fail("Missing native runtime directory: " + source.string());
    ArchiveNames names;
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        auto relative = fs::relative(entry.path(), source);
        relative_name(relative.generic_string()); names.add(relative, entry.is_directory());
        if (entry.is_symlink() || (!entry.is_regular_file() && !entry.is_directory())) fail("Unsafe native runtime entry");
        if (entry.is_directory()) fs::create_directories(destination / relative);
        else copy_runtime_file(entry.path(), destination / relative);
    }
}
std::string desktop_quote(const fs::path& path) {
    std::string out = "\"";
    for (unsigned char c : path.string()) {
        if (c < 32 || c == 127) fail("Control character in desktop executable path");
        if (c == '%') out += "%%";
        else if (c == '\\') out += "\\\\\\\\";
        else { if (c == '"' || c == '`' || c == '$') out += "\\\\"; out += char(c); }
    }
    return out + '"';
}
std::string desktop_value(const fs::path& path) {
    std::string out;
    for (unsigned char c : path.string()) {
        if (c < 32 || c == 127) fail("Control character in desktop path");
        if (c == '\\') out += "\\\\"; else out += char(c);
    }
    return out;
}
struct DesktopChange {
    fs::path target;
    bool existed{}, applied{}, exchanged_known{};
    std::string before, after;
    struct stat initial{}, committed{}, exchanged{};
    std::unique_ptr<Temporary> pending;
    explicit DesktopChange(const fs::path& directory, const std::string& text) : after(text) {
        auto dir = checked_absolute(directory); fs::create_directories(dir);
        if (!fs::is_directory(dir)) fail("Desktop integration destination is not a directory");
        target = dir / "pusu-native.desktop";
        if (fs::exists(fs::symlink_status(target))) {
            regular(target); before = read_text(target, 65536); existed = true; initial = identity(target);
            if (!before.starts_with("[Desktop Entry]\nX-Pusu-Native-Owned=true\n")) fail("Refusing to replace unrelated desktop application");
        }
    }
    void apply() {
        checked_absolute(target);
        if (existed && (!same_identity(target, initial) || read_text(target, 65536) != before))
            fail("Desktop application changed during installation; refusing replacement");
        pending = std::make_unique<Temporary>(target.parent_path());
        const auto file = pending->path / "entry";
        write_text(file, after, true); committed = identity(file);
        atomic_rename(file, target, existed ? RENAME_EXCHANGE : RENAME_NOREPLACE);
        applied = true;
        if (existed) { exchanged = identity(file); exchanged_known = true; }
        if (existed && (!same_identity(file, initial) || read_text(file, 65536) != before))
            fail("Desktop application raced publication; rolling back without deleting it");
        if (!same_identity(target, committed) || read_text(target, 65536) != after)
            fail("Desktop application changed immediately after publication");
    }
    void validate_commit() const {
        if (!same_identity(target, committed) || read_text(target, 65536) != after)
            fail("Desktop application changed before final commit");
        if (existed && (!same_identity(pending->path / "entry", exchanged) ||
                        read_text(pending->path / "entry", 65536) != before))
            fail("Desktop backup changed before final commit");
    }
    void rollback() {
        if (!applied) return;
        const auto recovery = pending->path;
        try {
            const auto file = pending->path / "entry";
            if (!same_identity(target, committed) || read_text(target, 65536) != after)
                fail("Desktop application changed after publication");
            if (existed && (!exchanged_known || !same_identity(file, exchanged)))
                fail("Desktop backup identity changed");
            atomic_rename(target, file, existed ? RENAME_EXCHANGE : RENAME_NOREPLACE);
            if (!same_identity(file, committed) || (existed && !same_identity(target, exchanged))) {
                const auto displaced = identity(file);
                struct stat restored{};
                if (existed) restored = identity(target);
                if (!same_identity(file, displaced) || (existed && !same_identity(target, restored)))
                    fail("Desktop rollback identities changed again");
                atomic_rename(file, target, existed ? RENAME_EXCHANGE : RENAME_NOREPLACE);
                fail("Desktop rollback raced publication; reversed the exchange without deleting either file");
            }
            applied = false;
        } catch (const std::exception& e) {
            pending->path.clear();
            fail("Desktop rollback refused; preserved recovery directory " + recovery.string() + ": " + e.what());
        } catch (...) {
            pending->path.clear(); throw;
        }
    }
};
void publish(Temporary& stage, const Destination& root, std::vector<DesktopChange>& desktop,
             const Owned* old_owned = nullptr, const Preserved* preserved = nullptr,
             const ImportProgress& progress = {}) {
    root.validate();
    if (root.existed) {
        if (!old_owned || !preserved || previous_owned(root.path) != *old_owned)
            fail("Ownership changed before atomic publication");
        validate_unowned(root.path, stage.path, *old_owned, *preserved);
    }
    const auto committed = identity(stage.path);
    bool published = false, exchanged_known = false;
    struct stat exchanged{};
    try {
        atomic_rename(stage.path, root.path, root.existed ? RENAME_EXCHANGE : RENAME_NOREPLACE);
        published = true;
        if (root.existed) { exchanged = identity(stage.path); exchanged_known = true; }
        report(progress, 0.99, "Validating atomic installation publication");
        if (root.existed) {
            if (!same_identity(stage.path, root.initial) || previous_owned(stage.path) != *old_owned)
                fail("Existing root changed during atomic exchange; rolling back without deleting it");
            validate_unowned(stage.path, root.path, *old_owned, *preserved);
        }
        for (auto& change : desktop) change.apply();
        for (const auto& change : desktop) change.validate_commit();
        if (!same_identity(root.path, committed)) fail("Published installation changed before commit");
        if (root.existed) {
            if (!same_identity(stage.path, exchanged)) fail("Installation backup identity changed before commit");
            validate_unowned(stage.path, root.path, *old_owned, *preserved);
        }
    } catch (...) {
        auto original = std::current_exception();
        std::string rollback_error;
        for (auto it = desktop.rbegin(); it != desktop.rend(); ++it) {
            try { it->rollback(); }
            catch (const std::exception& e) { rollback_error += std::string(e.what()) + "; "; }
        }
        if (published) {
            try {
                if (!same_identity(root.path, committed)) fail("Published installation identity changed concurrently");
                if (root.existed && (!exchanged_known || !same_identity(stage.path, exchanged)))
                    fail("Installation backup identity changed concurrently");
                atomic_rename(root.path, stage.path, root.existed ? RENAME_EXCHANGE : RENAME_NOREPLACE);
                if (!same_identity(stage.path, committed) || (root.existed && !same_identity(root.path, exchanged))) {
                    const auto displaced = identity(stage.path);
                    struct stat restored{};
                    if (root.existed) restored = identity(root.path);
                    if (!same_identity(stage.path, displaced) || (root.existed && !same_identity(root.path, restored)))
                        fail("Installation rollback identities changed again");
                    atomic_rename(stage.path, root.path, root.existed ? RENAME_EXCHANGE : RENAME_NOREPLACE);
                    fail("Installation rollback raced publication; reversed the exchange and retained both roots");
                }
            } catch (const std::exception& e) {
                auto recovery = stage.path; stage.path.clear();
                rollback_error += "Installation recovery preserved at " + recovery.string() + ": " + e.what();
            }
        }
        // Atomic save replacement can introduce new bytes after any validation.
        // Never delete a tree that was published, even after restoring the old root.
        if (published && !stage.path.empty()) {
            auto recovery = stage.path; stage.path.clear();
            if (!rollback_error.empty())
                fail("Publication rollback could not safely complete; recovery tree retained at " + recovery.string() + ": " + rollback_error);
            try { std::rethrow_exception(original); }
            catch (const std::exception& e) {
                fail("Installation publication rolled back; newly published tree retained at " + recovery.string() + ": " + e.what());
            } catch (...) {
                fail("Installation publication rolled back; newly published tree retained at " + recovery.string());
            }
        }
        if (!rollback_error.empty()) fail("Publication rollback could not safely complete: " + rollback_error);
        std::rethrow_exception(original);
    }
    if (!root.existed) stage.path.clear();
}
} // namespace

fs::path executable_directory() {
    std::vector<char> buffer(256);
    for (;;) {
        auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) fail("Cannot resolve native executable path: " + std::string(std::strerror(errno)));
        if (std::size_t(length) < buffer.size()) return fs::path(std::string(buffer.data(), std::size_t(length))).parent_path();
        if (buffer.size() >= 65536) fail("Native executable path exceeds limit");
        buffer.resize(buffer.size() * 2);
    }
}
void extract_source(const fs::path& source, const fs::path& destination, const ImportProgress& progress) {
    Destination root(destination);
    if (fs::exists(root.path)) fail("Extraction requires a fresh, absent destination");
    Temporary stage(root.path.parent_path()), scratch(root.path.parent_path());
    extract_into(source, stage.path, scratch.path, progress);
    auto owned = inventory(stage.path); seal(stage.path, owned);
    std::vector<DesktopChange> none; publish(stage, root, none, nullptr, nullptr, progress);
    report(progress, 1.0, "Original assets extracted");
}
void install_source(const InstallRequest& request, const ImportProgress& progress) {
    Destination root(request.destination);
    auto old_owned = previous_owned(root.path);
    auto runtime = checked_absolute(request.runtime_directory);
    if (!fs::is_directory(runtime)) fail("Native runtime distribution is absent");
    auto bin = fs::is_regular_file(runtime / "pusu-game") ? runtime : runtime / "bin";
    auto lib = fs::is_directory(runtime / "lib") ? runtime / "lib" : bin.parent_path() / "lib";
    auto share = fs::is_directory(runtime / "share") ? runtime / "share" : bin.parent_path() / "share";
    bin = checked_absolute(bin); lib = checked_absolute(lib); share = checked_absolute(share);
    native_binary(bin / "pusu-game"); native_binary(bin / "pusu-launcher");
    Temporary stage(root.path.parent_path()), scratch(root.path.parent_path());
    extract_into(request.source, stage.path / "data", scratch.path, progress);
    copy_runtime_file(bin / "pusu-game", stage.path / "bin/pusu-game", true);
    copy_runtime_file(bin / "pusu-launcher", stage.path / "bin/pusu-launcher", true);
    copy_tree(lib, stage.path / "lib"); copy_tree(share, stage.path / "share");
    fs::path icon;
    for (const auto* filename : {"pusu.png", "pusu.svg"})
        if (fs::is_regular_file(stage.path / "share/pusu" / filename)) { icon = root.path / "share/pusu" / filename; break; }
    if (icon.empty()) fail("Packaged Pusu desktop icon is absent");
    const auto settings = checked_absolute(user_settings_file());
    const auto settings_relative = settings.lexically_relative(root.path);
    if (!settings_relative.empty() && *settings_relative.begin() != "..")
        fail("Settings must be outside the installation root");
    // Settings and saves are not installation-owned; the launcher initializes settings independently.
    auto owned = inventory(stage.path);
    root.validate();
    if (previous_owned(root.path) != old_owned)
        fail("Ownership manifest changed during extraction; existing installation was preserved");
    auto preserved = preserve_unowned(root.path, stage.path, old_owned); seal(stage.path, owned);
    const auto desktop = std::string("[Desktop Entry]\nX-Pusu-Native-Owned=true\nType=Application\nVersion=1.0\nName=Pusu: Uyanış\nComment=Native Pusu game and graphics settings\nExec=") +
        desktop_quote(root.path / "bin/pusu-launcher") + " --data " + desktop_quote(root.path / "data") + " --settings " + desktop_quote(settings) +
        "\nIcon=" + desktop_value(icon) + "\nTerminal=false\nCategories=Game;ActionGame;\nStartupNotify=true\n";
    std::vector<DesktopChange> changes;
    if (request.applications_directory.empty()) fail("An applications directory is required for installation");
    auto applications = checked_absolute(request.applications_directory);
    auto desktop_directory = request.desktop_directory.empty() ? fs::path{} : checked_absolute(request.desktop_directory);
    for (const auto& directory : {applications, desktop_directory}) {
        if (directory.empty()) continue;
        auto relative = directory.lexically_relative(root.path);
        if (!relative.empty() && *relative.begin() != "..")
            fail("Desktop integration directories must be outside the installation root");
    }
    changes.emplace_back(applications, desktop);
    if (!desktop_directory.empty() && desktop_directory != applications) changes.emplace_back(desktop_directory, desktop);
    report(progress, 0.98, "Publishing native installation");
    publish(stage, root, changes, &old_owned, &preserved, progress);
    report(progress, 1.0, "Native Pusu installation complete");
}
} // namespace pusu
