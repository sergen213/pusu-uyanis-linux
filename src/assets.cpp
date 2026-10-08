#include "assets.hpp"
#include "scene_math.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <span>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pusu {
namespace {

class Reader {
    std::vector<unsigned char> data_;
    std::size_t offset_{};
public:
    explicit Reader(const std::filesystem::path& path, void* context = nullptr,
                    void (*progress)(void*, float) = nullptr) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open asset: " + path.string());
        if (progress) progress(context, 0.1f);
        const auto length = file.tellg();
        if (length < 0 || static_cast<std::uintmax_t>(length) > data_.max_size() ||
            static_cast<std::uintmax_t>(length) > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            fail("Invalid asset size");
        data_.resize(static_cast<std::size_t>(length));
        file.seekg(0);
        if (!data_.empty() && !file.read(reinterpret_cast<char*>(data_.data()), static_cast<std::streamsize>(data_.size())))
            fail("Cannot read complete asset");
    }
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(offset_));
    }
    std::size_t remaining() const { return data_.size() - offset_; }
    void require(std::size_t count, std::size_t width = 1) const {
        if (width == 0 || count > remaining() / width) fail("Truncated asset or malformed count");
    }
    std::uint8_t u8() { require(1); return data_[offset_++]; }
    std::uint16_t u16() { const auto a = u8(); return static_cast<std::uint16_t>(a | (std::uint16_t(u8()) << 8)); }
    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (unsigned j = 0; j != 4; ++j) value |= std::uint32_t(data_[offset_++]) << (8 * j);
        return value;
    }
    std::int32_t i32() { return std::bit_cast<std::int32_t>(u32()); }
    float f32() {
        const auto value = std::bit_cast<float>(u32());
        if (!std::isfinite(value)) fail("Nonfinite asset coordinate");
        return value;
    }
    Vec3 vec3() { const auto x = f32(), y = f32(); return {x, y, f32()}; }
    float ieee_f32() { return std::bit_cast<float>(u32()); }
    Vec2 ieee_vec2() { const auto x = ieee_f32(); return {x, ieee_f32()}; }
    Vec3 ieee_vec3() { const auto x = ieee_f32(), y = ieee_f32(); return {x, y, ieee_f32()}; }
    Bounds bounds() { const auto low = vec3(); return {low, vec3()}; }
    Matrix matrix() { Matrix result; for (auto& value : result) value = f32(); return result; }
    Quaternion ieee_quaternion() { const auto x = ieee_f32(), y = ieee_f32(), z = ieee_f32(); return {x, y, z, ieee_f32()}; }
    AdvancedBounds advanced_bounds() {
        const auto box = bounds();
        const auto center = vec3();
        const auto type = u32();
        return {box, center, type, f32()};
    }
    std::string fixed_name(std::size_t size) {
        auto value = text(size);
        const auto terminator = value.find('\0');
        if (terminator == std::string::npos) fail("Unterminated fixed asset name");
        value.resize(terminator);
        return value;
    }
    void bytes(std::span<std::uint8_t> destination) {
        require(destination.size());
        std::copy_n(data_.data() + offset_, destination.size(), destination.data());
        offset_ += destination.size();
    }
    // fread copies even a partial item, leaving every unavailable byte untouched.
    std::size_t partial_bytes(std::span<std::uint8_t> destination) {
        const auto count = std::min(destination.size(), remaining());
        if (count != 0) std::copy_n(data_.data() + offset_, count, destination.data());
        offset_ += count;
        return count;
    }
    std::string text(std::size_t size) {
        require(size);
        if (size == 0) return {};
        std::string value(reinterpret_cast<const char*>(data_.data() + offset_), size);
        offset_ += size;
        return value;
    }
    std::string name32() {
        auto result = text(u32());
        if (result.find('\0') != std::string::npos) fail("Embedded NUL in asset name");
        return result;
    }
    void magic(std::string_view expected) { if (text(expected.size()) != expected) fail("Unsupported asset magic/version"); }
    void eof() const { if (remaining() != 0) fail("Unexpected trailing asset bytes"); }
    template<class T, class F> std::vector<T> records(std::uint32_t count, std::size_t width, F decode) {
        require(count, width);
        std::vector<T> values;
        values.reserve(count);
        for (std::uint32_t j = 0; j != count; ++j) values.push_back(decode());
        return values;
    }
};

} // namespace

