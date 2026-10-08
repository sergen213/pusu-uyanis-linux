#include "renderer_shared.hpp"
#include "renderer_cube_cache.hpp"
#include <SDL_image.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pusu {
using Vertex=RenderVertex;
using Lighting=GridLighting;
Matrix perspective(float reference, float aspect, float near_plane, float far_plane) {
    if (!(reference > 0 && reference < 180 && aspect > 0 && near_plane > 0 && far_plane > near_plane))
        throw std::runtime_error("Invalid rendering projection");
    float y = 1.0f / (std::tan(reference*pi/360.0f) * 0.75f);
    Matrix result{}; result[0]=y/aspect; result[5]=y;
    result[10]=-(far_plane+near_plane)/(far_plane-near_plane); result[11]=-1;
    result[14]=-2*far_plane*near_plane/(far_plane-near_plane); return result;
}
std::int32_t leaf_at(const Level& level, Vec3 position) {
    if (level.nodes.empty()) return level.leaves.empty() ? -1 : 0;
    std::int32_t index=0;
    for(std::size_t visited=0;index>=0 && visited<=level.nodes.size();++visited) {
        if(static_cast<std::size_t>(index)>=level.nodes.size()) throw std::runtime_error("BSP node outside level");
        const auto& node=level.nodes[index]; const auto& plane=level.planes.at(node.plane);
        index=node.children[dot(plane.normal,position)-plane.distance > -0.001f ? 0 : 1];
    }
    if(index>=0) throw std::runtime_error("Cyclic BSP visibility tree");
    return ~index;
}
// Original x87 00467dbc returns an indefinite int64 on invalid input; its
// callers consume EAX. Preserve that low DWORD without undefined C++ casts.
std::int32_t original_integer(long double value) {
    if(!std::isfinite(value)||value>=0x1p63L||value< -0x1p63L)return 0;
    return std::bit_cast<std::int32_t>(std::uint32_t(std::uint64_t(std::int64_t(value))));
}
int native_bar_height(int width,int height,float fraction) {
    return std::max(0,original_integer((static_cast<long double>(height)-width*static_cast<long double>(0.540499985f))*0.5L*fraction));
}
int avi_atlas_side(int dimension) {
    if(dimension<=0)throw std::runtime_error("Original AVI texture has invalid decoded dimensions");
    // 00401419 JBE skips dimension <= candidate: exact powers are retained.
    for(int side=512;side;side/=2)if(side<dimension)return side*2;
    return 1;
}
CullingBounds cache_bounds(Bounds bounds) {
    CullingBounds result;result.bounds=bounds;result.center=(bounds.minimum+bounds.maximum)*0.5f;result.half=bounds.maximum-result.center;
    result.radius=float(std::sqrt(original_dot(result.half,result.half)));return result;
}
Vec3 original_rotation(const Matrix& m,Vec3 p) {
    return {(m[8]*p.z+m[4]*p.y)+m[0]*p.x,(m[1]*p.x+m[9]*p.z)+m[5]*p.y,(m[2]*p.x+m[10]*p.z)+m[6]*p.y};
}
constexpr unsigned plane_order[6][6]{{0,3,2,1,4,5},{1,3,2,0,4,5},{2,3,0,1,4,5},{3,2,0,1,4,5},{4,3,2,0,1,5},{5,3,2,0,1,4}};
bool ModelFrustum::sphere_visible(Vec3 center,float bound_radius) const {
        Vec3 delta=origin-center;float squared=float(original_dot(delta,delta));long double sum=static_cast<long double>(radius)+bound_radius;
        return !(static_cast<long double>(squared)>sum*float(sum));
    }
bool ModelFrustum::visible(CullingBounds& bounds,const Matrix& owner) const {
        Vec3 center=original_rotation(owner,bounds.center)+Vec3{owner[12],owner[13],owner[14]};
        if(!sphere_visible(center,bounds.radius))return false;
        Vec3 x{owner[0],owner[1],owner[2]},y{owner[4],owner[5],owner[6]},z{owner[8],owner[9],owner[10]};
        for(unsigned id:plane_order[bounds.last_plane]){const auto& plane=planes[id];
            float ax=std::abs(float(original_dot(plane.normal,x))),ay=std::abs(float(original_dot(plane.normal,y))),az=std::abs(float(original_dot(plane.normal,z)));
            long double support=(static_cast<long double>(ax)*bounds.half.x+static_cast<long double>(az)*bounds.half.z)+static_cast<long double>(ay)*bounds.half.y;
            if(original_plane_distance(center,plane)>support){bounds.last_plane=id;return false;}
        }return true;
    }
bool ModelFrustum::visible(const AdvancedBounds& bounds,unsigned& last_plane,std::uint32_t& mask) const {
        if(!mask)return true;
        if(!sphere_visible(bounds.center,bounds.radius))return false;
        for(unsigned id:plane_order[last_plane]){if(!(mask&(1u<<id)))continue;const auto& p=planes[id];
            if(original_plane_distance(bounds.center,p)>bounds.radius){last_plane=id;return false;}
            Vec3 minimum{p.normal.x>0?bounds.bounds.minimum.x:bounds.bounds.maximum.x,p.normal.y>0?bounds.bounds.minimum.y:bounds.bounds.maximum.y,p.normal.z>0?bounds.bounds.minimum.z:bounds.bounds.maximum.z};
            if(original_plane_distance(minimum,p)>=0){last_plane=id;return false;}
            Vec3 maximum{p.normal.x>0?bounds.bounds.maximum.x:bounds.bounds.minimum.x,p.normal.y>0?bounds.bounds.maximum.y:bounds.bounds.minimum.y,p.normal.z>0?bounds.bounds.maximum.z:bounds.bounds.minimum.z};
            if(original_plane_distance(maximum,p)<=0)mask&=~(1u<<id);
        }return true;
    }
