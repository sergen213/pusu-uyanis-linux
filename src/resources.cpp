#include "resources.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <openssl/evp.h>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

namespace pusu {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t resource_limit = 256U * 1024U * 1024U;

[[noreturn]] void resource_error(std::string_view reason) {
    throw std::runtime_error("AssetStore: " + std::string(reason));
}

char resource_lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool resource_filename(std::string_view name, bool optional = false) {
    if (name.empty() || name.front() == '/' || name.front() == '\\')
        resource_error("asset name must be relative");
    bool filename = true;
    std::size_t component = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '/' || c == '\\') {
            if (i == component || name[i - 1] == '.' || name[i - 1] == ' ')
                resource_error("invalid asset path component");
            component = i + 1;
        } else {
            if (static_cast<unsigned char>(c) < 32 || c == 127 || c == ':')
                resource_error("invalid Windows asset name");
            if (c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') {
                if (!optional) resource_error("invalid Windows asset name");
                filename = false;
            }
        }
    }
    if (name.size() == component || name.back() == '.' || name.back() == ' ')
        resource_error("invalid asset path component");
    return filename;
}

std::string resource_name(std::string_view name) {
    resource_filename(name);
    std::string key(name);
    for (char& c : key) c = c == '\\' ? '/' : resource_lower(c);
    return key;
}

bool resource_contained(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin(), c = candidate.begin();
    for (; r != root.end(); ++r, ++c)
        if (c == candidate.end() || *r != *c) return false;
    return true;
}

fs::path resource_resolve(const fs::path& root, const fs::path& relative) {
    if (fs::canonical(root) != root || !fs::is_directory(root))
        resource_error("asset root changed");
    fs::path candidate = root;
    for (const auto& part : relative) {
        candidate /= part;
        const auto resolved = fs::canonical(candidate);
        if (!resource_contained(root, resolved)) resource_error("symlink escapes asset root");
    }
    return fs::canonical(candidate);
}

struct ResourceDescriptor {
    int value = -1;
    ~ResourceDescriptor() { if (value >= 0) ::close(value); }
    ResourceDescriptor() = default;
    ResourceDescriptor(const ResourceDescriptor&) = delete;
    ResourceDescriptor& operator=(const ResourceDescriptor&) = delete;
    void replace(int next) {
        if (next < 0) resource_error(std::strerror(errno));
        if (value >= 0) ::close(value);
        value = next;
    }
};

std::vector<std::uint8_t> resource_read(const fs::path& resolved) {
    // Pin each canonical directory with openat; a substituted symlink cannot
    // escape containment between path validation and the actual file read.
    ResourceDescriptor descriptor;
    descriptor.replace(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    const auto relative = resolved.relative_path();
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        auto next = part;
        ++next;
        descriptor.replace(::openat(descriptor.value, part->c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
            (next == relative.end() ? 0 : O_DIRECTORY)));
    }
    struct stat status {};
    if (::fstat(descriptor.value, &status) != 0) resource_error(std::strerror(errno));
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > resource_limit)
        resource_error("not a regular asset or file exceeds 256 MiB");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(status.st_size));
    std::size_t consumed = 0;
    while (consumed < data.size()) {
        const auto count = ::read(descriptor.value, data.data() + consumed, data.size() - consumed);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) resource_error("asset read failed or file changed");
        consumed += static_cast<std::size_t>(count);
    }
    std::uint8_t extra;
    ssize_t count;
    do { count = ::read(descriptor.value, &extra, 1); } while (count < 0 && errno == EINTR);
    if (count != 0) resource_error("asset read failed or file changed");
    return data;
}

constexpr std::array<std::uint8_t, 9> resource_marker {0, 'T', 'M', 'S', 'A', 'M', 'V', 'O', 'H'};
constexpr std::array<std::uint8_t, 14> resource_prefix {
    0xa4, 0x9b, 0xfd, 0xff, 0x26, 0x24, 0xe9, 0xd7, 0xf1, 0xd6, 0xf0, 0xd6, 0xae, 0xbe};
constexpr std::array<std::uint8_t, 18> resource_suffix {
    0xa4, 0x9b, 0xfd, 0xff, 0x26, 0x21, 0xec, 0xce, 0xf1, 0xd6, 0xf0, 0xd6, 0xae, 0xbe, 1, 0, 0x14, 0};
constexpr std::array<std::uint8_t, 14> resource_tail {
    0xa4, 0x9b, 0xfd, 0xff, 0x26, 0x2b, 0xe4, 0xc3, 0xf1, 0xd6, 0xf0, 0xd6, 0xae, 0xbe};

template<std::size_t N>
bool resource_matches(std::span<const std::uint8_t> data, std::size_t at,
                      const std::array<std::uint8_t, N>& marker) {
    return at <= data.size() && N <= data.size() - at &&
        std::equal(marker.begin(), marker.end(), data.begin() + at);
}

bool resource_suffix_at(std::span<const std::uint8_t> data, std::size_t at) {
    if (at > data.size() || resource_suffix.size() > data.size() - at) return false;
    for (std::size_t n = 0; n < resource_suffix.size(); ++n)
        if (n == 10 ? (data[at + n] != 0xf0 && data[at + n] != 0xf1)
                    : data[at + n] != resource_suffix[n]) return false;
    return true;
}