bool Visibility::visible(std::uint32_t from_cluster, std::uint32_t to_cluster) const {
    if (from_cluster >= cluster_count || to_cluster >= cluster_count ||
        bytes_per_cluster < (std::uint64_t(cluster_count) + 7) / 8)
        throw std::runtime_error("Invalid PVS cluster or row dimensions");
    const auto byte = std::uint64_t(from_cluster) * bytes_per_cluster + to_cluster / 8;
    // The original exporter truncates up to eight final matrix bytes. Unknown
    // visibility must not hide genuine geometry: conservatively do not cull.
    if (byte >= bits.size()) return true;
    return (bits[static_cast<std::size_t>(byte)] & (1u << (to_cluster & 7))) != 0;
}

Level read_level(const std::filesystem::path& path, void* context, void (*progress)(void*, float)) {
    Reader r(path, context, progress);
    r.magic(std::string_view("PUSU\0\1", 6));
    Level level;
    level.bounds = r.bounds();
    for (auto& spacing : level.light_grid.spacing) {
        spacing = r.u32();
        if (spacing == 0) r.fail("Zero PL light-grid spacing");
    }
    level.shaders = r.records<Shader>(r.u32(), 72, [&] {
        auto name = r.fixed_name(64);
        const auto flags = r.u32();
        return Shader{std::move(name), flags, r.u32()};
    });
    level.planes = r.records<Plane>(r.u32(), 16, [&] {
        const auto normal = r.vec3();
        return Plane{normal, r.f32()};
    });
    level.nodes = r.records<Node>(r.u32(), 56, [&] {
        Node node;
        node.plane = r.u32();
        for (auto& child : node.children) child = r.i32();
        node.bounds = r.advanced_bounds();
        return node;
    });
    level.leaves = r.records<Leaf>(r.u32(), 68, [&] {
        Leaf leaf;
        leaf.cluster = r.i32();
        leaf.area = r.i32();
        leaf.bounds = r.advanced_bounds();
        leaf.first_surface = r.u32();
        leaf.surface_count = r.u32();
        leaf.first_brush = r.u32();
        leaf.brush_count = r.u32();
        return leaf;
    });
    level.leaf_surfaces = r.records<std::uint32_t>(r.u32(), 4, [&] { return r.u32(); });
    level.leaf_brushes = r.records<std::uint32_t>(r.u32(), 4, [&] { return r.u32(); });
    level.brushes = r.records<Brush>(r.u32(), 12, [&] {
        const auto first = r.u32(), count = r.u32();
        return Brush{first, count, r.u32()};
    });
    level.brush_sides = r.records<BrushSide>(r.u32(), 8, [&] {
        const auto plane = r.u32();
        return BrushSide{plane, r.u32()};
    });
    // Original loader copies raw 44-byte vertex records; count/index trust guards remain.
    level.vertices = r.records<WorldVertex>(r.u32(), 44, [&] {
        WorldVertex vertex;
        vertex.position = r.ieee_vec3();
        vertex.texcoord = r.ieee_vec2();
        vertex.lightmap_texcoord = r.ieee_vec2();
        vertex.normal = r.ieee_vec3();
        r.bytes(vertex.color);
        return vertex;
    });
    level.indices = r.records<std::uint16_t>(r.u32(), 2, [&] { return r.u16(); });
    level.surfaces = r.records<Surface>(r.u32(), 100, [&] {
        Surface surface;
        surface.shader = r.u32();
        const auto type = r.u32();
        if (type < 1 || type > 3) r.fail("Unsupported PL surface type");
        surface.type = static_cast<SurfaceType>(type);
        surface.first_vertex = r.u32();
        surface.vertex_count = r.u32();
        surface.first_index = r.u32();
        surface.index_count = r.u32();
        surface.lightmap = r.i32();
        // Every original surface constructor ignores these seven exporter
        // words (+28..52). Consume their known layout without inventing meaning.
        for (unsigned j = 0; j != 7; ++j) (void)r.u32();
        surface.patch_bounds = r.bounds();
        surface.normal = r.vec3();
        surface.patch_width = r.u32();
        surface.patch_height = r.u32();
        return surface;
    });
    const auto lightmaps = r.u32();
    if (progress) progress(context, 0.2f);
    r.require(lightmaps, 128 * 128 * 3);
    level.lightmaps.resize(lightmaps);
    for (auto& map : level.lightmaps) r.bytes(map);
    level.light_grid.samples = r.records<LightSample>(r.u32(), 8, [&] {
        LightSample sample;
        r.bytes(sample.ambient);
        r.bytes(sample.directed);
        sample.elevation = r.u8();
        sample.azimuth = r.u8();
        return sample;
    });
    const std::array<float, 3> minimum{level.bounds.minimum.x, level.bounds.minimum.y, level.bounds.minimum.z};
    const std::array<float, 3> maximum{level.bounds.maximum.x, level.bounds.maximum.y, level.bounds.maximum.z};
    std::uint64_t grid_size = 1;
    for (unsigned j = 0; j != 3; ++j) {
        const auto spacing = static_cast<double>(level.light_grid.spacing[j]);
        const auto low = std::ceil(static_cast<double>(minimum[j]) / spacing);
        const auto high = std::floor(static_cast<double>(maximum[j]) / spacing);
        const auto dimension = high - low + 1;
        if (dimension < 1 || dimension > std::numeric_limits<std::uint32_t>::max() ||
            low < std::numeric_limits<std::int32_t>::min() || low > std::numeric_limits<std::int32_t>::max())
            r.fail("Invalid PL light-grid bounds");
        const auto count = static_cast<std::uint32_t>(dimension);
        if (grid_size > level.light_grid.samples.size() / count) r.fail("PL light-grid dimensions exceed samples");
        grid_size *= count;
        level.light_grid.dimensions[j] = count;
        level.light_grid.origin_cells[j] = static_cast<std::int32_t>(low);
        const auto extent = std::trunc(static_cast<double>(maximum[j]) - minimum[j]);
        if (extent < 0 || extent > std::numeric_limits<std::uint32_t>::max())
            r.fail("Invalid PL light-grid extent");
        level.light_grid.edge_padding[j] = level.light_grid.spacing[j] -
            static_cast<std::uint32_t>(extent) % level.light_grid.spacing[j];
    }
    if (grid_size != level.light_grid.samples.size()) r.fail("PL light-grid sample count mismatch");
    const auto visibility_words = r.u32();
    r.require(visibility_words, 12);
    if (visibility_words != 0) {
        const auto byte_count = std::size_t(visibility_words) * 12;
        if (byte_count < 8) r.fail("Truncated PL visibility header");
        level.visibility.cluster_count = r.u32();
        level.visibility.bytes_per_cluster = r.u32();
        const auto clusters = level.visibility.cluster_count, row = level.visibility.bytes_per_cluster;
        if (clusters == 0 || row < (std::uint64_t(clusters) + 7) / 8)
            r.fail("Invalid PL visibility dimensions");
        const auto declared = std::uint64_t(clusters) * row;
        const auto stored = byte_count - 8;
        if (stored > declared || declared - stored > 8) r.fail("Inconsistent PL visibility byte count");
        level.visibility.bits.resize(stored);
        r.bytes(level.visibility.bits);
    }
    if (progress) progress(context, 0.3f);
    level.triggers = r.records<Trigger>(r.u32(), 96, [&] {
        auto name = r.fixed_name(64);
        const auto brush = r.u32();
        const auto bounds = r.bounds();
        return Trigger{std::move(name), brush, bounds, r.f32()};
    });
    r.eof();
    auto range = [&](std::uint32_t first, std::uint32_t count, std::size_t size) {
        if (first > size || count > size - first) r.fail("PL record range outside referenced array");
    };
    for (const auto& node : level.nodes) {
        if (node.plane >= level.planes.size()) r.fail("PL node plane out of range");
        for (auto child : node.children) {
            if (child >= 0) {
                if (static_cast<std::uint32_t>(child) >= level.nodes.size()) r.fail("PL node child out of range");
            } else if (static_cast<std::uint32_t>(~child) >= level.leaves.size()) r.fail("PL node leaf out of range");
        }
    }
    for (const auto& leaf : level.leaves) {
        if (leaf.cluster < -1 || (leaf.cluster >= 0 && static_cast<std::uint32_t>(leaf.cluster) >= level.visibility.cluster_count))
            r.fail("PL leaf cluster out of range");
        range(leaf.first_surface, leaf.surface_count, level.leaf_surfaces.size());
        range(leaf.first_brush, leaf.brush_count, level.leaf_brushes.size());
    }
    for (auto surface : level.leaf_surfaces) if (surface >= level.surfaces.size()) r.fail("PL leaf surface out of range");
    for (auto brush : level.leaf_brushes) if (brush >= level.brushes.size()) r.fail("PL leaf brush out of range");
    for (const auto& brush : level.brushes) {
        range(brush.first_side, brush.side_count, level.brush_sides.size());
        if (brush.shader >= level.shaders.size()) r.fail("PL brush shader out of range");
    }
    for (const auto& side : level.brush_sides)
        if (side.plane >= level.planes.size() || side.shader >= level.shaders.size()) r.fail("PL brush-side reference out of range");
    for (const auto& surface : level.surfaces) {
        if (surface.shader >= level.shaders.size()) r.fail("PL surface shader out of range");
        // Original 0044aac0/0044ab20 retain a signed value; 00409844's JL
        // skips the lightmap texture table for every negative value, including -3.
        if (surface.lightmap >= 0 && static_cast<std::uint32_t>(surface.lightmap) >= level.lightmaps.size())
            r.fail("PL surface lightmap out of range");
        range(surface.first_vertex, surface.vertex_count, level.vertices.size());
        range(surface.first_index, surface.index_count, level.indices.size());
        for (std::size_t j = surface.first_index; j != std::size_t(surface.first_index) + surface.index_count; ++j)
            if (level.indices[j] >= surface.vertex_count) r.fail("PL surface-local index out of range");
        if (surface.type == SurfaceType::patch &&
            (surface.patch_width < 3 || surface.patch_height < 3 ||
             surface.patch_width % 2 == 0 || surface.patch_height % 2 == 0 ||
             std::uint64_t(surface.patch_width) * surface.patch_height != surface.vertex_count))
            r.fail("Invalid PL quadratic patch dimensions");
    }
    for (const auto& trigger : level.triggers) if (trigger.brush >= level.brushes.size()) r.fail("PL trigger brush out of range");
    // Reject cyclic node graphs without recursive traversal of untrusted depth.
    std::vector<std::uint8_t> state(level.nodes.size());
    std::vector<std::pair<std::size_t, bool>> stack;
    for (std::size_t start = 0; start != level.nodes.size(); ++start) {
        if (state[start] != 0) continue;
        stack.emplace_back(start, false);
        while (!stack.empty()) {
            const auto [index, exiting] = stack.back();
            stack.pop_back();
            if (exiting) { state[index] = 2; continue; }
            if (state[index] == 1) r.fail("Cyclic PL BSP nodes");
            if (state[index] == 2) continue;
            state[index] = 1;
            stack.emplace_back(index, true);
            for (auto child : level.nodes[index].children)
                if (child >= 0) stack.emplace_back(static_cast<std::size_t>(child), false);
        }
    }
    return level;
}

