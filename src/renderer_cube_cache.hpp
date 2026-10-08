#pragma once

#include "assets.hpp"
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <unistd.h>

namespace pusu {
class AssetStore;
class CubeCacheValidationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct CubeCacheEntry { Vec3 center;std::uint32_t offset; };
struct CubeCache { std::vector<CubeCacheEntry> entries;std::vector<std::uint8_t> rgb; };
std::uint32_t cube_read_word(std::span<const std::uint8_t>,std::size_t offset);
std::vector<CubeCacheEntry> cube_cache_entries(std::span<const std::uint8_t>);
int cube_cache_quality(std::string_view marker);
std::optional<CubeCache> read_cube_cache(AssetStore&,std::string_view cache_base,int reflection_divisor);
std::uint32_t cube_cache_offset(const CubeCache&,Vec3 center);
void validate_cube_cache_geometry(const CubeCache&);
std::size_t cube_cache_region_bytes(const CubeCache&,std::uint32_t offset);
std::span<const std::uint8_t> cube_cache_faces_at_offset(const CubeCache&,std::uint32_t offset,int side);
std::span<const std::uint8_t> cube_cache_faces(const CubeCache&,Vec3 center,int side);
std::span<const std::uint8_t> owned_cube_cache_faces(const CubeCache&,std::size_t index,Vec3 center,int side);
namespace cube_cache_detail {
struct Fd {
    int value{-1};
    Fd() = default;
    explicit Fd(int fd):value(fd) {}
    ~Fd() noexcept { if(value>=0)::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept:value(std::exchange(other.value,-1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if(this!=&other){if(value>=0)::close(value);value=std::exchange(other.value,-1);}return *this;
    }
};
}
class CubeCacheOutput {
public:
    ~CubeCacheOutput() noexcept;
    CubeCacheOutput(const CubeCacheOutput&) = delete;
    CubeCacheOutput& operator=(const CubeCacheOutput&) = delete;
    CubeCacheOutput(CubeCacheOutput&&) noexcept;
    CubeCacheOutput& operator=(CubeCacheOutput&&) = delete;
    void begin_cube(Vec3 center);
    void face(std::span<const std::uint8_t> rgb);
    void commit(int divisor);
private:
    friend class PrivateCubeCache;
    CubeCacheOutput(cube_cache_detail::Fd parent,cube_cache_detail::Fd lock,
                    const std::string& leaf_name,std::string_view basename);
    cube_cache_detail::Fd parent_,lock_,final_,stage_;
    std::array<cube_cache_detail::Fd,3> files_;
    std::string pending_;
    std::array<std::string,3> names_;
    std::uint64_t offset_{},info_size_{};
    std::size_t face_size_{};
    unsigned faces_{};
    bool has_cube_{},stage_owned_{},committed_{};
    void require_open() const;
    void write_record(std::array<std::uint32_t,4> words);
    void cleanup() noexcept;
};
class PrivateCubeCache {
public:
    PrivateCubeCache(std::filesystem::path cache_root,std::string_view normalized_extensionless_bsp,std::string_view shape);
    PrivateCubeCache(const PrivateCubeCache&) = delete;
    PrivateCubeCache& operator=(const PrivateCubeCache&) = delete;
    PrivateCubeCache(PrivateCubeCache&&) = default;
    PrivateCubeCache& operator=(PrivateCubeCache&&) = default;
    std::optional<CubeCache> read(int reflection_divisor);
    CubeCacheOutput begin_output();
private:
    std::filesystem::path root_,level_,dir_;
    std::string leaf_name_,basename_;
    cube_cache_detail::Fd open_parent(bool create) const;
    cube_cache_detail::Fd open_lock(int parent,bool create,bool* created=nullptr) const;
};
} // namespace pusu