std::uint32_t resource_u32(std::span<const std::uint8_t> data, std::size_t at) {
    if (at > data.size() || 4 > data.size() - at) resource_error("truncated container field");
    return std::uint32_t(data[at]) | (std::uint32_t(data[at + 1]) << 8) |
        (std::uint32_t(data[at + 2]) << 16) | (std::uint32_t(data[at + 3]) << 24);
}

void resource_inflate(std::span<const std::uint8_t> compressed, std::size_t logical,
                      std::vector<std::uint8_t>& output) {
    if (logical > resource_limit || output.size() > resource_limit - logical)
        resource_error("decoded resource exceeds 256 MiB");
    const auto initial = output.size();
    output.resize(initial + logical + 1); // One extra byte detects an understated logical length.
    z_stream stream {};
    if (::inflateInit(&stream) != Z_OK) resource_error("zlib initialization failed");
    struct EndInflate { z_stream* stream; ~EndInflate() { ::inflateEnd(stream); } } end {&stream};
    stream.next_in = const_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = output.data() + initial;
    stream.avail_out = static_cast<uInt>(logical + 1);
    const auto result = ::inflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END || stream.total_in != compressed.size() ||
        stream.avail_in != 0 || stream.total_out != logical)
        resource_error("nonexact zlib stream or incorrect logical length");
    output.resize(initial + logical);
}

void resource_transform(std::span<std::uint8_t> payload) {
    // Observed class-2 key from the authorized, unmodified isolated patch runtime:
    // .work/transform/observed-key.json and .work/diagnosis/class2-name.log:372-405.
    constexpr std::array<unsigned char, 16> key {
        0xd6, 0x0b, 0xf7, 0x0e, 0xed, 0xfb, 0xf0, 0xaa,
        0x3b, 0xdc, 0x82, 0x91, 0x86, 0xb8, 0x56, 0x5e};
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
        cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher || EVP_EncryptInit_ex(cipher.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1 ||
        EVP_CIPHER_CTX_set_padding(cipher.get(), 0) != 1)
        resource_error("AES initialization failed");
    std::array<unsigned char, 4096> counters {};
    std::array<unsigned char, 4096 + 16> masks {};
    std::uint32_t counter = 0;
    for (std::size_t at = 0; at < payload.size();) {
        const auto amount = std::min(counters.size(), payload.size() - at);
        const auto blocks = (amount + 15) / 16;
        for (std::size_t block = 0; block < blocks; ++block, ++counter) {
            auto* bytes = counters.data() + block * 16;
            bytes[0] = static_cast<unsigned char>(counter >> 24);
            bytes[1] = static_cast<unsigned char>(counter >> 16);
            bytes[2] = static_cast<unsigned char>(counter >> 8);
            bytes[3] = static_cast<unsigned char>(counter);
        }
        int written = 0;
        if (EVP_EncryptUpdate(cipher.get(), masks.data(), &written, counters.data(),
                              static_cast<int>(blocks * 16)) != 1 || written != static_cast<int>(blocks * 16))
            resource_error("AES counter transform failed");
        for (std::size_t n = 0; n < amount; ++n)
            payload[at + n] ^= masks[(n & ~std::size_t(3)) + (3 - (n & 3))];
        at += amount;
    }
    int written = 0;
    if (EVP_EncryptFinal_ex(cipher.get(), masks.data(), &written) != 1 || written != 0)
        resource_error("AES counter finalization failed");
}