Mesh read_mesh(const std::filesystem::path& path) {
    Reader r(path);
    r.magic("PUPM");
    Mesh mesh;
    mesh.version = r.text(4);
    if (mesh.version == "0200" || mesh.version == "0500") mesh.primitive = Primitive::triangles;
    else if (mesh.version == "0300" || mesh.version == "0600") mesh.primitive = Primitive::triangle_strip;
    else r.fail("Unsupported PM version (engine accepts only 0200/0300/0500/0600)");
    const auto vertices = r.u32(), indices = r.u32(), bones = r.u32();
    // Resource constructor 00450840 passes these endpoints to 0041a3a0,
    // which derives center=(minimum+maximum)/2 and half=maximum-center.
    mesh.bounds = r.bounds();
    r.require(vertices, 32);
    // Original PM loader copies the separated attribute arrays as raw DWORDs.
    mesh.positions = r.records<Vec3>(vertices, 12, [&] { return r.ieee_vec3(); });
    mesh.texcoords = r.records<Vec2>(vertices, 8, [&] { return r.ieee_vec2(); });
    mesh.normals = r.records<Vec3>(vertices, 12, [&] { return r.ieee_vec3(); });
    mesh.indices = r.records<std::uint16_t>(indices, 2, [&] { return r.u16(); });
    for (auto index : mesh.indices) if (index >= vertices) r.fail("PM index outside vertex array");
    r.require(bones, 76);
    mesh.bones.reserve(bones);
    std::vector<std::string> parents;
    parents.reserve(bones);
    std::unordered_map<std::string_view, std::int32_t> by_name;
    for (std::uint32_t j = 0; j != bones; ++j) {
        Bone bone;
        bone.name = r.name32();
        if (bone.name.empty() || j > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            by_name.contains(bone.name)) r.fail("Invalid or duplicate PM bone name");
        const auto owned = r.u32();
        bone.local_bind = r.matrix();
        parents.push_back(r.name32());
        // Actual old-version compatibility rule in the original loader.
        if (!parents.back().empty() && (mesh.version == "0200" || mesh.version == "0300")) {
            if (bone.name == "Bip01 Spine") parents.back() = "Bip01";
            else if (bone.name == "Bip01 L Thigh" || bone.name == "Bip01 R Thigh") parents.back() = "Bip01 Pelvis";
        }
        bone.vertices = r.records<std::uint32_t>(owned, 4, [&] { return r.u32(); });
        mesh.bones.push_back(std::move(bone));
        by_name.emplace(mesh.bones.back().name, static_cast<std::int32_t>(j));
    }
    r.eof();
    if (!mesh.bones.empty()) mesh.vertex_bones.assign(vertices, -1);
    for (std::size_t j = 0; j != mesh.bones.size(); ++j) {
        auto& bone = mesh.bones[j];
        if (!parents[j].empty()) {
            const auto found = by_name.find(parents[j]);
            if (found == by_name.end()) r.fail("Missing PM parent bone");
            bone.parent = found->second;
        }
        for (auto index : bone.vertices) {
            if (index >= vertices) r.fail("PM bone vertex outside vertex array");
            if (mesh.vertex_bones[index] != -1) r.fail("PM vertex owned by multiple bones");
            mesh.vertex_bones[index] = static_cast<std::int32_t>(j);
        }
    }
    // Iterative parent walks bound stack usage even for a corrupt deep hierarchy.
    std::vector<std::uint8_t> state(bones);
    std::vector<std::size_t> chain;
    for (std::size_t j = 0; j != mesh.bones.size(); ++j) {
        chain.clear();
        auto current = static_cast<std::int32_t>(j);
        while (current >= 0 && state[static_cast<std::size_t>(current)] == 0) {
            state[static_cast<std::size_t>(current)] = 1;
            chain.push_back(static_cast<std::size_t>(current));
            current = mesh.bones[static_cast<std::size_t>(current)].parent;
        }
        if (current >= 0 && state[static_cast<std::size_t>(current)] == 1) r.fail("Cyclic PM bone hierarchy");
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            auto& bone = mesh.bones[*it];
            bone.global_bind = bone.parent < 0 ? bone.local_bind :
                multiply(mesh.bones[static_cast<std::size_t>(bone.parent)].global_bind, bone.local_bind);
            bone.inverse_bind = inverse_rigid(bone.global_bind);
            state[*it] = 2;
        }
    }
    // Original loader's four-vertex triangle-list repair and UV ordering.
    if (vertices == 4 && mesh.primitive == Primitive::triangles) {
        if (indices < 6) r.fail("PM quad has fewer than six indices");
        const std::array<std::uint16_t, 6> quad{0, 1, 2, 0, 2, 3};
        std::copy(quad.begin(), quad.end(), mesh.indices.begin());
        std::swap(mesh.texcoords[2], mesh.texcoords[3]);
    }
    return mesh;
}

