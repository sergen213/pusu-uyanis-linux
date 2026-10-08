#pragma once

#include "renderer.hpp"
#include "resources.hpp"
#include "scene_math.hpp"
#include <SDL.h>
#include <numbers>

namespace pusu {
inline constexpr float pi = std::numbers::pi_v<float>;
struct RenderNameHash {
    using is_transparent=void;
    std::size_t operator()(std::string_view s) const noexcept {std::size_t h=1469598103934665603ull;for(unsigned char c:s){if(c>='A'&&c<='Z')c+=32;if(c=='\\')c='/';h=(h^c)*1099511628211ull;}return h;}
};
struct RenderNameEqual {
    using is_transparent=void;
    bool operator()(std::string_view a,std::string_view b)const noexcept {if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i){char x=a[i],y=b[i];if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x=='\\')x='/';if(y=='\\')y='/';if(x!=y)return false;}return true;}
};
struct RenderVertex { Vec3 position; Vec2 uv, light_uv; Vec3 normal; std::array<float,4> color; };
struct GridLighting { Vec3 ambient{1,1,1}, directed{}, direction{0,0,1}; };
struct CullingBounds { Bounds bounds;Vec3 center{},half{};float radius{};unsigned last_plane{3}; };
struct ModelFrustum {
    std::array<Plane,6> planes{};Vec3 origin{};float radius{};
    bool sphere_visible(Vec3 center,float bound_radius) const;
    bool visible(CullingBounds& bounds,const Matrix& owner) const;
    bool visible(const AdvancedBounds& bounds,unsigned& last_plane,std::uint32_t& mask) const;
};
Matrix perspective(float reference,float aspect,float near_plane,float far_plane);
std::int32_t leaf_at(const Level&,Vec3 position);
std::int32_t original_integer(long double value);
int native_bar_height(int width,int height,float fraction);
int avi_atlas_side(int dimension);
CullingBounds cache_bounds(Bounds);
Vec3 original_rotation(const Matrix&,Vec3);
ModelFrustum model_frustum(const RenderCamera&,long double angle,float height_width);
GridLighting grid_sample(const LightGrid&,Vec3 minimum,Vec3 point);
std::array<std::uint8_t,4> grid_diffuse(const LightGrid&,Vec3 minimum,Vec3 point,Vec3 normal);
bool requires_grid_colors(const Material*);
Vec3 normalized_color(const std::uint8_t* rgb,float gain);
void normalize_lightmap(const Lightmap&,std::span<std::uint8_t,128*128*3>);
void reduce_lightmap(std::span<const std::uint8_t,128*128*3>,int divisor,std::span<std::uint8_t>);
std::array<float,9> material_normal_matrix(const Matrix&);
std::array<int,2> interface_extent(int width,int height);
RenderRect interface_rect(const InterfaceQuad&,int width,int height);
Matrix interface_model(const InterfaceQuad&,const RenderRect&);
std::array<std::array<int,4>,4> interface_margin_bands(int width,int height,int ui_width,int ui_height);
RenderVertex from_world(const WorldVertex&);
using SurfacePtr=std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)>;
struct DecodedLayout { int components{3};bool bgr{}; };
SurfacePtr image_surface(AssetStore&,std::string_view name,int diagnostic_size=4,DecodedLayout* layout=nullptr);
SurfacePtr reduce_image(SurfacePtr,int divisor);
SurfacePtr combined_image(AssetStore&,const MaterialTexture&);
void cube_face_bytes(const SDL_Surface&,DecodedLayout,std::span<std::uint8_t> raw);
void avi_frame_bytes(const VideoFrame&,std::vector<std::uint8_t>& rgb);
RenderVertex quadratic(const RenderVertex* controls,float u,float v);
void tessellate(const Level&,const Surface&,std::vector<RenderVertex>&,std::vector<std::uint32_t>&);
float wave_value(const MaterialWave&,double seconds);
Matrix texture_matrix(const MaterialPass&,double seconds,const Matrix& view);
void deform_vertices(const Material&,Primitive,std::span<const std::uint16_t> indices,
                     std::span<const RenderVertex> posed,std::span<RenderVertex> scratch,
                     const Matrix& model,const RenderCamera&,double seconds);
void deform_vertices(const Material&,Primitive,std::span<const std::uint32_t> indices,
                     std::span<const RenderVertex> posed,std::span<RenderVertex> scratch,
                     const Matrix& model,const RenderCamera&,double seconds);
Vec3 cube_capture_center(Vec3 previous,Vec3 contribution);
int effective_texture_filter(int requested,bool mipmaps);
} // namespace pusu
