#pragma once

#include "assets.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>

namespace pusu {

// Original shader registry names fold ASCII case, not path separators.
struct ShaderNameHash {
    using is_transparent=void;
    std::size_t operator()(std::string_view s) const noexcept {
        std::size_t h=1469598103934665603ull;
        for(unsigned char c:s) {
            if(c>='A'&&c<='Z')c+=32;
            h=(h^c)*1099511628211ull;
        }
        return h;
    }
};
struct ShaderNameEqual {
    using is_transparent=void;
    bool operator()(std::string_view a,std::string_view b) const noexcept {
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i) {
            char x=a[i],y=b[i];
            if(x>='A'&&x<='Z')x+=32;
            if(y>='A'&&y<='Z')y+=32;
            if(x!=y)return false;
        }
        return true;
    }
};

// Indexed Windows-style names; only relative entries physically contained in root.
class AssetStore {
public:
    explicit AssetStore(std::filesystem::path root);
    const std::filesystem::path& root() const noexcept { return root_; }
    bool contains(std::string_view logical_name) const;
    // Original optional opens: safe non-filename identifiers are unavailable.
    bool contains_optional_game_file(std::string_view logical_name) const;
    // File names recursively under a relative directory; empty prefix means root.
    std::vector<std::string> names(std::string_view prefix) const;
    std::filesystem::path path(std::string_view logical_name) const;
    std::vector<std::uint8_t> bytes(std::string_view logical_name) const;
    std::string text(std::string_view logical_name) const;
private:
    struct NameHash {
        using is_transparent = void;
        static char fold(char c) noexcept {
            if (c == '\\') return '/';
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        }
        std::size_t operator()(std::string_view name) const noexcept {
            std::size_t hash = 1469598103934665603ull;
            for (char c : name) hash = (hash ^ static_cast<unsigned char>(fold(c))) * 1099511628211ull;
            return hash;
        }
    };
    struct NameEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (NameHash::fold(a[i]) != NameHash::fold(b[i])) return false;
            return true;
        }
    };
    std::filesystem::path root_;
    std::unordered_map<std::string, std::filesystem::path, NameHash, NameEqual> files_;
};

// Missing PM retains its logical resource/part, but has no geometry. Present
// files still use the strict path and decoder boundary; no placeholder mesh.
std::optional<Mesh> read_runtime_mesh(const AssetStore&, std::string_view logical_pm_path);