Animation read_animation(const std::filesystem::path& path) {
    Reader r(path);
    Animation animation;
    animation.header_word = r.u32();
    animation.frame_count = r.u32();
    const auto count = r.u32();
    r.require(count, 4);
    animation.tracks.reserve(count);
    for (std::uint32_t j = 0; j != count; ++j) {
        BoneTrack track;
        track.name = r.name32();
        // Original PA loader copies raw 28-byte frames: position and quaternion.
        track.frames = r.records<BoneFrame>(animation.frame_count, 28, [&] {
            const auto position = r.ieee_vec3();
            return BoneFrame{position, r.ieee_quaternion()};
        });
        animation.tracks.push_back(std::move(track));
    }
    // 0044f8f0 closes after its declared tracks. Eleven shipped PA assets append
    // an exact duplicate stream; original gameplay ignores all trailing bytes.
    return animation;
}

Keyframes read_keyframes(const std::filesystem::path& path) {
    Reader r(path);
    Keyframes keyframes;
    if (r.remaining() < 8) r.fail("Uninitialized original PKA header bytes");
    const auto input_size = r.remaining();
    keyframes.header_word = r.u32();
    keyframes.frame_count = r.u32();
    if (keyframes.frame_count > r.remaining() / 40)
        r.fail("PKA frame count exceeds encoded payload budget");
    if (input_size > std::numeric_limits<std::size_t>::max() / 2)
        r.fail("PKA decoded allocation budget overflows");
    // Complete tracks consume 40 bytes/frame; at most one EOF track repeats
    // scratch. Bound decoded frame storage, not the original unchecked count.
    auto frame_bytes_left = input_size * 2;
    std::array<std::uint8_t, 256> name_bytes{};
    std::array<bool, 256> name_known{};
    std::array<std::array<std::uint8_t, 4>, 4> scratch{};
    std::array<std::array<bool, 4>, 4> scratch_known{};
    const auto read_scratch = [&](std::size_t count) {
        for (std::size_t j = 0; j != count; ++j)
            std::fill_n(scratch_known[j].begin(), r.partial_bytes(scratch[j]), true);
    };
    const auto value = [&](std::size_t j) {
        std::uint32_t bits = 0;
        for (unsigned byte = 0; byte != 4; ++byte) {
            if (!scratch_known[j][byte]) r.fail("Uninitialized original PKA coordinate scratch");
            bits |= std::uint32_t(scratch[j][byte]) << (8 * byte);
        }
        return std::bit_cast<float>(bits); // Preserve raw IEEE bits, including NaNs.
    };
    std::uint8_t name_length{};
    // 0041d8b0 tests only the next length read; short names/coordinates persist.
    while (r.partial_bytes(std::span(&name_length, 1)) != 0) {
        TransformTrack track;
        std::fill_n(name_known.begin(),
                    r.partial_bytes(std::span(name_bytes).first(name_length)), true);
        name_bytes[name_length] = 0;
        name_known[name_length] = true;
        std::size_t name_size = 0;
        for (;;) {
            if (!name_known[name_size]) r.fail("Uninitialized original PKA name scratch");
            if (name_bytes[name_size] == 0) break;
            ++name_size;
        }
        track.name.assign(reinterpret_cast<const char*>(name_bytes.data()), name_size);
        if (keyframes.frame_count > track.frames.max_size() ||
            keyframes.frame_count > frame_bytes_left / sizeof(TransformFrame))
            r.fail("PKA decoded frames exceed bounded allocation budget");
        frame_bytes_left -= std::size_t(keyframes.frame_count) * sizeof(TransformFrame);
        track.frames.reserve(keyframes.frame_count);
        for (std::uint32_t frame = 0; frame != keyframes.frame_count; ++frame) {
            read_scratch(3);
            const Vec3 position{value(0), value(1), value(2)};
            read_scratch(3);
            const Vec3 scale{value(0), value(1), value(2)};
            read_scratch(4);
            track.frames.push_back({position, scale, {value(0), value(1), value(2), value(3)}});
        }
        if (keyframes.tracks.size() == keyframes.tracks.max_size())
            r.fail("PKA track allocation exceeds maximum size");
        keyframes.tracks.push_back(std::move(track));
    }
    return keyframes;
}