std::vector<std::uint8_t> resource_decode(std::vector<std::uint8_t> data, std::string_view basename) {
    if (!resource_matches(data, 0, resource_marker)) return data;
    if (data.size() < resource_marker.size() + 26) resource_error("truncated protected container");
    const auto footer = data.size() - 26;
    constexpr std::array<std::uint8_t, 8> footer_marker {'T', 'M', 'S', 'A', 'M', 'V', 'O', 'F'};
    if (!resource_matches(data, footer, footer_marker) || resource_u32(data, footer + 8) != 1 ||
        data[footer + 12] != 0 || data[footer + 13] != 0 ||
        resource_u32(data, footer + 14) != footer - 1)
        resource_error("invalid protected footer or boundary");
    const std::span<const std::uint8_t> records(data.data(), footer);
    std::vector<std::uint8_t> output;
    bool filename_validated = false;
    std::size_t position = resource_marker.size();
    while (!resource_matches(records, position, resource_tail)) {
        std::size_t suffix = position;
        if (resource_matches(records, position, resource_prefix)) {
            const auto search_end = std::min(records.size(), position + 120);
            suffix = position + resource_prefix.size();
            while (suffix <= search_end && resource_suffix.size() <= search_end - suffix &&
                   !resource_suffix_at(records, suffix)) ++suffix;
            if (suffix > search_end || resource_suffix.size() > search_end - suffix)
                resource_error("missing bounded record suffix");
        } else if (!resource_suffix_at(records, suffix)) {
            resource_error("malformed record or missing protected index tail");
        }
        const auto digest_at = suffix + resource_suffix.size();
        if (digest_at > records.size() || 37 > records.size() - digest_at)
            resource_error("truncated protected record header");
        const auto fields = digest_at + 20;
        const auto flags = resource_u32(records, fields);
        const auto method = records[fields + 4];
        const auto stored = resource_u32(records, fields + 5);
        const auto logical = resource_u32(records, fields + 9);
        const auto begin = fields + 17;
        if (stored > resource_limit || logical > resource_limit || stored > records.size() - begin)
            resource_error("invalid protected chunk length");
        if (method != 1 || (flags != 0 && flags != 2))
            resource_error("unsupported protected chunk class or method");
        std::span<std::uint8_t> payload(data.data() + begin, stored);
        std::array<unsigned char, 20> digest {};
        unsigned int digest_length = 0;
        if (EVP_Digest(payload.data(), payload.size(), digest.data(), &digest_length, EVP_sha1(), nullptr) != 1 ||
            digest_length != digest.size() || !std::equal(digest.begin(), digest.end(), data.begin() + digest_at))
            resource_error("protected chunk SHA1 mismatch");
        if (flags == 2) {
            resource_transform(payload);
            resource_inflate(payload, logical, output);
        } else {
            std::vector<std::uint8_t> metadata;
            resource_inflate(payload, logical, metadata);
            for (const auto byte : metadata)
                if (byte > 127 || byte == 0) resource_error("invalid ASCII container metadata");
            const std::string_view text(reinterpret_cast<const char*>(metadata.data()), metadata.size());
            if (text.find("/content/raw") != std::string_view::npos) {
                const auto quote = text.find('"', 1);
                if (text.empty() || text.front() != '"' || quote == std::string_view::npos ||
                    text.substr(quote) != "\" \"/content/raw\" \"0[2]\"")
                    resource_error("malformed content filename metadata");
                const auto filename = text.substr(1, quote - 1);
                if (filename.find_first_of("/\\") != std::string_view::npos ||
                    resource_name(filename) != resource_name(basename))
                    resource_error("protected content filename does not match source basename");
                filename_validated = true;
            }
        }
        position = begin + stored;
    }
    // The fixed tail is separately encrypted index/authentic packaging metadata,
    // not another authored-content chunk; the validated footer bounds it.
    if (!filename_validated || output.empty())
        resource_error("protected container lacks validated filename or nonempty content");
    return output;
}
} // namespace

AssetStore::AssetStore(fs::path root) : root_(fs::canonical(std::move(root))) {
    if (!fs::is_directory(root_)) resource_error("asset root is not a directory");
    std::vector<fs::path> ancestors {root_};
    const auto index = [&](const auto& self, const fs::path& relative) -> void {
        for (const auto& entry : fs::directory_iterator(root_ / relative)) {
            const auto child = relative / entry.path().filename();
            const auto key = resource_name(child.generic_string());
            if (!files_.emplace(key, child).second)
                resource_error("case-insensitive asset path collision");
            const auto resolved = fs::canonical(entry.path());
            if (!resource_contained(root_, resolved)) resource_error("symlink escapes asset root");
            if (fs::is_directory(resolved)) {
                if (std::find(ancestors.begin(), ancestors.end(), resolved) != ancestors.end())
                    resource_error("cyclic asset directory symlink");
                ancestors.push_back(resolved);
                self(self, child);
                ancestors.pop_back();
            } else if (!fs::is_regular_file(resolved)) {
                resource_error("nonregular entry in asset root");
            }
        }
    };
    index(index, fs::path {});
}

bool AssetStore::contains(std::string_view logical_name) const {
    return files_.contains(resource_name(logical_name));
}

bool AssetStore::contains_optional_game_file(std::string_view logical_name) const {
    // Validate the entire name before absence: punctuation must not hide traversal.
    return resource_filename(logical_name, true) && files_.contains(logical_name);
}

std::optional<Mesh> read_runtime_mesh(const AssetStore& assets, std::string_view logical_pm_path) {
    if (!assets.contains(logical_pm_path)) return std::nullopt;
    return read_mesh(assets.path(logical_pm_path));
}

fs::path AssetStore::path(std::string_view logical_name) const {
    const auto found = files_.find(resource_name(logical_name));
    if (found == files_.end()) resource_error("asset not found: " + std::string(logical_name));
    const auto resolved = resource_resolve(root_, found->second);
    if (!fs::is_regular_file(resolved) && !fs::is_directory(resolved))
        resource_error("asset is no longer a regular file or directory");
    return resolved;
}

std::vector<std::uint8_t> AssetStore::bytes(std::string_view logical_name) const {
    const auto found = files_.find(resource_name(logical_name));
    if (found == files_.end()) resource_error("asset not found: " + std::string(logical_name));
    const auto resolved = resource_resolve(root_, found->second);
    return resource_decode(resource_read(resolved), found->second.filename().string());
}

std::string AssetStore::text(std::string_view logical_name) const {
    const auto data = bytes(logical_name);
    if (data.empty()) return {};
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

} // namespace pusu

#include <charconv>
#include <cmath>
#include <limits>

