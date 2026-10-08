#include "assets.hpp"

#include <bit>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

int main() {
    std::vector<unsigned char> bytes;
    auto u32 = [&](std::uint32_t n) {
        for (unsigned j = 0; j != 4; ++j) bytes.push_back(static_cast<unsigned char>(n >> (8 * j)));
    };
    auto f32 = [&](float n) { u32(std::bit_cast<std::uint32_t>(n)); };
    for (unsigned char c : std::string("PUPM0300")) bytes.push_back(c);
    u32(2); u32(3); u32(0);
    for (float n : {11.f, 12.f, 13.f, 21.f, 22.f, 23.f}) f32(n);
    for (float n : {11.f, 12.f, 13.f, 21.f, 22.f, 23.f}) f32(n);
    for (float n : {.25f, .5f, .75f, 1.f}) f32(n);
    for (float n : {0.f, 1.f, 0.f, 1.f, 0.f, 0.f}) f32(n);
    for (unsigned char c : {0, 0, 1, 0, 0, 0}) bytes.push_back(c);
    const auto path = std::filesystem::temp_directory_path() /
        ("pusu-assets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pm");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    auto save = [&] {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        assert(file.good());
    };
    auto replace_u32 = [&](std::size_t offset, std::uint32_t n) {
        for (unsigned j = 0; j != 4; ++j) bytes[offset + j] = static_cast<unsigned char>(n >> (8 * j));
    };
    save();
    const auto mesh = pusu::read_mesh(path);
    assert(mesh.positions.size() == 2 && mesh.positions[1].x == 21.f);
    assert(mesh.texcoords[0].x == .25f && mesh.texcoords[1].y == 1.f);
    assert(mesh.normals[0].y == 1.f && mesh.normals[1].x == 1.f);
    assert(mesh.indices == std::vector<std::uint16_t>({0, 1, 0}));
    assert(mesh.bounds.minimum.x == 11.f && mesh.bounds.minimum.y == 12.f && mesh.bounds.minimum.z == 13.f);
    assert(mesh.bounds.maximum.x == 21.f && mesh.bounds.maximum.y == 22.f && mesh.bounds.maximum.z == 23.f);
    constexpr std::uint32_t mesh_nan_bits[] = {0xffffffffu, 0xffc23456u, 0x7fc34567u};
    replace_u32(44, mesh_nan_bits[0]);
    replace_u32(68, mesh_nan_bits[1]);
    replace_u32(88, mesh_nan_bits[2]);
    save();
    const auto raw_mesh = pusu::read_mesh(path);
    assert(std::bit_cast<std::uint32_t>(raw_mesh.positions[0].x) == mesh_nan_bits[0]);
    assert(std::bit_cast<std::uint32_t>(raw_mesh.texcoords[0].x) == mesh_nan_bits[1]);
    assert(std::bit_cast<std::uint32_t>(raw_mesh.normals[0].y) == mesh_nan_bits[2]);
    assert(raw_mesh.positions[0].y == 12.f && raw_mesh.positions[0].z == 13.f);
    assert(raw_mesh.positions[1].x == 21.f && raw_mesh.positions[1].y == 22.f && raw_mesh.positions[1].z == 23.f);
    assert(raw_mesh.texcoords[0].y == .5f && raw_mesh.texcoords[1].x == .75f && raw_mesh.texcoords[1].y == 1.f);
    assert(raw_mesh.normals[0].x == 0.f && raw_mesh.normals[0].z == 0.f);
    assert(raw_mesh.normals[1].x == 1.f && raw_mesh.normals[1].y == 0.f && raw_mesh.normals[1].z == 0.f);
    assert(raw_mesh.indices == mesh.indices);
    assert(raw_mesh.bounds.minimum.x == 11.f && raw_mesh.bounds.minimum.y == 12.f && raw_mesh.bounds.minimum.z == 13.f);
    assert(raw_mesh.bounds.maximum.x == 21.f && raw_mesh.bounds.maximum.y == 22.f && raw_mesh.bounds.maximum.z == 23.f);
    bytes[8] = bytes[9] = bytes[10] = bytes[11] = 0xff;
    save();
    bool rejected = false;
    try { (void)pusu::read_mesh(path); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);

    // Genuine PL schema, reproducing askeri_us_1's signed lightmap -3 without
    // depending on shipped media or constructing a mock renderer.
    bytes.clear();
    for (unsigned char c : std::string("PUSU\0\1", 6)) bytes.push_back(c);
    for (float n : {0.f, 0.f, 0.f, 2.f, 2.f, 2.f}) f32(n);
    u32(4); u32(4); u32(4); // Grid spacing, bounds contain one sample.
    u32(1); // Shader table.
    for (unsigned j = 0; j != 64; ++j) bytes.push_back(j == 0 ? 's' : 0);
    u32(0); u32(1);
    for (unsigned j = 0; j != 7; ++j) u32(0); // Planes through brush sides.
    u32(3);
    const auto vertex_offset = bytes.size();
    for (unsigned j = 0; j != 3; ++j) {
        f32(static_cast<float>(j)); f32(0.f); f32(0.f);
        for (unsigned k = 0; k != 4; ++k) f32(0.f);
        f32(0.f); f32(0.f); f32(1.f);
        for (unsigned k = 0; k != 4; ++k) bytes.push_back(255);
    }
    u32(3);
    for (unsigned char c : {0, 0, 1, 0, 2, 0}) bytes.push_back(c);
    u32(1);
    u32(0); u32(1); u32(0); u32(3); u32(0); u32(3);
    const auto lightmap_offset = bytes.size();
    u32(std::bit_cast<std::uint32_t>(std::int32_t(-3)));
    for (unsigned j = 0; j != 7; ++j) u32(0);
    for (unsigned j = 0; j != 6; ++j) f32(0.f);
    f32(0.f); f32(0.f); f32(1.f); u32(0); u32(0);
    u32(0); // No lightmap texture: -3 must not index this table.
    u32(1);
    for (unsigned j = 0; j != 8; ++j) bytes.push_back(0);
    u32(0); u32(0); // No PVS or triggers.
    save();
    const auto level = pusu::read_level(path);
    assert(level.surfaces.size() == 1 && level.surfaces.front().lightmap == -3);
    // WorldVertex is raw IEEE payload; structural bounds remain finite-only.
    constexpr std::uint32_t world_nan_bits[] = {0x7fc12345u, 0xffc23456u, 0x7fc34567u, 0xffc45678u};
    replace_u32(vertex_offset, world_nan_bits[0]);
    replace_u32(vertex_offset + 12, world_nan_bits[1]);
    replace_u32(vertex_offset + 20, world_nan_bits[2]);
    replace_u32(vertex_offset + 36, world_nan_bits[3]);
    save();
    const auto raw_level = pusu::read_level(path);
    const auto& vertex = raw_level.vertices.front();
    assert(std::bit_cast<std::uint32_t>(vertex.position.x) == world_nan_bits[0]);
    assert(std::bit_cast<std::uint32_t>(vertex.texcoord.x) == world_nan_bits[1]);
    assert(std::bit_cast<std::uint32_t>(vertex.lightmap_texcoord.x) == world_nan_bits[2]);
    assert(std::bit_cast<std::uint32_t>(vertex.normal.z) == world_nan_bits[3]);
    replace_u32(6, world_nan_bits[0]);
    save();
    rejected = false;
    try { (void)pusu::read_level(path); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected); // Global bounds still guard structural calculations.
    replace_u32(6, 0);
    for (unsigned j = 0; j != 4; ++j) bytes[lightmap_offset + j] = 0;
    save();
    rejected = false;
    try { (void)pusu::read_level(path); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected); // Nonnegative IDs still require an existing texture.

    // PA frame components retain raw IEEE payloads; counts remain structural.
    bytes.clear();
    u32(100); u32(2); u32(1);
    const std::string bone_name = "Bip01 R Foot";
    u32(static_cast<std::uint32_t>(bone_name.size()));
    bytes.insert(bytes.end(), bone_name.begin(), bone_name.end());
    constexpr std::uint32_t pose_nan_bits = 0xffc00000u;
    constexpr std::uint32_t rotation_nan_bits = 0x7fc12345u;
    u32(pose_nan_bits);
    for (float n : {12.f, 13.f, 21.f, 22.f, 23.f}) f32(n);
    u32(rotation_nan_bits); // Generic raw-copy regression, not a corpus quaternion.
    for (float n : {41.f, 42.f, 43.f, 51.f, 52.f, 53.f, 54.f}) f32(n);
    save();
    const auto animation = pusu::read_animation(path);
    assert(animation.header_word == 100 && animation.frame_count == 2);
    assert(animation.tracks.size() == 1 && animation.tracks[0].name == bone_name);
    assert(animation.tracks[0].frames.size() == 2);
    const auto& pose = animation.tracks[0].frames[0];
    assert(std::bit_cast<std::uint32_t>(pose.position.x) == pose_nan_bits);
    assert(pose.position.y == 12.f && pose.position.z == 13.f);
    assert(pose.rotation.x == 21.f && pose.rotation.y == 22.f && pose.rotation.z == 23.f);
    assert(std::bit_cast<std::uint32_t>(pose.rotation.w) == rotation_nan_bits);
    const auto& next_pose = animation.tracks[0].frames[1];
    assert(next_pose.position.x == 41.f && next_pose.position.y == 42.f && next_pose.position.z == 43.f);
    assert(next_pose.rotation.x == 51.f && next_pose.rotation.y == 52.f &&
           next_pose.rotation.z == 53.f && next_pose.rotation.w == 54.f);
    replace_u32(4, 0xffffffffu);
    save();
    rejected = false;
    try { (void)pusu::read_animation(path); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);

    // PKA fread shares persistent DWORD scratch and name storage across tracks.
    // A short name read at EOF keeps the old NUL and repeats the last quaternion.
    bytes.clear();
    u32(1625); u32(2);
    bytes.push_back(4);
    for (unsigned char c : std::string("abcd")) bytes.push_back(c);
    constexpr std::uint32_t nan_payload = 0x7fc12345;
    u32(nan_payload);
    for (float n : {12.f, 13.f, 21.f, 22.f, 23.f, 31.f, 32.f, 33.f, 34.f,
                    41.f, 42.f, 43.f, 51.f, 52.f, 53.f, 61.f, 62.f, 63.f, 64.f}) f32(n);
    bytes.push_back(8);
    bytes.push_back('X'); bytes.push_back('Y');
    assert(bytes.size() - 8 >= 2 * 40);
    assert(4 * sizeof(pusu::TransformFrame) <= 2 * bytes.size());
    save();
    const auto keyframes = pusu::read_keyframes(path);
    assert(keyframes.header_word == 1625 && keyframes.frame_count == 2);
    assert(keyframes.tracks.size() == 2);
    assert(keyframes.tracks[0].name == "abcd" && keyframes.tracks[1].name == "XYcd");
    assert(keyframes.tracks[0].frames.size() == 2 && keyframes.tracks[1].frames.size() == 2);
    const auto& first = keyframes.tracks[0].frames[0];
    assert(std::bit_cast<std::uint32_t>(first.position.x) == nan_payload);
    assert(first.position.y == 12.f && first.position.z == 13.f);
    assert(first.scale.x == 21.f && first.scale.y == 22.f && first.scale.z == 23.f);
    assert(first.rotation.x == 31.f && first.rotation.y == 32.f &&
           first.rotation.z == 33.f && first.rotation.w == 34.f);
    const auto& last = keyframes.tracks[0].frames[1];
    assert(last.position.x == 41.f && last.position.y == 42.f && last.position.z == 43.f);
    assert(last.scale.x == 51.f && last.scale.y == 52.f && last.scale.z == 53.f);
    assert(last.rotation.x == 61.f && last.rotation.y == 62.f &&
           last.rotation.z == 63.f && last.rotation.w == 64.f);
    for (const auto& frame : keyframes.tracks[1].frames) {
        assert(frame.position.x == last.rotation.x && frame.position.y == last.rotation.y &&
               frame.position.z == last.rotation.z);
        assert(frame.scale.x == last.rotation.x && frame.scale.y == last.rotation.y &&
               frame.scale.z == last.rotation.z);
        assert(frame.rotation.x == last.rotation.x && frame.rotation.y == last.rotation.y &&
               frame.rotation.z == last.rotation.z && frame.rotation.w == last.rotation.w);
    }

    // fread may overwrite only the low bytes of an already initialized DWORD.
    bytes.resize(bytes.size() - 3);
    bytes.push_back(1); bytes.push_back('z');
    bytes.push_back(0xaa); bytes.push_back(0xbb);
    save();
    const auto partial = pusu::read_keyframes(path);
    assert(partial.tracks.size() == 2 && partial.tracks[1].name == "z");
    assert(partial.tracks[1].frames.size() == 2);
    const auto updated_x = (std::bit_cast<std::uint32_t>(last.rotation.x) & 0xffff0000u) | 0xbbaau;
    for (const auto& frame : partial.tracks[1].frames) {
        assert(std::bit_cast<std::uint32_t>(frame.position.x) == updated_x);
        assert(std::bit_cast<std::uint32_t>(frame.scale.x) == updated_x);
        assert(std::bit_cast<std::uint32_t>(frame.rotation.x) == updated_x);
        assert(frame.position.y == 62.f && frame.position.z == 63.f);
        assert(frame.scale.y == 62.f && frame.scale.z == 63.f);
        assert(frame.rotation.y == 62.f && frame.rotation.z == 63.f && frame.rotation.w == 64.f);
    }

    bytes.clear();
    u32(1625); u32(2);
    bytes.push_back(8);
    bytes.push_back('X'); bytes.push_back('Y');
    save();
    rejected = false;
    try { (void)pusu::read_keyframes(path); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected); // No prior initialized name or coordinate scratch; also over budget.

    // Keep the input large enough for frame2 so the budget cannot mask unknown scratch.
    bytes.clear();
    u32(1625); u32(2);
    bytes.push_back(80);
    bytes.insert(bytes.end(), 80, 'a');
    assert(bytes.size() - 8 >= 2 * 40);
    save();
    rejected = false;
    try { (void)pusu::read_keyframes(path); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);

    // Comment-truncated part directives must not become resource names or parts.
    const std::string object_text =
        "{\npart //disabled_mesh //disabled_shader\n}\n"
        "{\npart mesh_black shader_black\n}\n"
        "{\npart mesh_light shader_light\n}\n";
    bytes.assign(object_text.begin(), object_text.end());
    save();
    const auto object = pusu::read_object(path);
    assert(object.parts.size() == 2);
    assert(object.parts[0].mesh == "mesh_black" && object.parts[0].shader == "shader_black");
    assert(object.parts[1].mesh == "mesh_light" && object.parts[1].shader == "shader_light");
    for (const std::string malformed : {
             "{\npart\n}\n",
             "{\npart mesh //missing_shader\n}\n",
             "{\npart \"\" shader\n}\n"}) {
        bytes.assign(malformed.begin(), malformed.end());
        save();
        rejected = false;
        try { (void)pusu::read_object(path); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
}
