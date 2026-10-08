#undef NDEBUG // This standalone boundary check remains active in release builds.
#include "resources.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <openssl/evp.h>
#include <stdexcept>
#include <string_view>

namespace {
std::vector<std::uint8_t> unhex(std::string_view hex) {
    auto digit = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    return result;
}
void save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("fixture write failed");
}
template<class F> void rejected(F&& f) {
    bool failed = false;
    try { f(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}
}

int main(int argc, char** argv) {
    const auto temporary = std::filesystem::temp_directory_path() /
        ("pusu-resources-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temporary / "Script");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{temporary};
    // Genuine three records from the original 3te_common.shader; the non-content
    // package index is omitted, with its marker and enclosing footer retained.
    const auto original = unhex(
        "00544d53414d564f48a49bfdff2624e9d7f1d6f0d6aebefcd6dfb5c1d01f07ceefeeddde4ff1d1aebea49bfdff2621eccef1d6f1d6aebe01001400"
        "11a628c7b0c72f3ca0beb8e687e3426facd4cf5e02000000016a0000006a0000008c010000"
        "658ffcff18d21dc4e677d37feabd74a8078b2e76e4796aec05c74d8a7d6b034da8aac6184147895946b49bd40ee98e5c8c4b534888fe764d4f2e260ee54197b6a55b2d99d1f9f826ea31a13a81593b5437d11f6999e6e22fd9ab1521f9205bb5aca7ff4bfbeea09b09fc"
        "a49bfdff2624e9d7f1d6f0d6aebefdd6dfbac1d9184dccf4a681cb40d3b1d1aebea49bfdff2621eccef1d6f1d6aebe01001400"
        "a464533019709392db9a04f248dc5b3c9e77708500000000019e000000ea0000005b040000"
        "789c558d510a83301044ff3d450e201a8342c95fcfd05e604db7b2756396240df4f68d08a5c2c21b66e0ed9d3cde3278b1ca683df57aeccd4599c18e53bde6ea05dc0a0b465530260a9b5553370edd60065d57a63987c04a452c74ccffad038199983261b20a7c79a616bc8b1fc99582912b981c6e096bc2cd85c71e5e5060ff8d87e66cffb567bbe093185bc185df3b38b8b5c2052f11536abe0cf75040"
        "a49bfdff2624e9d7f1d6f0d6aebefdd6dfb5c1d01f07ceefeec6d15ec893d1aebea49bfdff2621eccef1d6f1d6aebe01001400"
        "f825c7f9e4d4b80d8547767ee66d95fc65e798d900000000012f000000290000007d020000"
        "789c53322e498d4fcecfcdcdcfd32bce484c492d525250d24fcecf2b49cd2bd12f4a2c07720da28d62950028fd0d63"
        "a49bfdff262be4c3f1d6f0d6aebe544d53414d564f46010000000000540200000000000000000000");
    const auto fixture = temporary / "Script/3te_common.shader";
    save(fixture, original);
    std::filesystem::create_directories(temporary / "Models");
    // Same two-vertex PUPM0300 fixture as checks/assets.cpp.
    const auto pm = unhex(
        "5055504d30333030"
        "020000000300000000000000"
        "0000304100004041000050410000a8410000b0410000b841"
        "0000304100004041000050410000a8410000b0410000b841"
        "0000803e0000003f0000403f0000803f"
        "000000000000803f000000000000803f0000000000000000"
        "000001000000");
    const auto pm_fixture = temporary / "Models/check.pm";
    save(pm_fixture, pm);
    pusu::AssetStore store(temporary);
    const std::string expected = "\r\n\r\n3te_objects/cizilmeyen_obje\r\n{\r\n\r\n\t// TEXTURE\r\n\tcull front\r\n\tlightgrid off\r\n\tsurfaceparm nonsolid\r\n}\r\n";
    assert(store.text("SCRIPT\\3TE_COMMON.SHADER") == expected);
    assert(store.contains("SCRIPT\\3TE_COMMON.SHADER") && !store.contains("script/missing.shader"));
    assert((store.names("sCrIpT\\") == std::vector<std::string>{"Script/3te_common.shader"}));
    rejected([&] { (void)store.names("../"); });
    rejected([&] { (void)store.bytes("../Script/3te_common.shader"); });
    rejected([&] { (void)store.path("C:\\Script\\3te_common.shader"); });
    assert(store.contains_optional_game_file("SCRIPT\\3TE_COMMON.SHADER"));
    assert(!store.contains_optional_game_file("script/missing.shader"));
    for (const char punctuation : std::string_view("?*\"<>|")) {
        const auto candidate = "script/3te_common" + std::string(1, punctuation) + ".shader";
        assert(!store.contains_optional_game_file(candidate));
        rejected([&] { (void)store.contains(candidate); });
        rejected([&] { (void)store.path(candidate); });
        rejected([&] { (void)store.text(candidate); });
        rejected([&] { (void)pusu::read_runtime_mesh(store, candidate); });
    }
    assert(!store.contains_optional_game_file("SCRIPT\\3TE_*.SHADER"));
    for (const std::string_view candidate : {
             "", "/mesh.pm", "\\mesh.pm", "C:\\mesh.pm", "C:mesh.pm",
             "../mesh.pm", "dir/../mesh.pm", "dir\\..\\mesh.pm", "./mesh.pm",
             "dir//mesh.pm", "dir\\\\mesh.pm", "dir/", "dir\\",
             "dir./mesh.pm", "dir /mesh.pm", "mesh.pm.", "mesh.pm ",
             "/?mesh.pm", "\\?mesh.pm", "C:\\?mesh.pm", "?dir/C:mesh.pm",
             "?dir/../mesh.pm", "?dir\\..\\mesh.pm", "?dir/./mesh.pm",
             "?dir//mesh.pm", "?dir\\\\mesh.pm", "?dir/", "?dir\\",
             "?dir./mesh.pm", "?dir /mesh.pm", "?dir/mesh.pm.", "?dir/mesh.pm "}) {
        rejected([&] { (void)store.contains_optional_game_file(candidate); });
        rejected([&] { (void)store.contains(candidate); });
        rejected([&] { (void)store.path(candidate); });
        rejected([&] { (void)store.text(candidate); });
        rejected([&] { (void)pusu::read_runtime_mesh(store, candidate); });
    }
    // A punctuation marker must not hide a later NUL/control-byte violation.
    for (unsigned control = 0; control <= 127; ++control) {
        if (control >= 32 && control != 127) continue;
        for (const std::string_view prefix : {"script/bad", "script/bad?"}) {
            const auto candidate = std::string(prefix) + static_cast<char>(control) + "name.pm";
            rejected([&] { (void)store.contains_optional_game_file(candidate); });
            rejected([&] { (void)store.contains(candidate); });
            rejected([&] { (void)store.path(candidate); });
            rejected([&] { (void)store.text(candidate); });
            rejected([&] { (void)pusu::read_runtime_mesh(store, candidate); });
        }
    }
    auto corrupt = original;
    corrupt[96] ^= 1; // Consumer sees an integrity failure, never corrupted text.
    save(fixture, corrupt);
    rejected([&] { (void)store.text("script/3te_common.shader"); });
    corrupt = original;
    for (unsigned i = 0; i < 4; ++i) corrupt[84 + i] = 0xff;
    save(fixture, corrupt);
    rejected([&] { (void)store.bytes("script/3te_common.shader"); });
    save(fixture, original);

    assert(!pusu::read_runtime_mesh(store, "MODELS\\MISSING.PM"));
    const auto mesh = pusu::read_runtime_mesh(store, "MODELS\\CHECK.PM");
    assert(mesh && mesh->version == "0300" && mesh->primitive == pusu::Primitive::triangle_strip);
    assert(mesh->positions.size() == 2 && mesh->positions[1].x == 21.f);
    assert(mesh->texcoords.size() == 2 && mesh->texcoords[0].x == .25f && mesh->texcoords[1].y == 1.f);
    assert(mesh->normals.size() == 2 && mesh->normals[0].y == 1.f && mesh->normals[1].x == 1.f);
    assert((mesh->indices == std::vector<std::uint16_t>{0, 1, 0}));
    assert(mesh->bounds.minimum.x == 11.f && mesh->bounds.minimum.y == 12.f &&
           mesh->bounds.minimum.z == 13.f);
    assert(mesh->bounds.maximum.x == 21.f && mesh->bounds.maximum.y == 22.f &&
           mesh->bounds.maximum.z == 23.f && mesh->bones.empty() && mesh->vertex_bones.empty());
    auto corrupt_pm = pm;
    for (unsigned i = 0; i < 4; ++i) corrupt_pm[8 + i] = 0xff;
    save(pm_fixture, corrupt_pm);
    assert(store.contains("models/check.pm"));
    rejected([&] { (void)pusu::read_runtime_mesh(store, "models/check.pm"); });
    save(pm_fixture, pm);

    // An optional indexed hit is not permission to read a replaced escaping link.
    pusu::AssetStore mesh_root(temporary / "Models");
    assert(mesh_root.contains_optional_game_file("CHECK.PM"));
    std::filesystem::remove(pm_fixture);
    std::filesystem::create_symlink(fixture, pm_fixture);
    assert(mesh_root.contains_optional_game_file("CHECK.PM"));
    rejected([&] { (void)mesh_root.path("check.pm"); });
    rejected([&] { (void)mesh_root.text("check.pm"); });
    rejected([&] { (void)pusu::read_runtime_mesh(mesh_root, "check.pm"); });

    pusu::MaterialLibrary grammar;
    grammar.parse(
        "Check/Surface\n{\n"
        "surfaceparm playerclip\nsurfaceparm nonsolid\ncull front\npolygonoffset\n"
        "{\nanimmap 2 a.tga b.tga\ntcgen environment\nmap diffuse.tga\n"
        "blendfunc blend\nblendfunc gl_one gl_src_alpha\n"
        "tcmod scale 2\ntcmod turb 0 1 0 1\nrgbgen custom .5 .25 .1\n"
        "rgbgen wave sin 1 1 1 .2 0 3\nalphagen custom .2\nalphafunc .8\n}\n"
        "lightgrid off\n{\nmap $lightmap\ndepthwrite\n}\n}\n"
        "}\ncheck/surface\n{\n{\nmap wrong.tga\n}\n}\n", "boundary.shader");
    assert(grammar.definition_count() == 2);
    const auto* material = grammar.find("CHECK/SURFACE");
    assert(material && material->collision_enabled == false);
    assert(material->cull == pusu::MaterialCull::front);
    assert(material->lightgrid == pusu::MaterialLightGrid::interpolate);
    assert(material->polygon_offset == -2.f && material->reflection == .5f);
    assert(material->passes.size() == 2);
    const auto& first = material->passes[0];
    assert(first.texture.image == "diffuse.tga");
    assert(first.animation && first.animation->frequency == 2.f);
    assert((first.animation->frames == std::vector<std::string>{"a.tga", "b.tga"}));
    assert(first.source_mode == pusu::MaterialSourceMode::image &&
           first.tc_gen == pusu::MaterialTcGen::environment);
    assert(material->passes[1].source_mode == pusu::MaterialSourceMode::lightmap);
    assert(first.blend_source == pusu::MaterialBlendFactor::one &&
           first.blend_destination == pusu::MaterialBlendFactor::src_alpha);
    assert(first.tc_mods.size() == 1 && first.tc_mods[0].values[0] == 2.f &&
           first.tc_mods[0].values[1] == 1.f);
    assert((first.color == std::array<std::uint8_t, 4>{127, 63, 25, 51}));
    assert(!first.alpha_test && !first.depth_write && material->passes[1].depth_write);

    assert(grammar.find("CHECK\\SURFACE") == nullptr);
    assert(&grammar.resolve("CHECK/SURFACE") == material);
    assert(&grammar.resolve("check/surface", false) == material);

    pusu::MaterialLibrary authored;
    authored.parse(
        "textures/common/caulk\n{\n}\n"
        "noshader\n{\n{\nmap ignored.tga\n}\n}\n"
        "Textures/Authored\n{\n{\nmap actual.tga\n}\n}\n"
        "Invisible\n{\n}\n");
    const auto* caulk = authored.find("TEXTURES/COMMON/CAULK");
    assert(caulk && caulk->passes.empty());
    assert(&authored.resolve("NoShader") == caulk);
    assert(&authored.resolve("NOSHADER", false) == caulk);
    assert(authored.find("noshader")->passes.size() == 1);
    assert(&authored.resolve("textures/authored", false) == authored.find("Textures/Authored"));
    assert(authored.resolve("textures/authored").passes[0].texture.image == "actual.tga");
    assert(&authored.resolve("INVISIBLE") == authored.find("invisible"));
    assert(authored.resolve("invisible", false).passes.empty());
    assert(authored.definition_count() == 4);

    const pusu::MaterialLibrary implicit;
    const auto& object = implicit.resolve("textures/check.tga");
    assert(object.name == "textures/check.tga" && object.passes.size() == 1);
    const auto& object_pass = object.passes[0];
    assert(object_pass.texture.kind == pusu::MaterialTextureKind::image &&
           object_pass.texture.image == object.name &&
           object_pass.texture.wrap == pusu::TextureWrap::repeat);
    assert(object_pass.source_mode == pusu::MaterialSourceMode::image &&
           object_pass.rgb_gen == pusu::MaterialRgbGen::vertex);
    assert(!object_pass.blend && object_pass.depth_test && object_pass.depth_write &&
           object.sort == pusu::MaterialSort::opaque);
    const auto& world = implicit.resolve("textures/check.tga", false);
    assert(&world != &object && world.passes.size() == 2);
    const auto& lightmap = world.passes[0];
    const auto& image = world.passes[1];
    assert(lightmap.texture.kind == pusu::MaterialTextureKind::lightmap &&
           lightmap.source_mode == pusu::MaterialSourceMode::lightmap &&
           lightmap.rgb_gen == pusu::MaterialRgbGen::constant &&
           !lightmap.blend && lightmap.depth_write && lightmap.depth_test);
    assert(image.texture.kind == pusu::MaterialTextureKind::image &&
           image.texture.image == world.name && image.texture.wrap == pusu::TextureWrap::repeat &&
           image.source_mode == pusu::MaterialSourceMode::image &&
           image.rgb_gen == pusu::MaterialRgbGen::constant);
    assert((image.color == std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    assert(image.blend && image.blend_source == pusu::MaterialBlendFactor::dst_color &&
           image.blend_destination == pusu::MaterialBlendFactor::zero &&
           !image.depth_write && image.depth_test && world.sort == pusu::MaterialSort::opaque);
    assert(implicit.resolve("objects/check.tga", false).passes.empty());
    assert(implicit.resolve("objects/check.tga", false).sort == pusu::MaterialSort::translucent);
    assert(implicit.resolve("objects/check.tga").passes.size() == 1);
    assert(implicit.resolve("Textures/uppercase.tga", false).passes.empty());
    assert(implicit.resolve("objects/withtextures.tga", false).passes.size() == 2);
    const auto& no_shader_object = implicit.resolve("NoShader");
    const auto& no_shader_world = implicit.resolve("NOSHADER", false);
    assert(no_shader_object.name == "textures/common/caulk" &&
           no_shader_object.passes[0].texture.image == "textures/common/caulk");
    assert(no_shader_world.name == "textures/common/caulk" && no_shader_world.passes.empty());
    const auto& caulk_world = implicit.resolve("textures/common/caulk", false);
    assert(&caulk_world != &no_shader_world && caulk_world.passes.size() == 2);
    assert(&implicit.resolve("noshader") == &no_shader_object);
    assert(&implicit.resolve("noshader", false) == &no_shader_world);
    for (unsigned i = 0; i < 512; ++i) {
        const auto name = "textures/cache-growth-" + std::to_string(i);
        (void)implicit.resolve(name);
        (void)implicit.resolve(name, false);
    }
    assert(&implicit.resolve("TEXTURES/CHECK.TGA") == &object);
    assert(&implicit.resolve("TEXTURES/CHECK.TGA", false) == &world);
    assert(&object.passes[0] == &object_pass && &world.passes[1] == &image);
    assert(object_pass.texture.image == "textures/check.tga" && image.blend);
    assert(implicit.find("textures/check.tga") == nullptr && implicit.definitions().empty() &&
           implicit.definition_count() == 0);
    bool empty_rejected = false;
    try { (void)implicit.resolve(""); }
    catch (const std::invalid_argument&) { empty_rejected = true; }
    assert(empty_rejected);
    rejected([&] { (void)implicit.resolve(std::string_view("bad\0name", 8)); });
    rejected([&] { (void)implicit.resolve(std::string(8193, 'a')); });
    assert(implicit.resolve(std::string(8192, 'a')).name.size() == 8192);

    // Optional full original-data run: compare every recovered shader/UI byte
    // against independently decoded golden files, then parse all shaders.
    if (argc == 3) {
        pusu::AssetStore game(argv[1]), golden(argv[2]);
        pusu::MaterialLibrary materials;
        std::size_t compared = 0, shaders = 0;
        for (const auto& directory : {"script", "interface"}) {
            for (const auto& item : std::filesystem::directory_iterator(golden.root() / directory)) {
                if (!item.is_regular_file()) continue;
                const auto name = std::string(directory) + "/" + item.path().filename().string();
                const auto decoded = game.bytes(name);
                assert(decoded == golden.bytes(name));
                std::array<unsigned char, 32> digest{};
                unsigned length = 0;
                assert(EVP_Digest(decoded.data(), decoded.size(), digest.data(), &length,
                                  EVP_sha256(), nullptr) == 1 && length == digest.size());
                for (const auto byte : digest)
                    std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
                std::cout << std::dec << "  " << name << '\n';
                ++compared;
                if (item.path().extension() == ".shader") {
                    materials.parse({reinterpret_cast<const char*>(decoded.data()), decoded.size()}, name);
                    ++shaders;
                }
            }
        }
        assert(compared == 96 && shaders == 87 && materials.definition_count() == 2965);
        std::cout << "Compared " << compared << " files; parsed " << shaders << " scripts, "
                  << materials.definition_count() << " material definitions\n";
    } else if (argc != 1) {
        throw std::runtime_error("usage: checks_resources [ORIGINAL_GAME_ROOT GOLDEN_PLAINTEXT_ROOT]");
    }
}