namespace pusu {
namespace {
constexpr std::size_t shader_source_limit = 16U * 1024U * 1024U;
constexpr std::size_t shader_item_limit = 4096;
constexpr std::size_t shader_definition_limit = 65536;
using ShaderTokens = std::vector<std::string_view>;

[[noreturn]] void shader_error(std::string_view reason) {
    throw std::runtime_error("MaterialLibrary: " + std::string(reason));
}

bool shader_equal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](char x, char y) { return resource_lower(x) == resource_lower(y); });
}

std::string shader_key(std::string_view name) {
    std::string key(name);
    for (char& c : key) c = resource_lower(c);
    return key;
}

bool shader_token_byte(unsigned char c) {
    if (c >= ' ' && c <= '~') return true;
    // Original 004866c0 accepts these single-byte Turkish characters in
    // addition to CRT printable ASCII; never transcode or Unicode-casefold.
    constexpr std::array<unsigned char, 12> extra{
        0xfd, 0xdd, 0xdc, 0xfc, 0xde, 0xfe, 0xd0, 0xf0, 0xc7, 0xe7, 0xd6, 0xf6};
    return std::find(extra.begin(), extra.end(), c) != extra.end();
}

// The original consumes LF-delimited lines, not a Quake brace/block-comment lexer.
// Space is quote-aware; control whitespace still terminates a quoted token.
bool shader_line(std::string_view source, std::size_t& position, ShaderTokens& tokens,
                 std::size_t& work) {
    tokens.clear();
    if (position == source.size()) return false;
    const auto end = source.find('\n', position);
    const auto raw_line = source.substr(position, (end == source.npos ? source.size() : end) - position);
    const auto line = raw_line.substr(0, raw_line.find('\0'));
    position = end == source.npos ? source.size() : end + 1;
    if (raw_line.size() > 65536) shader_error("shader line exceeds limit");
    work += raw_line.size();
    if (work > 64U * 1024U * 1024U) shader_error("shader parsing work exceeds limit");
    std::size_t begin = 0;
    while (begin < line.size()) {
        while (begin < line.size() &&
               (!shader_token_byte(static_cast<unsigned char>(line[begin])) || line[begin] == ' ')) ++begin;
        if (begin == line.size()) break;
        bool quoted = false, had_quote = false;
        std::size_t finish = begin;
        while (finish < line.size()) {
            const auto c = static_cast<unsigned char>(line[finish]);
            if (!shader_token_byte(c) || (c == ' ' && !quoted)) break;
            if (c == '"') { quoted = !quoted; had_quote = true; }
            ++finish;
        }
        auto token = line.substr(begin, finish - begin);
        if (had_quote && !quoted && token.size() >= 2)
            token = token.substr(1, token.size() - 2);
        if (token.starts_with("//")) break;
        if (token.size() > 8192 || tokens.size() == shader_item_limit)
            shader_error("shader token limit exceeded");
        tokens.push_back(token);
        begin = finish;
    }
    return true;
}

double shader_number(std::string_view token) {
    if (token.starts_with('+')) token.remove_prefix(1);
    double value{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || !std::isfinite(value)) return 0;
    return value;
}

float shader_float(std::string_view token) {
    const double value = shader_number(token);
    if (std::abs(value) > std::numeric_limits<float>::max())
        shader_error("shader number exceeds float range");
    return static_cast<float>(value);
}

std::int32_t shader_integer(std::string_view token) {
    if (token.starts_with('+')) token.remove_prefix(1);
    std::int32_t value{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec == std::errc::result_out_of_range) shader_error("shader integer exceeds range");
    return result.ec == std::errc{} ? value : 0;
}

std::uint8_t shader_byte(double value) {
    // Original x87 multiply, truncating signed DWORD conversion, then low byte;
    // neither saturation nor floating-point colors reproduce custom/custom_ovb.
    const long double scaled = std::trunc(static_cast<long double>(value) * 255.0L);
    if (scaled < std::numeric_limits<std::int32_t>::min() ||
        scaled > std::numeric_limits<std::int32_t>::max()) return 0;
    return static_cast<std::uint8_t>(static_cast<std::int32_t>(scaled));
}

std::optional<Waveform> shader_waveform(std::string_view token) {
    if (shader_equal(token, "sin")) return Waveform::sin;
    if (shader_equal(token, "triangle")) return Waveform::triangle;
    if (shader_equal(token, "square")) return Waveform::square;
    if (shader_equal(token, "sawtooth")) return Waveform::sawtooth;
    if (shader_equal(token, "inversesawtooth")) return Waveform::inverse_sawtooth;
    return {};
}

MaterialWave shader_wave(const ShaderTokens& t, std::size_t function, Waveform waveform) {
    return {waveform, shader_float(t[function + 1]), shader_float(t[function + 2]),
        shader_float(t[function + 3]), shader_float(t[function + 4])};
}