enum class Waveform { sin, triangle, square, sawtooth, inverse_sawtooth };
struct MaterialWave {
    Waveform function{Waveform::sin};
    float base{}, amplitude{}, phase{}, frequency{};
};
enum class MaterialCull { none, front, back };
enum class MaterialLightGrid { off, center, interpolate, interpolate_once };
enum class MaterialSort : std::uint32_t {
    opaque = 0x100000, translucent = 0x400000,
    sky = 0x800000, sky_alternate = 0x1000000
};
enum class MaterialSky { none, normal, alternate };
enum class TextureWrap { repeat, clamp, clamp_to_edge };
enum class MaterialTextureKind { none, image, lightmap, white, cube_capture, cube_faces, combined, avi };
enum class MaterialSourceMode { none, image, lightmap, environment, cube };
struct CombinedTextureLayer {
    std::string name, image;
    std::int32_t x{}, y{};
};
struct MaterialTexture {
    MaterialTextureKind kind{MaterialTextureKind::none};
    TextureWrap wrap{TextureWrap::repeat};
    std::string image, fallback_image;
    std::int32_t cube_size{64};
    // Original order: +X, -X, +Y, -Y, +Z, -Z.
    std::array<std::string, 6> cube_faces;
    std::int32_t width{}, height{};
    std::vector<CombinedTextureLayer> layers;
};
struct MaterialAnimation {
    float frequency{};
    std::vector<std::string> frames;
    TextureWrap wrap{TextureWrap::repeat};
};
enum class MaterialBlendFactor {
    zero, one, src_color, one_minus_src_color, dst_color, one_minus_dst_color,
    src_alpha, one_minus_src_alpha
};
enum class MaterialDepthFunc { lequal, equal };
enum class MaterialAlphaTest { gt0, lt128, ge128, eq0 };
enum class MaterialRgbGen {
    unspecified, constant, wave, entity, one_minus_entity, vertex, one_minus_vertex, lighting_diffuse
};
enum class MaterialTcGen { base, environment, cube };
enum class MaterialTcModKind { rotate, scale, scroll, transform, cube, avi };
struct MaterialTcMod {
    MaterialTcModKind kind{MaterialTcModKind::scale};
    // rotate: negative degrees/sec; scale/scroll: first two values;
    // transform: normalized m00,m01,m10,m11, translation S,T.
    std::array<float, 6> values{};
};
enum class MaterialDeformKind { wave, autosprite, autosprite2 };
struct MaterialDeform {
    MaterialDeformKind kind{MaterialDeformKind::wave};
    float spread{};
    MaterialWave wave;
};
enum class MaterialDepthWrite { automatic, on, off };
struct MaterialPass {
    MaterialTexture texture;
    // Original animation descriptor survives later texture and tcgen commands.
    std::optional<MaterialAnimation> animation;
    // Binding source is independent of retained texture payload and sticky tc_gen.
    MaterialSourceMode source_mode{MaterialSourceMode::none};
    bool blend{}, depth_test{true}, depth_write{};
    MaterialDepthWrite depth_write_policy{MaterialDepthWrite::automatic};
    MaterialBlendFactor blend_source{MaterialBlendFactor::one}, blend_destination{MaterialBlendFactor::zero};
    MaterialDepthFunc depth_function{MaterialDepthFunc::lequal};
    std::optional<MaterialAlphaTest> alpha_test;
    MaterialRgbGen rgb_gen{MaterialRgbGen::unspecified};
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    MaterialWave rgb_wave;
    MaterialTcGen tc_gen{MaterialTcGen::base};
    std::vector<MaterialTcMod> tc_mods;
};
struct Material {
    std::string name, physical_material_name, source;
    MaterialCull cull{MaterialCull::back};
    MaterialLightGrid lightgrid{MaterialLightGrid::interpolate};
    MaterialSort sort{MaterialSort::opaque};
    MaterialSky sky{MaterialSky::none};
    bool mipmaps{true}, picmip{true}, compression{true}, fog{true}, takeable{};
    // nullopt inherits compiled PL flags: collision from contents & 0x10001,
    // impact permission from !(surface_flags & 0x10). Commands override in order.
    std::optional<bool> collision_enabled, impact_enabled;
    // Authored "reflection" is particle-bounce restitution/damping (default .5),
    // not optical reflectance or Fresnel/PBR data. Traced original +0x98 reads:
    // collision 0044b224 -> 0044bd20; proof: .work/native-gl-gaps/reflection.
    float polygon_offset{}, reflection{0.5f};
    std::vector<MaterialDeform> deforms;
    // Zero passes is an authored invisible material, not a missing texture.
    std::vector<MaterialPass> passes;
};
class MaterialLibrary {
public:
    struct RuntimeMask {
        const Material* definition{};
        std::uint32_t mask{};
    };
    using RuntimeMasks=std::unordered_map<std::string,RuntimeMask,ShaderNameHash,ShaderNameEqual>;
    MaterialLibrary() = default;
    explicit MaterialLibrary(const AssetStore& assets);
    void parse(std::string_view source, std::string_view source_name = {});
    // Original first case-insensitive definition wins; no implicit fallback.
    const Material* find(std::string_view name) const;
    // Canonical authored material or context-specific implicit material.
    // Authored references, like find(), may be invalidated by parse(); implicit
    // references survive cache growth for the lifetime of this library.
    const Material& resolve(std::string_view name, bool object_context = true) const;
    // Original 0040db40 caches the requested name before noshader remapping;
    // The first actual constructor context owns its material and trace flags
    // for this epoch. As with resolve(), parse() invalidates authored references.
    const Material& construct(std::string_view name,std::uint32_t contents=0,
                              std::uint32_t surface_flags=0,bool object_context=true) const;
    std::uint32_t runtime_trace_mask(std::string_view requested_name) const;
    // Read-only first constructed instance, or null if this epoch has none.
    const Material* find_instance(std::string_view requested_name) const noexcept;
    void clear_runtime_masks() const noexcept;
    RuntimeMasks take_runtime_masks() const noexcept;
    void swap_runtime_masks(RuntimeMasks&) const noexcept;
    const std::vector<Material>& definitions() const noexcept { return definitions_; }
    std::size_t definition_count() const noexcept { return definitions_.size(); }
private:
    std::vector<Material> definitions_;
    std::unordered_map<std::string, std::size_t> names_;
    mutable std::unordered_map<std::string, Material> implicit_world_, implicit_object_;
    mutable RuntimeMasks runtime_masks_;
};

} // namespace pusu