ModelFrustum model_frustum(const RenderCamera& camera,long double angle,float height_width) {
    constexpr Matrix q{0,0,-1,0,-1,0,0,0,0,1,0,0,0,0,0,1};
    float sine=float(-std::sin(angle)),cosine=float(std::cos(angle)),negative_cosine=float(-std::cos(angle));
    float height=float(-(std::tan(angle)*height_width*camera.near_plane));
    std::array<Vec3,6> normals{{{-1,0,0},{1,0,0},{sine,negative_cosine,0},{sine,cosine,0},
        original_normalized({height,0,camera.near_plane}),original_normalized({height,0,0.0f-camera.near_plane})}};
    std::array<Vec3,6> points{{{camera.near_plane,0,0},{camera.far_plane,0,0},{},{},{},{}}};
    for(unsigned i=0;i<6;++i){normals[i]=original_rotation(q,normals[i]);points[i]=original_rotation(q,points[i])+Vec3{};}
    const auto& f=camera.frame0;Vec3 x{f[0],f[1],f[2]},y{f[4],f[5],f[6]},z{f[8],f[9],f[10]},t{f[12],f[13],f[14]},back{0.0f-z.x,0.0f-z.y,0.0f-z.z};
    ModelFrustum result;result.radius=camera.far_plane*0.5f;result.origin=back*result.radius+t;result.planes[0].normal=z;result.planes[1].normal=back;
    for(unsigned i=2;i<4;++i)result.planes[i].normal=z*normals[i].z+x*normals[i].x;
    for(unsigned i=4;i<6;++i)result.planes[i].normal=z*normals[i].z+y*normals[i].y;
    for(unsigned i=0;i<6;++i)result.planes[i].distance=float(original_dot(result.planes[i].normal,i<2?z*points[i].z+t:t));return result;
}
Lighting grid_sample(const LightGrid& grid,Vec3 minimum,Vec3 point) {
    if(grid.samples.empty())return {};
    const float p[]{point.x,point.y,point.z},m[]{minimum.x,minimum.y,minimum.z};
    std::uint32_t indices[3][2]{};float fraction[3]{};
    for(int axis=0;axis<3;++axis){
        if(!grid.spacing[axis]||!grid.dimensions[axis])return {};
        long double q=(static_cast<long double>(grid.edge_padding[axis])+p[axis]-m[axis])/grid.spacing[axis];
        fraction[axis]=float(q-std::floor(q));
        for(int upper=0;upper<2;++upper){auto cell=original_integer(std::floor(q+upper));
            indices[axis][upper]=std::uint32_t(std::clamp(cell,0,std::int32_t(grid.dimensions[axis]-1)));}
    }
    Lighting result{{},{},{}};
    constexpr unsigned corners[]{0,3,2,1,4,7,6,5};
    for(unsigned corner:corners){
        float weight=1;std::uint32_t c[3]{};
        for(int axis=0;axis<3;++axis){bool upper=(corner&(1u<<axis))!=0;c[axis]=indices[axis][upper];weight*=upper?fraction[axis]:1-fraction[axis];}
        auto index=c[0]+std::size_t(grid.dimensions[0])*(c[1]+std::size_t(grid.dimensions[1])*c[2]);
        const auto& sample=grid.samples.at(index);
        double elevation=sample.elevation*(360.0/255)*double(0.01745329238474369f),azimuth=sample.azimuth*(360.0/255)*double(0.01745329238474369f);
        result.ambient=result.ambient+Vec3{float(sample.ambient[0]),float(sample.ambient[1]),float(sample.ambient[2])}*weight;
        result.directed=result.directed+Vec3{float(sample.directed[0]),float(sample.directed[1]),float(sample.directed[2])}*weight;
        result.direction=result.direction+Vec3{float(std::cos(elevation)*std::cos(azimuth)),float(std::cos(elevation)*std::sin(azimuth)),float(std::sin(elevation))}*weight;
    }
    result.direction=original_normalized(result.direction);return result;
}
std::array<std::uint8_t,4> grid_diffuse(const LightGrid& grid,Vec3 minimum,Vec3 point,Vec3 normal) {
    if(grid.samples.empty())return {255,255,255,255};
    auto lighting=grid_sample(grid,minimum,point);float cosine=dot(lighting.direction,normal);
    if(!(cosine>0))cosine=0;
    Vec3 color=(lighting.ambient+lighting.directed*cosine)*(4.0f/255);
    float peak=std::max({1.0f,color.x,color.y,color.z});color=color/peak;
    return {std::uint8_t(original_integer(color.x*255+0.5f)),std::uint8_t(original_integer(color.y*255+0.5f)),std::uint8_t(original_integer(color.z*255+0.5f)),255};
}
bool requires_grid_colors(const Material* material) {
    return material&&std::any_of(material->passes.begin(),material->passes.end(),[](const auto& pass){
        return pass.rgb_gen==MaterialRgbGen::vertex||pass.rgb_gen==MaterialRgbGen::lighting_diffuse;
    });
}
Vec3 normalized_color(const std::uint8_t* rgb,float gain) {
    Vec3 color{rgb[0]*gain/255.0f,rgb[1]*gain/255.0f,rgb[2]*gain/255.0f};float peak=std::max({1.0f,color.x,color.y,color.z});
    color=color/peak;return {std::floor(color.x*255+0.5f)/255,std::floor(color.y*255+0.5f)/255,std::floor(color.z*255+0.5f)/255};
}
Vertex from_world(const WorldVertex& v) {
    auto color=original_world_color(v.color);return {v.position,v.texcoord,v.lightmap_texcoord,v.normal,{color[0]/255.0f,color[1]/255.0f,color[2]/255.0f,1}};
}
SurfacePtr image_surface(AssetStore& assets,std::string_view name,int diagnostic_size,DecodedLayout* layout) {
    std::string stem(name);if(auto dot=stem.find('.');dot!=std::string::npos)stem.resize(dot);
    std::string selected;
    for(std::string_view suffix:{".tga",".jpg"}){std::string candidate=stem+std::string(suffix);if(assets.contains(candidate)){selected=std::move(candidate);break;}}
    SurfacePtr loaded(nullptr,SDL_FreeSurface);
    if(!selected.empty()){
        auto bytes=assets.bytes(selected);if(bytes.size()>std::size_t(std::numeric_limits<int>::max()))throw std::runtime_error("Texture exceeds SDL stream limit");
        const auto* header=reinterpret_cast<const std::uint8_t*>(bytes.data());bool tga=selected.ends_with(".tga");
        if(!tga||(bytes.size()>=18&&header[1]==0&&header[2]==2)){SDL_RWops* rw=SDL_RWFromConstMem(bytes.data(),int(bytes.size()));if(!rw)throw std::runtime_error(SDL_GetError());loaded.reset(IMG_LoadTyped_RW(rw,1,tga?"TGA":"JPG"));}
        if(loaded&&layout){layout->components=tga?header[16]/8:3;layout->bgr=tga;}
    }
    if(!loaded){
        // Original 405a20 passes (0,4,4) to 404cf0; missing and failed decodes share this pattern.
        if(diagnostic_size<=0)throw std::runtime_error("Invalid original diagnostic image dimensions");
        SurfacePtr image(SDL_CreateRGBSurfaceWithFormat(0,diagnostic_size,diagnostic_size,32,SDL_PIXELFORMAT_RGBA32),SDL_FreeSurface);if(!image)throw std::runtime_error(SDL_GetError());
        for(int y=0;y<image->h;++y)for(int x=0;x<image->w;++x){auto* p=static_cast<std::uint8_t*>(image->pixels)+y*image->pitch+x*4;std::uint8_t c=(x%4==0&&y%4<=1)?255:0;p[0]=p[1]=p[2]=c;p[3]=255;}return image;
    }
    if(loaded->format->format==SDL_PIXELFORMAT_RGBA32)return loaded;
    SurfacePtr rgba(SDL_ConvertSurfaceFormat(loaded.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface);if(!rgba)throw std::runtime_error(SDL_GetError());return rgba;
}
SurfacePtr reduce_image(SurfacePtr image,int divisor) {
    if(divisor<=1)return image;int dx=std::min(divisor,image->w),dy=std::min(divisor,image->h),w=image->w/dx,h=image->h/dy;
    SurfacePtr reduced(SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32),SDL_FreeSurface);if(!reduced)throw std::runtime_error(SDL_GetError());
    const std::size_t row_advance=std::size_t(w)*dx+std::size_t(image->w)*(dy-1);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){std::size_t k=std::size_t(y)*row_advance+std::size_t(x)*dx;
        const auto* p=static_cast<const std::uint8_t*>(image->pixels)+(k/image->w)*image->pitch+(k%image->w)*4;
        auto* out=static_cast<std::uint8_t*>(reduced->pixels)+y*reduced->pitch+x*4;std::memcpy(out,p,4);
    }return reduced;
}
Vertex quadratic(const Vertex* controls,float u,float v) {
    float a[3]{(1-u)*(1-u),2*u*(1-u),u*u},b[3]{(1-v)*(1-v),2*v*(1-v),v*v}; Vertex out{};
    for(int y=0;y<3;++y) for(int x=0;x<3;++x) {
        const Vertex& c=controls[y*3+x]; float w=a[x]*b[y]; out.position=out.position+c.position*w; out.normal=out.normal+c.normal*w;
        out.uv.x+=c.uv.x*w;out.uv.y+=c.uv.y*w;out.light_uv.x+=c.light_uv.x*w;out.light_uv.y+=c.light_uv.y*w;
        for(int k=0;k<4;++k)out.color[k]+=c.color[k]*w;
    }
    out.normal=original_normalized(out.normal);for(auto& c:out.color)c=float(original_integer(c*255))/255;return out;
}
void tessellate(const Level& level,const Surface& surface,std::vector<Vertex>& vertices,std::vector<std::uint32_t>& indices) {
    if(surface.patch_width<3 || surface.patch_height<3 || !(surface.patch_width&1) || !(surface.patch_height&1) ||
       std::uint64_t(surface.patch_width)*surface.patch_height!=surface.vertex_count)
        throw std::runtime_error("Invalid quadratic patch control net");
    // Original xCBezierSurface constructor requests seven samples per segment.
    constexpr unsigned subdivisions=6;
    for(unsigned y=0;y+2<surface.patch_height;y+=2)for(unsigned x=0;x+2<surface.patch_width;x+=2) {
        Vertex controls[9];for(unsigned j=0;j<3;++j)for(unsigned i=0;i<3;++i)
            controls[j*3+i]=from_world(level.vertices.at(surface.first_vertex+(y+j)*surface.patch_width+x+i));
        std::uint32_t first=static_cast<std::uint32_t>(vertices.size());
        for(unsigned j=0;j<=subdivisions;++j)for(unsigned i=0;i<=subdivisions;++i)
            vertices.push_back(quadratic(controls,float(i)/subdivisions,float(j)/subdivisions));
        for(unsigned j=0;j<subdivisions;++j)for(unsigned i=0;i<subdivisions;++i) {
            std::uint32_t a=first+j*(subdivisions+1)+i,b=a+subdivisions+1;
            indices.insert(indices.end(),{a,b,a+1,a+1,b,b+1});
        }
    }
}
float renderer_horizontal_fov(float reference_degrees,float aspect) {
    return 360.0f/pi*std::atan(std::tan(reference_degrees*pi/360.0f)*0.75f*aspect);
}
std::array<std::uint8_t,4> original_world_color(std::array<std::uint8_t,4> stored) {
    Vec3 c=normalized_color(stored.data(),8);return {std::uint8_t(c.x*255+0.5f),std::uint8_t(c.y*255+0.5f),std::uint8_t(c.z*255+0.5f),255};
}
Vec3 cube_capture_center(Vec3 previous,Vec3 contribution) {
    for(int step=0;step<6;++step)previous=(previous+contribution)*0.5f;
    return previous;
}
int effective_texture_filter(int requested,bool mipmaps) {
    if(requested<0||requested>3||(!mipmaps&&requested>=2))return 1;
    return requested;
}
float wave_value(const MaterialWave& wave,double time) {
    float r=float(std::fmod(time*wave.frequency,1.0)),p=float(std::fmod(r+wave.phase,1.0)),value=0;
    switch(wave.function){case Waveform::sin:value=std::sin((r+wave.phase)*360*0.01745329238474369f);break;
        case Waveform::triangle:value=4*p;if(value>2)value=4-value;value-=1;break;
        case Waveform::square:value=p<=0.5f?1.0f:-1.0f;break;case Waveform::sawtooth:value=p;break;
        case Waveform::inverse_sawtooth:value=float(std::fmod(1-(r+wave.phase),1.0));break;}
    return wave.base+wave.amplitude*value;
}
Matrix texture_matrix(const MaterialPass& pass,double time,const Matrix& view) {
    Matrix result=render_identity;
    for(const auto& mod:pass.tc_mods){Matrix m=render_identity;const auto& v=mod.values;
        switch(mod.kind){
            case MaterialTcModKind::rotate:{float angle=float(std::fmod(time*v[0],360.0))*0.01745329238474369f,c=std::cos(angle),s=std::sin(angle);
                m[0]=m[5]=c;m[1]=s;m[4]=-s;m[12]=0.5f*(1-c+s);m[13]=0.5f*(1-s-c);break;}
            case MaterialTcModKind::scale:m[0]=v[0];m[5]=v[1];break;
            case MaterialTcModKind::scroll:m[12]=-float(std::fmod(time*v[0],1.0));m[13]=-float(std::fmod(time*v[1],1.0));break;
            case MaterialTcModKind::transform:m[0]=v[0];m[4]=v[1];m[1]=v[2];m[5]=v[3];m[12]=m[14]=float(time)*v[4];break;
            case MaterialTcModKind::cube:m=inverse_rigid(view);m[12]=m[13]=m[14]=0;break;
            case MaterialTcModKind::avi:m[5]=-1;break;
        }
        result=multiply(result,m);
    }return result;
}
SurfacePtr combined_image(AssetStore& assets,const MaterialTexture& input) {
    if(input.width<=0||input.height<=0)throw std::runtime_error("Invalid combined material image dimensions");
    SurfacePtr atlas(SDL_CreateRGBSurfaceWithFormat(0,input.width,input.height,32,SDL_PIXELFORMAT_RGBA32),SDL_FreeSurface);
    if(!atlas)throw std::runtime_error(SDL_GetError());SDL_FillRect(atlas.get(),nullptr,SDL_MapRGBA(atlas->format,0,0,0,255));
    for(const auto& layer:input.layers){auto image=image_surface(assets,layer.image,4);
        if(std::int64_t(layer.x)+image->w>input.width||std::int64_t(layer.y)+image->h>input.height)continue;
        if(layer.x<0||layer.y<0)throw std::runtime_error("Original combined image subupload has an invalid negative offset");
        for(int y=0;y<image->h;++y)for(int x=0;x<image->w;++x){auto* from=static_cast<const std::uint8_t*>(image->pixels)+y*image->pitch+x*4;
            auto* to=static_cast<std::uint8_t*>(atlas->pixels)+(layer.y+y)*atlas->pitch+(layer.x+x)*4;std::memcpy(to,from,3);}}
    return atlas;
}
void cube_face_bytes(const SDL_Surface& image,DecodedLayout layout,std::span<std::uint8_t> raw) {
    std::size_t available=std::size_t(image.w)*image.h*layout.components;
    if(raw.size()>available)throw std::runtime_error("Original cube-face raw prefix exceeds decoded image storage");
    for(std::size_t k=0;k<raw.size();++k){std::size_t pixel=k/layout.components;int channel=int(k%layout.components);
        auto* p=static_cast<const std::uint8_t*>(image.pixels)+(pixel/image.w)*image.pitch+(pixel%image.w)*4;raw[k]=p[layout.bgr&&channel<3?2-channel:channel];}
}
void avi_frame_bytes(const VideoFrame& frame,std::vector<std::uint8_t>& rgb) {
    if(frame.rgba.size()!=std::size_t(frame.width)*frame.height*4)throw std::runtime_error("Original AVI material upload has invalid pixel storage");
    rgb.resize(std::size_t(frame.width)*frame.height*3);
    for(int y=0;y<frame.height;++y)for(int x=0;x<frame.width;++x){auto* p=frame.rgba.data()+(std::size_t(frame.height-1-y)*frame.width+x)*4;
        std::memcpy(rgb.data()+(std::size_t(y)*frame.width+x)*3,p,3);}
}
namespace {
template<class Index> void deform_kernel(const Material& material,Primitive primitive,std::span<const Index> indices,
    std::span<const RenderVertex> posed,std::span<RenderVertex> scratch,const Matrix& model,const RenderCamera& camera,double seconds) {
    if(scratch.size()!=posed.size())throw std::runtime_error("Deform scratch size differs from posed geometry");
    std::copy(posed.begin(),posed.end(),scratch.begin());
    if(material.deforms.empty())return;
    const Matrix inverse_model=inverse_unchecked(model),camera_axes=multiply(inverse_model,inverse_rigid(camera.view));
    Vec3 right=original_normalized(Vec3{camera_axes[0],camera_axes[1],camera_axes[2]}),up=original_normalized(Vec3{camera_axes[4],camera_axes[5],camera_axes[6]});
    for(const auto& deform:material.deforms) {
        if(deform.kind==MaterialDeformKind::wave){
            if(posed.empty()||indices.empty())continue;Vec3 reference=posed[indices[0]].position;
            Vec3 direction=original_normalized(posed.front().position-posed.back().position);
            for(auto& vertex:scratch){auto p=vertex.position;float shift=dot(p-reference,direction)/deform.spread;vertex.position=p+vertex.normal*wave_value(deform.wave,seconds+shift);}
            for(auto& vertex:scratch)vertex.normal={};
            auto add_normal=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c){
                if(a>=scratch.size()||b>=scratch.size()||c>=scratch.size())throw std::out_of_range("Deform triangle outside vertex storage");
                auto& va=scratch[a];auto& vb=scratch[b];auto& vc=scratch[c];
                Vec3 n=original_normalized(cross(vb.position-va.position,vc.position-va.position));if(material.cull==MaterialCull::front)n=n*-1;
                va.normal=va.normal+n;vb.normal=vb.normal+n;vc.normal=vc.normal+n;};
            if(primitive==Primitive::triangle_strip){for(std::size_t i=2;i<indices.size();++i){std::uint32_t a=indices[i-2],b=indices[i-1],c=indices[i];if(i&1)std::swap(a,b);if(a!=b&&b!=c&&a!=c)add_normal(a,b,c);}}
            else for(std::size_t i=0;i+2<indices.size();i+=3)add_normal(indices[i],indices[i+1],indices[i+2]);
            continue;
        }
        if(scratch.size()%4!=0)throw std::runtime_error("Autosprite authored mesh does not contain four-vertex groups");
        for(std::size_t first=0;first<scratch.size();first+=4) {
            Vertex* quad=scratch.data()+first;Vec3 center{};for(int i=0;i<4;++i)center=center+quad[i].position*0.25f;
            Vec3 edge0=quad[0].position-quad[1].position,edge1=quad[0].position-quad[3].position;
            float a=dot(edge0,edge0)*0.5f,b=dot(edge1,edge1)*0.5f;if(a<0.001f)a=0.1f;if(b<0.001f)b=0.1f;
            float major_size=original_sqrt(std::max(a,b)),minor_size=original_sqrt(std::min(a,b));Vec3 major=right,minor=up;
            Vec3 view_axis=original_normalized(Vec3{camera_axes[8],camera_axes[9],camera_axes[10]});
            if(deform.kind==MaterialDeformKind::autosprite2){major=original_normalized(a>=b?edge0:edge1);minor=original_normalized(cross(major,view_axis));}
            Vec3 n=deform.kind==MaterialDeformKind::autosprite?view_axis:original_normalized(cross(minor,major));
            const float signs[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
            for(int i=0;i<4;++i){quad[i].position=center+major*(major_size*signs[i][0])+minor*(minor_size*signs[i][1]);quad[i].normal=n;}
        }
    }
}
}
void deform_vertices(const Material& material,Primitive primitive,std::span<const std::uint16_t> indices,
    std::span<const RenderVertex> posed,std::span<RenderVertex> scratch,const Matrix& model,const RenderCamera& camera,double seconds) {
    deform_kernel(material,primitive,indices,posed,scratch,model,camera,seconds);
}
void deform_vertices(const Material& material,Primitive primitive,std::span<const std::uint32_t> indices,
    std::span<const RenderVertex> posed,std::span<RenderVertex> scratch,const Matrix& model,const RenderCamera& camera,double seconds) {
    deform_kernel(material,primitive,indices,posed,scratch,model,camera,seconds);
}
void normalize_lightmap(const Lightmap& input,std::span<std::uint8_t,128*128*3> output) {
    for(std::size_t p=0;p<input.size();p+=3){auto color=normalized_color(input.data()+p,4);
        auto* out=output.data()+p;out[0]=std::uint8_t(color.x*255+0.5f);out[1]=std::uint8_t(color.y*255+0.5f);out[2]=std::uint8_t(color.z*255+0.5f);}
}
void reduce_lightmap(std::span<const std::uint8_t,128*128*3> pixels,int divisor,std::span<std::uint8_t> reduced) {
    if(divisor<1||divisor>128)throw std::runtime_error("Invalid lightmap reduction divisor");
    int size=128/divisor;if(reduced.size()!=std::size_t(size)*size*3)throw std::runtime_error("Lightmap reduction storage size mismatch");
    for(int y=0;y<size;++y)for(int x=0;x<size;++x){std::size_t source=std::size_t(y)*(size*divisor+128*(divisor-1))+x*divisor;
        std::memcpy(reduced.data()+(y*size+x)*3,pixels.data()+source*3,3);}
}
std::array<float,9> material_normal_matrix(const Matrix& model) {
    const float a=model[0],b=model[1],c=model[2],d=model[4],e=model[5],f=model[6],g=model[8],h=model[9],i=model[10];
    std::array<float,9> normal_matrix{e*i-f*h,f*g-d*i,d*h-e*g,c*h-b*i,a*i-c*g,b*g-a*h,b*f-c*e,c*d-a*f,a*e-b*d};
    const float determinant=a*normal_matrix[0]+d*normal_matrix[3]+g*normal_matrix[6];for(auto& value:normal_matrix)value/=determinant;
    return normal_matrix;
}
std::array<int,2> interface_extent(int width,int height) {
    int ui_width=std::min(width,height*4/3),ui_height=ui_width*3/4;return {ui_width,ui_height};
}
RenderRect interface_rect(const InterfaceQuad& q,int width,int height) {
    RenderRect rect=q.rect;if(q.native_bar_fraction){int bar=native_bar_height(width,height,*q.native_bar_fraction);if(!bar)return {};
        float h=float(bar)*768/height;rect={0,q.rect.y==0?0:768-h,1024,h};}
    return rect;
}
Matrix interface_model(const InterfaceQuad& q,const RenderRect& rect) {
    Matrix model=render_identity;model[0]=rect.width;model[5]=rect.height;model[12]=rect.x;model[13]=rect.y;
    if(q.rotation_degrees!=0){float angle=q.rotation_degrees*pi/180,c=std::cos(angle),sn=std::sin(angle),x=rect.x-q.rotation_center.x,y=rect.y-q.rotation_center.y;
        model[0]=c*rect.width;model[1]=sn*rect.width;model[4]=-sn*rect.height;model[5]=c*rect.height;
        model[12]=q.rotation_center.x+c*x-sn*y;model[13]=q.rotation_center.y+sn*x+c*y;}
    return model;
}
std::array<std::array<int,4>,4> interface_margin_bands(int width,int height,int ui_width,int ui_height) {
    int x=(width-ui_width)/2,y=(height-ui_height)/2;
    return {{{0,0,x,height},{x+ui_width,0,width-x-ui_width,height},{x,0,ui_width,y},{x,y+ui_height,ui_width,height-y-ui_height}}};
}
#ifdef PUSU_RENDERER_MATH_CHECK
void renderer_math_check() {
    RenderCamera camera;auto frustum=model_frustum(camera,90.0L*std::bit_cast<float>(std::uint32_t{0x3c0efa35}),0.75f);
    auto bound=cache_bounds({{-1,-1,-11},{1,1,-9}});if(!frustum.visible(bound,render_identity))throw std::runtime_error("Original model frustum interior invariant failed");
    auto outside=render_identity;outside[12]=10000;if(frustum.visible(bound,outside))throw std::runtime_error("Original model frustum exterior invariant failed");
    auto touching=cache_bounds({{0,0,-4},{0,0,-4}});if(!frustum.visible(touching,render_identity))throw std::runtime_error("Original OBB plane equality must remain visible");
    AdvancedBounds leaf;leaf.bounds={{0,0,-4},{0,0,-4}};leaf.center={0,0,-4};unsigned cached_plane=3;std::uint32_t mask=63;
    if(frustum.visible(leaf,cached_plane,mask))throw std::runtime_error("Original AABB minimum plane equality must reject");
    auto unordered=render_identity;unordered[12]=std::numeric_limits<float>::quiet_NaN();
    if(!frustum.visible(bound,unordered))throw std::runtime_error("Original unordered culling comparison must not invent a rejection");
    if(native_bar_height(1280,720,1)!=14||native_bar_height(1024,768,1)!=107||native_bar_height(1280,720,0)!=0)
        throw std::runtime_error("Original drawable-pixel gameplay bar invariant failed");
    for(int dimension:{128,256,512,129,576,720}){int atlas=avi_atlas_side(dimension);
        if((atlas&(atlas-1))!=0||atlas<dimension||atlas/2>=dimension)
            throw std::runtime_error("Original AVI atlas must be the smallest enclosing power of two");}
    SurfacePtr source(SDL_CreateRGBSurfaceWithFormat(0,5,4,32,SDL_PIXELFORMAT_RGBA32),SDL_FreeSurface);if(!source)throw std::runtime_error(SDL_GetError());
    for(int y=0;y<4;++y)for(int x=0;x<5;++x){auto* p=static_cast<std::uint8_t*>(source->pixels)+y*source->pitch+x*4;p[0]=std::uint8_t(y*5+x);p[3]=77;}
    auto reduced=reduce_image(std::move(source),2);const std::uint8_t expected[]{0,2,9,11};
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){auto* p=static_cast<const std::uint8_t*>(reduced->pixels)+y*reduced->pitch+x*4;
        if(p[0]!=expected[y*2+x]||p[3]!=77)throw std::runtime_error("Original odd-width byte-decimation cursor invariant failed");}
    auto close=[](float a,float b){if(std::abs(a-b)>0.0001f)throw std::runtime_error("Renderer math invariant failed");};
    {
        Material material;MaterialDeform wave_deform;wave_deform.kind=MaterialDeformKind::wave;wave_deform.spread=1;wave_deform.wave.base=1;
        material.cull=MaterialCull::front;material.deforms.push_back(wave_deform);
        std::array<RenderVertex,4> posed{};
        posed[0].position={0,0,0};posed[1].position={1,0,0};posed[2].position={0,1,0};posed[3].position={1,1,0};
        for(auto& vertex:posed)vertex.normal={0,0,1};
        const std::array<std::uint16_t,4> narrow{0,1,2,3};const std::array<std::uint32_t,4> wide{0,1,2,3};
        std::array<RenderVertex,4> a{},b{};
        deform_vertices(material,Primitive::triangle_strip,narrow,posed,a,render_identity,camera,0);
        deform_vertices(material,Primitive::triangle_strip,wide,posed,b,render_identity,camera,0);
        if(std::memcmp(a.data(),b.data(),sizeof(a))!=0)throw std::runtime_error("Deform uint16/uint32 kernels diverged");
        for(const auto& vertex:a){close(vertex.position.z,1);if(!(vertex.normal.z<0))throw std::runtime_error("Deform strip/front-cull winding changed");}
        material.deforms[0].kind=MaterialDeformKind::autosprite;
        deform_vertices(material,Primitive::triangles,narrow,posed,a,render_identity,camera,0);
        deform_vertices(material,Primitive::triangles,wide,posed,b,render_identity,camera,0);
        if(std::memcmp(a.data(),b.data(),sizeof(a))!=0)throw std::runtime_error("Autosprite index-width seam diverged");
        material.deforms.clear();posed[0].uv.x=std::bit_cast<float>(std::uint32_t{0x7fc12345});
        deform_vertices(material,Primitive::triangles,narrow,posed,a,render_identity,camera,0);
        if(std::bit_cast<std::uint32_t>(a[0].uv.x)!=0x7fc12345u)throw std::runtime_error("CPU deform seam changed raw IEEE attributes");
        auto scaled=render_identity;scaled[0]=-2;scaled[5]=3;scaled[10]=4;
        auto normals=material_normal_matrix(scaled);close(normals[0],-.5f);close(normals[4],1.0f/3);close(normals[8],.25f);
        const Vec2 triangle[3]{{-1,-1},{0,1},{1,-1}};
        auto signed_area=[&](float y_scale,float x_scale){auto a=triangle[0],b=triangle[1],c=triangle[2];
            return ((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x))*x_scale*y_scale;};
        if(!(signed_area(1,1)<0&&signed_area(-1,1)>0&&signed_area(-1,-1)<0))
            throw std::runtime_error("Vulkan negative viewport/CW-to-CCW signed-area contract changed");
        if(interface_extent(1280,720)!=std::array<int,2>{960,720})throw std::runtime_error("Legacy UI centered aspect changed");
        InterfaceQuad q;q.rect={0,1,1024,768};q.native_bar_fraction=1;
        auto rect=interface_rect(q,1280,720);close(rect.height,14.0f*768/720);close(rect.y,768-rect.height);
        q.native_bar_fraction=0;if(interface_rect(q,1280,720).height!=0)throw std::runtime_error("Zero native bar must be omitted");
        auto bands=interface_margin_bands(1280,720,960,720);
        if(bands[0]!=std::array<int,4>{0,0,160,720}||bands[1]!=std::array<int,4>{1120,0,160,720})
            throw std::runtime_error("Legacy margin bands changed");
        q.native_bar_fraction.reset();q.rotation_degrees=90;q.rotation_center={};
        auto rotated=interface_model(q,{1,2,3,4});close(rotated[1],3);close(rotated[4],-4);close(rotated[12],-2);close(rotated[13],1);
        const std::array<std::uint8_t,16> rgba{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        std::vector<std::uint8_t> rgb;avi_frame_bytes(VideoFrame{2,2,rgba,0},rgb);
        if(rgb!=std::vector<std::uint8_t>{9,10,11,13,14,15,1,2,3,5,6,7})throw std::runtime_error("AVI RGB flip/alpha omission changed");
        SurfacePtr face(SDL_CreateRGBSurfaceWithFormat(0,2,1,32,SDL_PIXELFORMAT_RGBA32),SDL_FreeSurface);if(!face)throw std::runtime_error(SDL_GetError());
        std::memcpy(face->pixels,rgba.data(),8);std::array<std::uint8_t,6> raw{};cube_face_bytes(*face,{4,true},raw);
        if(raw!=std::array<std::uint8_t,6>{3,2,1,4,7,6})throw std::runtime_error("Cube raw prefix/BGR channels changed");
        Lightmap map{};map[0]=1;map[1]=2;map[2]=3;std::array<std::uint8_t,128*128*3> map_rgb{};
        normalize_lightmap(map,map_rgb);if(map_rgb[0]!=4||map_rgb[1]!=8||map_rgb[2]!=12)throw std::runtime_error("Lightmap gain4 byte kernel changed");
    }
    if(original_world_color({1,2,3,0})!=std::array<std::uint8_t,4>{8,16,24,255}||
       original_world_color({255,128,0,17})!=std::array<std::uint8_t,4>{255,128,0,255})
        throw std::runtime_error("Original world-color hue and alpha invariant failed");
    close(renderer_horizontal_fov(90,4.0f/3),90);close(perspective(90,4.0f/3,1,1000)[5],perspective(90,16.0f/9,1,1000)[5]);
    Vertex controls[9]{};for(int y=0;y<3;++y)for(int x=0;x<3;++x){controls[y*3+x].position={float(x),float(y),x==1&&y==1?4.0f:0.0f};controls[y*3+x].normal={0,0,1};}
    auto center=quadratic(controls,0.5f,0.5f);close(center.position.x,1);close(center.position.y,1);close(center.position.z,1);
    Level level;level.vertices.resize(9);for(int i=0;i<9;++i){level.vertices[i].position=controls[i].position;level.vertices[i].normal={0,0,1};}
    Surface patch;patch.patch_width=patch.patch_height=3;patch.vertex_count=9;
    std::vector<Vertex> vertices;std::vector<std::uint32_t> indices;tessellate(level,patch,vertices,indices);
    for(const auto& vertex:vertices){auto p=vertex.position;close(p.z,p.x*(2-p.x)*p.y*(2-p.y));}
    for(std::size_t i=0;i<indices.size();i+=3){
        if(i+2>=indices.size())throw std::runtime_error("Quadratic patch has an incomplete triangle");
        const auto& a=vertices.at(indices[i]).position;const auto& b=vertices.at(indices[i+1]).position;const auto& c=vertices.at(indices[i+2]).position;
        if(!(cross(b-a,c-a).z<0))throw std::runtime_error("Quadratic patch triangles must preserve nondegenerate original winding");
    }
    LightGrid grid;grid.spacing={1,1,1};grid.dimensions={2,1,1};grid.samples.resize(2);grid.samples[0].ambient={64,0,0};grid.samples[1].ambient={192,0,0};close(grid_sample(grid,{},{0.5f,0,0}).ambient.x,128);
    grid.samples[1].ambient={0,0,0};if(grid_diffuse(grid,{},{0.5f,0,0},{0,0,1})!=std::array<std::uint8_t,4>{128,0,0,255})throw std::runtime_error("Black grid corners must retain their weight");
    grid.edge_padding={1,0,0};close(grid_sample(grid,{},{0,0,0}).ambient.x,0);
    if(original_integer(std::numeric_limits<float>::quiet_NaN())!=0||original_integer(0x1p63L)!=0)throw std::runtime_error("Original invalid x87 conversion invariant failed");
    MaterialWave wave;wave.frequency=1;wave.amplitude=1;wave.function=Waveform::triangle;close(wave_value(wave,0),-1);close(wave_value(wave,0.5),1);
    wave.function=Waveform::square;close(wave_value(wave,0.5),1);wave.function=Waveform::inverse_sawtooth;close(wave_value(wave,0),0);
    MaterialPass transformed;MaterialTcMod transform;transform.kind=MaterialTcModKind::transform;transform.values={1,0,0,1,0.25f,0.5f};transformed.tc_mods.push_back(transform);
    auto matrix=texture_matrix(transformed,2,render_identity);close(matrix[12],0.5);close(matrix[13],0);close(matrix[14],0.5);
    auto require_cube=[](bool condition,const char* message){if(!condition)throw std::runtime_error(message);};
    auto rejects_cube=[](auto operation){
        try{operation();}catch(const std::runtime_error&){return;}
        throw std::runtime_error("Malformed cube cache must raise a meaningful error");
    };
    auto append_word=[](std::vector<std::uint8_t>& bytes,std::uint32_t word){
        for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(std::uint8_t(word>>shift));
    };
    auto append_entry=[&](std::vector<std::uint8_t>& bytes,Vec3 position,std::uint32_t offset){
        append_word(bytes,std::bit_cast<std::uint32_t>(position.x));append_word(bytes,std::bit_cast<std::uint32_t>(position.y));
        append_word(bytes,std::bit_cast<std::uint32_t>(position.z));append_word(bytes,offset);
    };
    std::vector<std::uint8_t> terminator;for(int i=0;i<4;++i)append_word(terminator,99999999);
    std::vector<std::uint8_t> wire;append_entry(wire,{0,0,0},18);append_entry(wire,{0.05f,0,0},36);append_entry(wire,{5,6,7},0);
    wire.insert(wire.end(),terminator.begin(),terminator.end());
    CubeCache cache{cube_cache_entries(wire),std::vector<std::uint8_t>(54)};
    for(std::size_t i=0;i<cache.rgb.size();++i)cache.rgb[i]=std::uint8_t(i);
    require_cube(cube_read_word(wire,12)==18&&cache.entries.size()==3&&cache.entries[2].center.y==6,
        "Cube cache must decode little-endian float/offset records");
    require_cube(cube_cache_offset(cache,{0.04f,0,0})==18,
        "Cube cache overlap must choose the first record, not the nearest");
    auto zero_faces=cube_cache_faces(cache,{5,6,7},1);auto ordered_faces=cube_cache_faces(cache,{0.04f,0,0},1);
    require_cube(zero_faces.size()==18&&zero_faces.data()==cache.rgb.data()&&ordered_faces[0]==18&&ordered_faces[17]==35,
        "Cube cache matched offset zero and all six contiguous RGB faces must remain valid");
    require_cube(cube_cache_offset(cache,{100,100,100})==0&&cube_cache_faces(cache,{100,100,100},1).data()==cache.rgb.data(),
        "Original cube cache no-match must use offset zero");
    std::vector<std::uint8_t> boundary_wire(wire.begin(),wire.begin()+16);
    boundary_wire.insert(boundary_wire.end(),terminator.begin(),terminator.end());
    CubeCache boundary{cube_cache_entries(boundary_wire),{}};
    const float tolerance=std::bit_cast<float>(std::uint32_t{0x3dcccccd});
    const float inside=std::nextafter(tolerance,0.0f);
    require_cube(cube_cache_offset(boundary,{inside,inside,inside})==18&&cube_cache_offset(boundary,{tolerance,0,0})==0&&
        cube_cache_offset(boundary,{0,-tolerance,0})==0&&cube_cache_offset(boundary,{0,0,tolerance})==0,
        "Cube cache tolerance must be componentwise, widened and strictly less than float 0.1");
    // Float subtraction rounds this difference back to the tolerance; widened subtraction must match.
    boundary.entries[0].center={-0x1p-29f,0,0};
    require_cube(cube_cache_offset(boundary,{-tolerance,0,0})==18,
        "Cube cache matching must subtract float centers in long double");
    CubeCache empty{cube_cache_entries(terminator),{}};
    require_cube(empty.entries.empty()&&cube_cache_offset(empty,{})==0,"Sentinel-only cube cache must allow an empty RGB blob");
    rejects_cube([&]{cube_cache_faces(empty,{},1);});
    rejects_cube([&]{cube_cache_faces(cache,{5,6,7},2);});
    rejects_cube([&]{cube_cache_faces(cache,{100,100,100},2);});
    rejects_cube([&]{cube_cache_faces(cache,{},0);});
    rejects_cube([&]{cube_cache_faces(cache,{},std::numeric_limits<int>::max());});
    rejects_cube([&]{cube_read_word(wire,wire.size()-3);});
    rejects_cube([&]{cube_read_word(wire,std::numeric_limits<std::size_t>::max());});
    rejects_cube([&]{cube_cache_entries(std::span<const std::uint8_t>(wire).first(wire.size()-1));});
    rejects_cube([&]{cube_cache_entries(std::span<const std::uint8_t>(wire).first(16));});
    std::vector<std::uint8_t> negative;append_entry(negative,{},0xffffffffu);negative.insert(negative.end(),terminator.begin(),terminator.end());
    rejects_cube([&]{cube_cache_entries(negative);});
    constexpr std::uint32_t cache_nan_bits=0x7fc12345u;
    std::vector<std::uint8_t> nonfinite;append_entry(nonfinite,{std::bit_cast<float>(cache_nan_bits),
        std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()},18);
    nonfinite.insert(nonfinite.end(),terminator.begin(),terminator.end());
    CubeCache raw_coordinates{cube_cache_entries(nonfinite),std::vector<std::uint8_t>(36)};
    require_cube(std::bit_cast<std::uint32_t>(raw_coordinates.entries[0].center.x)==cache_nan_bits&&
        std::bit_cast<std::uint32_t>(raw_coordinates.entries[0].center.y)==0x7f800000u&&
        std::bit_cast<std::uint32_t>(raw_coordinates.entries[0].center.z)==0xff800000u&&
        cube_cache_offset(raw_coordinates,{})==0&&cube_cache_faces(raw_coordinates,{},1).data()==raw_coordinates.rgb.data()&&
        cube_cache_offset(cache,{std::bit_cast<float>(cache_nan_bits),0,0})==0,
        "Cube cache XYZ must preserve raw IEEE bits and unordered comparisons must retain original no-match offset zero");
    auto bad_sentinel=terminator;bad_sentinel[4]=0;rejects_cube([&]{cube_cache_entries(bad_sentinel);});
    auto trailing=terminator;trailing.insert(trailing.end(),terminator.begin(),terminator.end());rejects_cube([&]{cube_cache_entries(trailing);});
    std::vector<std::uint8_t> sentinel_coordinate;append_entry(sentinel_coordinate,{std::bit_cast<float>(std::uint32_t{99999999}),0,0},0);
    sentinel_coordinate.insert(sentinel_coordinate.end(),terminator.begin(),terminator.end());
    auto sentinel_coordinate_entries=cube_cache_entries(sentinel_coordinate);
    require_cube(sentinel_coordinate_entries.size()==1&&sentinel_coordinate_entries[0].offset==0&&
        std::bit_cast<std::uint32_t>(sentinel_coordinate_entries[0].center.x)==99999999,
        "Cube cache sentinel selection must use the offset DWORD, not float coordinate bits");
    std::vector<std::uint8_t> mixed_sentinel;append_entry(mixed_sentinel,{},99999999);
    mixed_sentinel.insert(mixed_sentinel.end(),terminator.begin(),terminator.end());
    rejects_cube([&]{cube_cache_entries(mixed_sentinel);});
    require_cube(cube_cache_quality(" \t+1 ignored tokens\r\n999")==1&&cube_cache_quality("2147483647")==std::numeric_limits<int>::max(),
        "Cube quality must read only the first bounded positive decimal token on the first line");
    for(std::string_view invalid:{"","false","0","-1","+","1junk","2147483648","\n1"})
        rejects_cube([&]{cube_cache_quality(invalid);});
    auto capture_center=cube_capture_center({64,64,64},{});
    require_cube(capture_center.x==1&&capture_center.y==1&&capture_center.z==1,
        "Cube capture center must perform all six sequential float half-averages");
    for(bool mipmaps:{false,true})for(int requested=-1;requested<=4;++requested){
        int expected_filter=requested<0||requested>3?1:(!mipmaps&&requested>=2?1:requested);
        require_cube(effective_texture_filter(requested,mipmaps)==expected_filter,"Original texture filter and mipmap policy invariant failed");
    }
    std::vector<std::uint8_t> geometry_wire;append_entry(geometry_wire,{},0);append_entry(geometry_wire,{1,2,3},72);
    geometry_wire.insert(geometry_wire.end(),terminator.begin(),terminator.end());
    CubeCache geometry_cache{cube_cache_entries(geometry_wire),std::vector<std::uint8_t>(72+288)};
    validate_cube_cache_geometry(geometry_cache);validate_cube_cache_geometry(empty);
    require_cube(cube_cache_region_bytes(geometry_cache,0)==18*2*2&&cube_cache_region_bytes(geometry_cache,72)==18*4*4&&
        cube_cache_region_bytes(geometry_cache,cube_cache_offset(geometry_cache,{100,100,100}))==72,
        "Cube cache region geometry must retain varying face sides and original no-match region zero");
    auto coincident=geometry_cache;coincident.entries[1].center=coincident.entries[0].center;
    validate_cube_cache_geometry(coincident);
    require_cube(cube_cache_offset(coincident,{})==0&&
        owned_cube_cache_faces(coincident,0,{},2).data()==coincident.rgb.data()&&
        owned_cube_cache_faces(coincident,1,{},4).data()==coincident.rgb.data()+72,
        "Owned coincident-center cubes must reuse their ordered distinct six-face regions without changing imported first-match lookup");
    rejects_cube([&]{owned_cube_cache_faces(coincident,1,{},2);});
    rejects_cube([&]{owned_cube_cache_faces(coincident,2,{},4);});
    rejects_cube([&]{owned_cube_cache_faces(coincident,1,{0.05f,0,0},4);});
    coincident.entries[1].center=raw_coordinates.entries[0].center;
    require_cube(owned_cube_cache_faces(coincident,1,coincident.entries[1].center,4).data()==coincident.rgb.data()+72,
        "Owned cube identity must preserve unordered raw coordinate DWORDs");
    auto changed_nan=coincident.entries[1].center;changed_nan.x=std::bit_cast<float>(cache_nan_bits+1);
    rejects_cube([&]{owned_cube_cache_faces(coincident,1,changed_nan,4);});
    rejects_cube([&]{cube_cache_region_bytes(geometry_cache,18);});
    rejects_cube([&]{cube_cache_region_bytes(empty,0);});
    auto invalid_geometry=geometry_cache;invalid_geometry.rgb.resize(72+18*15);
    rejects_cube([&]{validate_cube_cache_geometry(invalid_geometry);});
    invalid_geometry=geometry_cache;invalid_geometry.rgb.pop_back();
    rejects_cube([&]{validate_cube_cache_geometry(invalid_geometry);});
    invalid_geometry=geometry_cache;invalid_geometry.entries[0].offset=18;
    rejects_cube([&]{validate_cube_cache_geometry(invalid_geometry);});
    invalid_geometry=geometry_cache;invalid_geometry.entries[1].offset=0;
    rejects_cube([&]{validate_cube_cache_geometry(invalid_geometry);});
    invalid_geometry=geometry_cache;invalid_geometry.entries[1].offset=std::uint32_t(invalid_geometry.rgb.size());
    rejects_cube([&]{validate_cube_cache_geometry(invalid_geometry);});
    invalid_geometry=empty;invalid_geometry.rgb.push_back(0);
    rejects_cube([&]{validate_cube_cache_geometry(invalid_geometry);});
    invalid_geometry=geometry_cache;invalid_geometry.rgb.resize(18);
    rejects_cube([&]{validate_cube_cache_geometry(invalid_geometry);});
}
#endif
} // namespace pusu
#ifdef PUSU_RENDERER_MATH_CHECK
int main() { pusu::renderer_math_check(); }
#endif