void shader_material_command(Material& m, const ShaderTokens& t) {
    const auto is = [&](std::string_view s) { return shader_equal(t[0], s); };
    const auto arg = [&](std::string_view s) { return t.size() > 1 && shader_equal(t[1], s); };
    if (is("nocompression")) m.compression = false;
    else if (is("nopicmip")) m.picmip = false;
    else if (is("nomipmaps")) m.mipmaps = false;
    else if (is("nofog")) m.fog = false;
    else if (is("takeable")) m.takeable = true;
    else if (is("polygonoffset")) m.polygon_offset = t.size() > 1 ? shader_float(t[1]) : -2.0f;
    else if (is("skybox")) {
        const bool alternate = t.size() == 2 && arg("in");
        m.sky = alternate ? MaterialSky::alternate : MaterialSky::normal;
        m.sort = alternate ? MaterialSort::sky_alternate : MaterialSort::sky;
    } else if (is("deformvertexes") && t.size() > 1) {
        MaterialDeform deform;
        if (arg("autosprite")) deform.kind = MaterialDeformKind::autosprite;
        else if (arg("autosprite2")) deform.kind = MaterialDeformKind::autosprite2;
        else if (arg("wave") && t.size() == 8) {
            const auto function = shader_waveform(t[3]);
            if (!function) return;
            deform.spread = shader_float(t[2]);
            deform.wave = shader_wave(t, 3, *function);
        } else return;
        if (m.deforms.size() == shader_item_limit) shader_error("too many material deforms");
        m.deforms.push_back(deform);
    } else if (t.size() == 2) {
        if (is("material")) m.physical_material_name = t[1];
        else if (is("reflection")) m.reflection = shader_float(t[1]);
        else if (is("cull")) {
            if (arg("front")) m.cull = MaterialCull::front;
            else if (arg("back")) m.cull = MaterialCull::back;
            else if (arg("none") || arg("disable")) m.cull = MaterialCull::none;
        } else if (is("lightgrid")) {
            if (arg("off")) m.lightgrid = MaterialLightGrid::off;
            else if (arg("center")) m.lightgrid = MaterialLightGrid::center;
            else if (arg("interpolate")) m.lightgrid = MaterialLightGrid::interpolate;
            else if (arg("interpolate_once")) m.lightgrid = MaterialLightGrid::interpolate_once;
        } else if (is("surfaceparm")) {
            if (arg("nonsolid")) m.collision_enabled = false;
            else if (arg("playerclip")) m.collision_enabled = true;
            else if (arg("noimpact")) m.impact_enabled = false;
        }
    }
}

MaterialBlendFactor shader_blend_factor(std::string_view t, bool source) {
    if (shader_equal(t, "gl_one")) return MaterialBlendFactor::one;
    if (shader_equal(t, "gl_src_color")) return MaterialBlendFactor::src_color;
    if (shader_equal(t, "gl_dst_color")) return MaterialBlendFactor::dst_color;
    if (source && shader_equal(t, "gl_one_minus_dst_color")) return MaterialBlendFactor::one_minus_dst_color;
    if (!source && shader_equal(t, "gl_one_minus_src_color")) return MaterialBlendFactor::one_minus_src_color;
    if (shader_equal(t, "gl_src_alpha")) return MaterialBlendFactor::src_alpha;
    if (shader_equal(t, "gl_one_minus_src_alpha")) return MaterialBlendFactor::one_minus_src_alpha;
    return MaterialBlendFactor::zero;
}

void shader_add_modifier(MaterialPass& p, MaterialTcMod mod) {
    if (p.tc_mods.size() == shader_item_limit) shader_error("too many texture modifiers");
    p.tc_mods.push_back(mod);
}

