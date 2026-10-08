#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pusu {

struct Vec2 { float x{}, y{}; };
struct Vec3 { float x{}, y{}, z{}; };
struct Bounds { Vec3 minimum, maximum; };
// Column-major; points are column vectors, translation occupies elements 12..14.
using Matrix = std::array<float, 16>;
enum class Primitive { triangles, triangle_strip };

struct Bone {
    std::string name;
    std::int32_t parent{-1};
    Matrix local_bind{}, global_bind{}, inverse_bind{};
    std::vector<std::uint32_t> vertices;
};
struct Mesh {
    std::string version;
    Primitive primitive{};
    // Authored PM header minimum/maximum, not the resource's derived center/half.
    // Original 00450840 overrides skeletal collision bounds separately; it does
    // not reinterpret these six floats or recompute them from vertex geometry.
    Bounds bounds;
    // Separate on disk, in model-space rest pose. Skin owned vertices with
    // animated_global * inverse_bind; normals use only its upper-left 3x3.
    // PM attributes retain raw IEEE values, including authored NaNs.
    std::vector<Vec3> positions;
    std::vector<Vec2> texcoords;
    std::vector<Vec3> normals;
    std::vector<std::uint16_t> indices;
    std::vector<Bone> bones;
    // -1 means genuinely unowned (omer.pm contains one); never invent a bone.
    std::vector<std::int32_t> vertex_bones;
};

struct Quaternion { float x{}, y{}, z{}, w{}; };
// PA frame components retain raw IEEE values, including authored NaNs.
struct BoneFrame { Vec3 position; Quaternion rotation; };
struct BoneTrack { std::string name; std::vector<BoneFrame> frames; };
struct Animation {
    // Original sample spacing in milliseconds (0044f8b0/00448120);
    // playback duration is (frame_count - 1) * header_word, with a 1 ms minimum.
    std::uint32_t header_word{}, frame_count{};
    std::vector<BoneTrack> tracks;
};
struct TransformFrame { Vec3 position, scale; Quaternion rotation; };
struct TransformTrack { std::string name; std::vector<TransformFrame> frames; };
struct Keyframes {
    // Original 0041d8b0 takes playback duration in milliseconds from header_word;
    // frame_count is the number of transform samples in each track.
    // Original fread semantics preserve raw IEEE values and initialized scratch
    // across short reads, including nonfinite authored transform components.
    std::uint32_t header_word{}, frame_count{};
    std::vector<TransformTrack> tracks;
};
struct ObjectPart { std::string mesh, shader; };
struct Attachment {
    std::string object;
    std::optional<std::string> bone;
    std::optional<Matrix> transform;
};
struct Object {
    std::optional<float> damage, damage_radius, health;
    std::optional<std::string> blow_sound, animation_set;
    std::vector<ObjectPart> parts;
    std::vector<Attachment> attachments;
};

struct AdvancedBounds {
    Bounds bounds;
    Vec3 center;
    std::uint32_t type{};
    float radius{};
};
struct Shader {
    std::string name;
    std::uint32_t surface_flags{}, contents{};
};
// Classify a point using dot(normal, point) - distance.
struct Plane { Vec3 normal; float distance{}; };
struct Node {
    std::uint32_t plane{};
    // Nonnegative: node ordinal; negative: leaf ordinal = bitwise complement.
    // children[0] is front (distance > -0.001f), children[1] is back.
    std::array<std::int32_t, 2> children{};
    AdvancedBounds bounds;
};
struct Leaf {
    std::int32_t cluster{}, area{};
    AdvancedBounds bounds;
    std::uint32_t first_surface{}, surface_count{}, first_brush{}, brush_count{};
};
struct Brush { std::uint32_t first_side{}, side_count{}, shader{}; };
struct BrushSide { std::uint32_t plane{}, shader{}; };
struct WorldVertex {
    // Raw IEEE float payload, including NaNs, is preserved for consumers, not count/index data.
    Vec3 position;
    Vec2 texcoord, lightmap_texcoord;
    Vec3 normal;
    // Serialized RGB retained, original renderer ignores disk alpha.
    std::array<std::uint8_t, 4> color{};
};
enum class SurfaceType : std::uint32_t { planar = 1, patch = 2, triangle_soup = 3 };
struct Surface {
    std::uint32_t shader{};
    SurfaceType type{};
    std::uint32_t first_vertex{}, vertex_count{}, first_index{}, index_count{};
    // Original signed lighting selector: negative values (shipped maps use -3)
    // are not lightmap texture-array indices; preserve them without remapping.
    std::int32_t lightmap{};
    Bounds patch_bounds;
    Vec3 normal;
    std::uint32_t patch_width{}, patch_height{};
};
using Lightmap = std::array<std::uint8_t, 128 * 128 * 3>;
struct LightSample {
    std::array<std::uint8_t, 3> ambient{}, directed{};
    // Original direction=(cos(e)*cos(a),cos(e)*sin(a),sin(e));
    // angles are encoded in 0..255 representing 0..360 degrees.
    std::uint8_t elevation{}, azimuth{};
};
struct LightGrid {
    std::array<std::uint32_t, 3> spacing{}, dimensions{};
    // Original loader stores ceil(minimum/spacing), not world-unit translation.
    std::array<std::int32_t, 3> origin_cells{};
    std::array<std::uint32_t, 3> edge_padding{};
    // x-fastest: x + dimensions[0] * (y + dimensions[1] * z).
    std::vector<LightSample> samples;
};
struct Visibility {
    std::uint32_t cluster_count{}, bytes_per_cluster{};
    // Exact stored PVS bytes. Original exporter may omit up to eight final
    // bytes; visible() conservatively returns true for unavailable bits.
    std::vector<std::uint8_t> bits;
    bool visible(std::uint32_t from_cluster, std::uint32_t to_cluster) const;
};
struct Trigger {
    std::string name;
    std::uint32_t brush{};
    Bounds bounds;
    float repeat_interval_seconds{};
};
struct Level {
    Bounds bounds;
    std::vector<Shader> shaders;
    std::vector<Plane> planes;
    std::vector<Node> nodes;
    std::vector<Leaf> leaves;
    std::vector<std::uint32_t> leaf_surfaces, leaf_brushes;
    std::vector<Brush> brushes;
    std::vector<BrushSide> brush_sides;
    std::vector<WorldVertex> vertices;
    // Indices are relative to each surface's first_vertex.
    std::vector<std::uint16_t> indices;
    std::vector<Surface> surfaces;
    std::vector<Lightmap> lightmaps;
    LightGrid light_grid;
    Visibility visibility;
    std::vector<Trigger> triggers;
};

// Optional synchronous loading callbacks: 0.1 after successful file open, before
// sizing/bulk input; 0.2 after the lightmap count, before typed pixel allocation
// and decoding (bulk input is already buffered); 0.3 after light-grid derived
// values/validation and visibility payload decoding, before the trigger count.
Level read_level(const std::filesystem::path& path, void* context = nullptr,
                 void (*progress)(void*, float) = nullptr);
Mesh read_mesh(const std::filesystem::path& path);
Animation read_animation(const std::filesystem::path& path);
Keyframes read_keyframes(const std::filesystem::path& path);
Object read_object(const std::filesystem::path& path);

} // namespace pusu