Object read_object(const std::filesystem::path& path) {
    Reader r(path);
    const auto data = r.text(r.remaining());
    if (data.find('\0') != std::string::npos) r.fail("Embedded NUL in PO text");
    Object object;
    std::size_t depth = 0, offset = 0;
    auto number = [&](std::string_view token) {
        if (!token.empty() && (token.back() == 'f' || token.back() == 'F')) token.remove_suffix(1);
        if (!token.empty() && token.front() == '+') token.remove_prefix(1);
        float value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(value))
            r.fail("Invalid PO numeric value");
        return value;
    };
    while (offset < data.size()) {
        const auto end = data.find('\n', offset);
        const std::string_view line(data.data() + offset, (end == std::string::npos ? data.size() : end) - offset);
        offset = end == std::string::npos ? data.size() : end + 1;
        std::vector<std::string_view> tokens;
        bool comment_truncated = false;
        for (std::size_t pos = 0; pos < line.size();) {
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
            if (pos == line.size()) break;
            if (line.substr(pos, 2) == "//") { comment_truncated = true; break; }
            if (line[pos] == '"') {
                const auto start = ++pos;
                const auto close = line.find('"', start);
                if (close == std::string_view::npos) r.fail("Unterminated PO quoted name");
                tokens.push_back(line.substr(start, close - start));
                pos = close + 1;
            } else {
                const auto start = pos;
                while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
                tokens.push_back(line.substr(start, pos - start));
            }
        }
        if (tokens.empty()) continue;
        const auto command = tokens.front();
        if (command.starts_with("{")) { ++depth; continue; }
        if (command.starts_with("}")) {
            if (depth != 0) --depth;
            continue;
        }
        if (depth != 0) {
            if (command == "part") {
                // Original 004517bd reads an undefined missing operand, not an ignore branch.
                // Approved native safety extension: omit only comment-empty parts.
                if (comment_truncated && tokens.size() == 1) continue;
                if (tokens.size() != 3 || tokens[1].empty() || tokens[2].empty()) r.fail("Invalid PO part");
                object.parts.push_back({std::string(tokens[1]), std::string(tokens[2])});
            }
            continue;
        }
        if (command == "damage" || command == "damage_radius" || command == "health") {
            if (tokens.size() != 2) r.fail("Invalid PO numeric directive");
            const auto value = number(tokens[1]);
            if (command == "damage") object.damage = value;
            else if (command == "damage_radius") object.damage_radius = value;
            else object.health = value;
        } else if (command == "blow_sound" || command == "anim_set") {
            if (tokens.size() != 2 || tokens[1].empty()) r.fail("Invalid PO resource directive");
            if (command == "blow_sound") object.blow_sound = std::string(tokens[1]);
            else object.animation_set = std::string(tokens[1]);
        } else if (command == "boneattachment" || command == "attachment") {
            const auto base = command == "boneattachment" ? std::size_t(3) : std::size_t(2);
            if (tokens.size() < base || tokens[1].empty())
                r.fail("Invalid PO attachment");
            Attachment attachment;
            attachment.object = tokens[1];
            if (base == 3) {
                if (tokens[2].empty()) r.fail("Empty PO attachment bone");
                attachment.bone = std::string(tokens[2]);
            }
            if (tokens.size() == base + 16) {
                Matrix transform;
                for (std::size_t j = 0; j != 16; ++j) transform[j] = number(tokens[base + j]);
                attachment.transform = transform;
            }
            // Original accepts surplus operands but only applies exactly sixteen
            // matrix values; chr_dummy_01..04 contain six ignored legacy values.
            object.attachments.push_back(std::move(attachment));
        }
        // Original parser ignores unknown lines, including no_weapon.PO's
        // leading shader identifier; no guessed resource/texture is generated.
    }
    // Original allows EOF inside a part block (col_alt_kol_sisman.PO) and
    // ignores unmatched closing braces; preserve those shipped semantics.
    return object;
}

} // namespace pusu