void shader_pass_command(MaterialPass& p, const ShaderTokens& t) {
    const auto is = [&](std::string_view s) { return shader_equal(t[0], s); };
    const auto arg = [&](std::string_view s) { return t.size() > 1 && shader_equal(t[1], s); };
    if (is("depthwrite")) p.depth_write_policy = MaterialDepthWrite::on;
    else if (is("nodepthwrite")) p.depth_write_policy = MaterialDepthWrite::off;
    else if (is("nodepthtest")) p.depth_test = false;
    else if (is("map") && t.size() > 1) {
        MaterialTexture texture;
        if (arg("$lightmap")) texture.kind = MaterialTextureKind::lightmap;
        else if (arg("$whiteimage")) texture.kind = MaterialTextureKind::white;
        else if (arg("$cubemap")) {
            // Native renderer supports cubemaps and edge clamp, so take the
            // original capable branch rather than activating its image fallback.
            texture.kind = t.size() < 5 ? MaterialTextureKind::cube_capture : MaterialTextureKind::cube_faces;
            texture.wrap = TextureWrap::clamp_to_edge;
            if (t.size() > 2) texture.fallback_image = t[2];
            if (t.size() > 3) texture.cube_size = shader_integer(t[3]);
            if (texture.cube_size <= 0 || texture.cube_size > 2048)
                shader_error("cubemap size outside supported range");
            for (std::size_t i = 0; i < 6 && i + 4 < t.size(); ++i)
                texture.cube_faces[i] = t[i + 4];
            shader_add_modifier(p, {MaterialTcModKind::cube, {}});
        } else {
            texture.kind = MaterialTextureKind::image;
            texture.image = t[1];
        }
        if (texture.kind == MaterialTextureKind::lightmap) {
            p.source_mode = MaterialSourceMode::lightmap;
        } else if (texture.kind == MaterialTextureKind::cube_capture ||
                   texture.kind == MaterialTextureKind::cube_faces) {
            p.source_mode = MaterialSourceMode::cube;
            p.tc_gen = MaterialTcGen::cube;
        } else p.source_mode = MaterialSourceMode::image;
        p.texture = std::move(texture);
    } else if (is("clampmap") && t.size() > 1 && !arg("$cubemap")) {
        MaterialTexture texture;
        texture.kind = MaterialTextureKind::image;
        texture.wrap = TextureWrap::clamp_to_edge;
        texture.image = t[1];
        p.source_mode = MaterialSourceMode::image;
        p.texture = std::move(texture);
    } else if ((is("animmap") || is("animclampmap")) && t.size() > 1) {
        MaterialAnimation animation;
        animation.wrap = is("animclampmap") ? TextureWrap::clamp : TextureWrap::repeat;
        animation.frequency = shader_float(t[1]);
        animation.frames.reserve(t.size() - 2);
        for (std::size_t i = 2; i < t.size(); ++i) animation.frames.emplace_back(t[i]);
        p.animation = std::move(animation);
        p.source_mode = MaterialSourceMode::image;
        p.texture = {};
    } else if (is("combinedmap") && t.size() >= 4) {
        MaterialTexture texture;
        texture.kind = MaterialTextureKind::combined;
        texture.image = t[1];
        texture.width = shader_integer(t[2]);
        texture.height = shader_integer(t[3]);
        if (texture.width <= 0 || texture.height <= 0 || texture.width > 8192 || texture.height > 8192 ||
            static_cast<std::uint64_t>(texture.width) * texture.height > 16U * 1024U * 1024U)
            shader_error("combined texture dimensions outside supported range");
        texture.layers.reserve((t.size() - 4) / 4);
        // Ignore a trailing incomplete group instead of the original out-of-bounds read.
        for (std::size_t i = 4; i + 3 < t.size(); i += 4)
            texture.layers.push_back({std::string(t[i]), std::string(t[i + 1]),
                shader_integer(t[i + 2]), shader_integer(t[i + 3])});
        p.source_mode = MaterialSourceMode::image;
        p.texture = std::move(texture);
    } else if (is("avimap")) {
        if (t.size() > 1) {
            MaterialTexture texture;
            texture.kind = MaterialTextureKind::avi;
            texture.image = t[1];
            p.source_mode = MaterialSourceMode::image;
            p.texture = std::move(texture);
        }
        shader_add_modifier(p, {MaterialTcModKind::avi, {}});
    } else if (is("blendfunc")) {
        if (t.size() == 3) {
            p.blend = true;
            p.blend_source = shader_blend_factor(t[1], true);
            p.blend_destination = shader_blend_factor(t[2], false);
        } else if (t.size() == 2) {
            if (arg("filter")) {
                p.blend_source = MaterialBlendFactor::dst_color;
                p.blend_destination = MaterialBlendFactor::zero;
            } else if (arg("add")) {
                p.blend_source = MaterialBlendFactor::one;
                p.blend_destination = MaterialBlendFactor::one;
            } else if (arg("blend")) {
                p.blend_source = MaterialBlendFactor::src_alpha;
                p.blend_destination = MaterialBlendFactor::one_minus_src_alpha;
            } else return;
            p.blend = true;
        }
    } else if (is("rgbgen")) {
        if (t.size() >= 3 && (arg("custom") || arg("custom_ovb"))) {
            p.rgb_gen = MaterialRgbGen::constant;
            for (std::size_t i = 0; i < 3; ++i)
                p.color[i] = shader_byte(i + 2 < t.size() ? shader_number(t[i + 2]) : 1.0);
        } else if (t.size() == 7 && arg("wave")) {
            if (const auto function = shader_waveform(t[2])) {
                p.rgb_gen = MaterialRgbGen::wave;
                p.rgb_wave = shader_wave(t, 2, *function);
            }
        } else if (t.size() == 2) {
            if (arg("identity") || arg("identitylighting")) {
                p.rgb_gen = MaterialRgbGen::constant;
                p.color[0] = p.color[1] = p.color[2] = 255;
            } else if (arg("entity")) p.rgb_gen = MaterialRgbGen::entity;
            else if (arg("oneminusentity")) p.rgb_gen = MaterialRgbGen::one_minus_entity;
            else if (arg("vertex")) p.rgb_gen = MaterialRgbGen::vertex;
            else if (arg("oneminusvertex")) p.rgb_gen = MaterialRgbGen::one_minus_vertex;
            else if (arg("lightingdiffuse")) p.rgb_gen = MaterialRgbGen::lighting_diffuse;
        }
    } else if (is("alphagen") && t.size() >= 3 && arg("custom")) {
        p.color[3] = shader_byte(shader_number(t[2]));
    } else if (is("tcmod") && t.size() > 1) {
        MaterialTcMod mod;
        std::size_t count{};
        float missing{};
        if (arg("rotate")) { mod.kind = MaterialTcModKind::rotate; count = 1; }
        else if (arg("scale")) { mod.kind = MaterialTcModKind::scale; count = 2; missing = 1; }
        else if (arg("scroll")) { mod.kind = MaterialTcModKind::scroll; count = 2; }
        else if (arg("transform")) { mod.kind = MaterialTcModKind::transform; count = 6; }
        else return; // No runtime turb/stretch handlers were registered.
        for (std::size_t i = 0; i < count; ++i)
            mod.values[i] = i + 2 < t.size() ? shader_float(t[i + 2]) :
                (mod.kind == MaterialTcModKind::transform && i < 4 ? 1.0f : missing);
        if (mod.kind == MaterialTcModKind::rotate) mod.values[0] = -mod.values[0];
        if (mod.kind == MaterialTcModKind::transform) {
            for (std::size_t i : {0U, 2U}) {
                const float length = std::hypot(mod.values[i], mod.values[i + 1]);
                if (length != 0) {
                    mod.values[i] /= length;
                    mod.values[i + 1] /= length;
                }
            }
        }
        shader_add_modifier(p, mod);
    } else if (t.size() == 2) {
        if (is("tcgen")) {
            if (arg("environment")) {
                p.source_mode = MaterialSourceMode::environment;
                p.tc_gen = MaterialTcGen::environment;
            } else if (arg("lightmap")) p.source_mode = MaterialSourceMode::lightmap;
        } else if (is("depthfunc")) {
            if (arg("lequal")) p.depth_function = MaterialDepthFunc::lequal;
            else if (arg("equal")) p.depth_function = MaterialDepthFunc::equal;
        } else if (is("alphafunc")) {
            if (arg("gt0")) p.alpha_test = MaterialAlphaTest::gt0;
            else if (arg("lt128")) p.alpha_test = MaterialAlphaTest::lt128;
            else if (arg("ge128")) p.alpha_test = MaterialAlphaTest::ge128;
            else if (arg("eq0")) p.alpha_test = MaterialAlphaTest::eq0;
        }
    }
}

void shader_process(Material& m) {
    bool opaque = false, writer = false;
    for (auto& p : m.passes) {
        if (p.rgb_gen == MaterialRgbGen::unspecified) p.rgb_gen = MaterialRgbGen::constant;
        p.depth_write = p.depth_write_policy == MaterialDepthWrite::on;
        if (p.depth_write_policy == MaterialDepthWrite::off) continue;
        if (!p.blend) {
            opaque = true;
            if (!writer) { p.depth_write = true; writer = true; }
        } else if (p.depth_write) {
            if (writer) p.depth_write = false;
            writer = true;
        } else if (!p.alpha_test && opaque && !writer) {
            p.depth_write = true;
            writer = true;
        }
    }
    if (!opaque && m.sort == MaterialSort::opaque) m.sort = MaterialSort::translucent;
}

void shader_body(Material& m, std::string_view source, std::size_t position, std::size_t& work) {
    ShaderTokens tokens;
    unsigned depth = 0;
    bool saw_pass_command = false;
    while (shader_line(source, position, tokens, work)) {
        if (tokens.empty()) continue;
        if (tokens[0].starts_with('{')) {
            depth = std::min(depth + 1, 2U);
            if (depth == 2) {
                if (m.passes.size() == shader_item_limit) shader_error("too many shader passes");
                m.passes.emplace_back();
            }
        } else if (tokens[0].starts_with('}')) {
            if (depth == 0 || --depth == 0) break;
        } else if (depth == 1 && !saw_pass_command) {
            shader_material_command(m, tokens);
        } else if (depth == 2) {
            saw_pass_command = true;
            shader_pass_command(m.passes.back(), tokens);
        }
    }
    shader_process(m);
}
} // namespace

std::vector<std::string> AssetStore::names(std::string_view prefix) const {
    std::string key;
    if (!prefix.empty()) {
        const auto trimmed = prefix.find_last_not_of("/\\");
        if (trimmed == prefix.npos) resource_error("asset prefix must be relative");
        key = resource_name(prefix.substr(0, trimmed + 1));
        key.push_back('/');
    }
    std::vector<std::string> names;
    for (const auto& [indexed, relative] : files_) {
        if (!indexed.starts_with(key)) continue;
        const auto resolved = resource_resolve(root_, relative);
        if (fs::is_regular_file(resolved)) names.push_back(relative.generic_string());
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
            [](char x, char y) {
                return static_cast<unsigned char>(resource_lower(x)) <
                    static_cast<unsigned char>(resource_lower(y));
            });
    });
    return names;
}

MaterialLibrary::MaterialLibrary(const AssetStore& assets) {
    if (!assets.contains("script")) return;
    struct ShaderFile {
        std::string name;
        fs::file_time_type time;
    };
    std::vector<ShaderFile> files;
    for (const auto& name : assets.names("script")) {
        // Original script\*.shader does not recurse below the script directory.
        if (name.find('/', 7) != name.npos) continue;
        if (!shader_equal(fs::path(name).extension().string(), ".shader")) continue;
        const auto path = assets.path(name);
        if (files.size() == shader_item_limit) shader_error("too many shader files");
        files.push_back({name, fs::last_write_time(path)});
    }
    // 00432300 compares the high then low DWORD of last-write FILETIME.
    // Timestamp ties have no defined original order (its comparator returns
    // +1 for equality). Native CAB timestamps distinguish the only cross-file
    // duplicate: stock_machine 20:02:00 precedes covered_objects 20:02:02.
    std::stable_sort(files.begin(), files.end(), [](const ShaderFile& a, const ShaderFile& b) {
        return a.time < b.time;
    });
    for (const auto& file : files) parse(assets.text(file.name), file.name);
}

void MaterialLibrary::parse(std::string_view source, std::string_view source_name) {
    if (source.size() > shader_source_limit) shader_error("shader source exceeds limit");
    if (source_name.size() > 8192) shader_error("shader source name exceeds limit");
    clear_runtime_masks();
    ShaderTokens tokens;
    std::size_t position = 0, depth = 0, work = 0;
    while (shader_line(source, position, tokens, work)) {
        if (tokens.empty()) continue;
        if (tokens[0].starts_with('{')) ++depth;
        else if (tokens[0].starts_with('}')) {
            if (depth != 0) --depth;
        } else if (depth == 0) {
            if (definitions_.size() == shader_definition_limit) shader_error("too many shader definitions");
            Material material;
            material.name = tokens[0];
            material.source = source_name;
            shader_body(material, source, position, work);
            auto key = shader_key(material.name);
            const auto index = definitions_.size();
            definitions_.push_back(std::move(material));
            names_.try_emplace(std::move(key), index);
        }
    }
}

const Material* MaterialLibrary::find(std::string_view name) const {
    const auto found = names_.find(shader_key(name));
    return found == names_.end() ? nullptr : &definitions_[found->second];
}

const Material& MaterialLibrary::resolve(std::string_view name, bool object_context) const {
    if (name.empty() || name.size() > 8192 || name.find('\0') != name.npos)
        throw std::invalid_argument("MaterialLibrary: invalid material name");
    auto key = shader_key(name);
    const bool no_shader = key == "noshader";
    const std::string_view canonical_name = no_shader ? "textures/common/caulk" : name;
    const auto authored = no_shader ? names_.find("textures/common/caulk") : names_.find(key);
    if (authored != names_.end()) return definitions_[authored->second];

    auto& cache = object_context ? implicit_object_ : implicit_world_;
    if (const auto found = cache.find(key); found != cache.end()) return found->second;
    Material material;
    material.name = canonical_name;
    // The original tests param_4, not the remapped noshader image, case-sensitively.
    if (object_context || name.find("textures") != name.npos) {
        material.passes.reserve(object_context ? 1 : 2);
        if (!object_context) {
            auto& lightmap = material.passes.emplace_back();
            lightmap.texture.kind = MaterialTextureKind::lightmap;
            lightmap.source_mode = MaterialSourceMode::lightmap;
        }
        auto& image = material.passes.emplace_back();
        image.texture.kind = MaterialTextureKind::image;
        image.texture.image = canonical_name;
        image.source_mode = MaterialSourceMode::image;
        image.rgb_gen = object_context ? MaterialRgbGen::vertex : MaterialRgbGen::constant;
        if (!object_context) {
            image.blend = true;
            image.blend_source = MaterialBlendFactor::dst_color;
            image.blend_destination = MaterialBlendFactor::zero;
        }
    }
    shader_process(material);
    return cache.emplace(std::move(key), std::move(material)).first->second;
}

const Material& MaterialLibrary::construct(std::string_view name,std::uint32_t contents,
                                           std::uint32_t surface_flags,bool object_context) const {
    if(const auto* instance=find_instance(name))return *instance;
    const auto& material=resolve(name,object_context);
    auto mask=0x108000u|((contents&0x10001u)?0x600u:0x400u);
    if(surface_flags&0x10u)mask&=~0x400u;
    if(material.collision_enabled)
        mask=*material.collision_enabled?mask|0x200u:mask&~0x200u;
    if(material.impact_enabled)
        mask=*material.impact_enabled?mask|0x400u:mask&~0x400u;
    if(material.takeable)mask|=0x1000u;
    runtime_masks_.emplace(std::string(name),RuntimeMask{&material,mask});
    return material;
}

std::uint32_t MaterialLibrary::runtime_trace_mask(std::string_view requested_name) const {
    const auto found=runtime_masks_.find(requested_name);
    if(found==runtime_masks_.end())throw std::logic_error("unconstructed runtime shader");
    return found->second.mask;
}

const Material* MaterialLibrary::find_instance(std::string_view requested_name) const noexcept {
    const auto found=runtime_masks_.find(requested_name);
    return found==runtime_masks_.end()?nullptr:found->second.definition;
}

void MaterialLibrary::clear_runtime_masks() const noexcept {
    runtime_masks_.clear();
}

MaterialLibrary::RuntimeMasks MaterialLibrary::take_runtime_masks() const noexcept {
    RuntimeMasks masks;
    masks.swap(runtime_masks_);
    return masks;
}

void MaterialLibrary::swap_runtime_masks(RuntimeMasks& masks) const noexcept {
    runtime_masks_.swap(masks);
}
} // namespace pusu
