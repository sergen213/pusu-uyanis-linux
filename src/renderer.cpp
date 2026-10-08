#include "renderer.hpp"
#include "renderer_shared.hpp"
#include "renderer_cube_cache.hpp"
#include "resources.hpp"
#include "scene_math.hpp"
#include <GL/glew.h>
#include <SDL.h>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <iostream>

namespace pusu {
namespace {
using Vertex=RenderVertex;
GLuint shader(GLenum type,const char* source) {
    GLuint result=glCreateShader(type);glShaderSource(result,1,&source,nullptr);glCompileShader(result);GLint ok=0;glGetShaderiv(result,GL_COMPILE_STATUS,&ok);
    if(!ok){GLint size=0;glGetShaderiv(result,GL_INFO_LOG_LENGTH,&size);std::string log(std::max(size,1),0);glGetShaderInfoLog(result,size,nullptr,log.data());glDeleteShader(result);throw std::runtime_error("OpenGL shader: "+log);}return result;
}
GLuint program(const char* vertex,const char* fragment) {
    GLuint v=shader(GL_VERTEX_SHADER,vertex),f=0,p=0;
    try{f=shader(GL_FRAGMENT_SHADER,fragment);p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok){GLint size=0;glGetProgramiv(p,GL_INFO_LOG_LENGTH,&size);std::string log(std::max(size,1),0);glGetProgramInfoLog(p,size,nullptr,log.data());throw std::runtime_error("OpenGL program: "+log);}}
    catch(...){if(p)glDeleteProgram(p);if(f)glDeleteShader(f);glDeleteShader(v);throw;}
    glDeleteShader(v);glDeleteShader(f);return p;
}
const char* geometry_vertex=R"GLSL(#version 330 core
layout(location=0) in vec3 position;layout(location=1) in vec2 uv;layout(location=2) in vec2 lightUV;
layout(location=3) in vec3 normal;layout(location=4) in vec4 color;
uniform mat4 model,viewProjection,view,tcMatrix;uniform mat3 normalMatrix;
uniform int tcGen;out vec2 texcoord;out vec3 reflection;out vec4 vertexColor;out float fogDepth;
void main(){vec3 worldPosition=(model*vec4(position,1)).xyz;vertexColor=color;
vec3 eyePosition=(view*vec4(worldPosition,1)).xyz,t=vec3(uv,0);
if(tcGen==1)t=vec3(lightUV,0);else if(tcGen==2||tcGen==3){
vec3 eyeNormal=normalize(mat3(view)*normalize(normalMatrix*normal)),r=reflect(normalize(eyePosition),eyeNormal);
if(tcGen==2){float m=2.0*sqrt(r.x*r.x+r.y*r.y+(r.z+1.0)*(r.z+1.0));t=vec3(r.xy/max(m,0.00001)+0.5,0);}else t=r;}
vec4 generated=tcMatrix*vec4(t,1);texcoord=generated.xy;reflection=generated.xyz;
fogDepth=abs(eyePosition.z);gl_Position=viewProjection*vec4(worldPosition,1);}
)GLSL";
const char* geometry_fragment=R"GLSL(#version 330 core
in vec2 texcoord;in vec3 reflection;in vec4 vertexColor;in float fogDepth;out vec4 outputColor;
uniform sampler2D image;uniform samplerCube cubeImage;
uniform int rgbGen,alphaTest;uniform bool useCube,uiTint,legacyClamp;
uniform vec4 constantColor,entityColor;
uniform vec4 fogColor;uniform vec3 fogParameters;uniform int fogMode;uniform bool fogEnabled;
void main(){vec4 c=useCube?texture(cubeImage,reflection):texture(image,legacyClamp?clamp(texcoord,0.0,1.0):texcoord);
vec3 rgb=constantColor.rgb;if(rgbGen==1)rgb=vertexColor.rgb;else if(rgbGen==3)rgb*=entityColor.rgb;
float alpha=constantColor.a;if(rgbGen==1&&constantColor.a==1.0)alpha=vertexColor.a;
c*=vec4(rgb,alpha*entityColor.a);if(uiTint&&rgbGen!=3)c.rgb*=entityColor.rgb;
if((alphaTest==1&&!(c.a>0))||(alphaTest==2&&!(c.a<128.0/255.0))||(alphaTest==3&&!(c.a>=128.0/255.0))||(alphaTest==4&&c.a!=0))discard;
if(fogEnabled){float f=1.0;if(fogMode==2048)f=exp(-fogParameters.z*fogDepth);
else if(fogMode==2049){float d=fogParameters.z*fogDepth;f=exp(-d*d);}
else f=(fogParameters.y-fogDepth)/max(fogParameters.y-fogParameters.x,0.00001);c.rgb=mix(fogColor.rgb,c.rgb,clamp(f,0.0,1.0));}
outputColor=c;}
)GLSL";
const char* fullscreen_vertex=R"GLSL(#version 330 core
out vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2-1,0,1);})GLSL";
const char* post_fragment=R"GLSL(#version 330 core
in vec2 uv;out vec4 outputColor;uniform sampler2D image,bloomImage;uniform sampler2DArray historyImage;uniform int operation,historyLayer;uniform vec2 direction;uniform float gammaValue,bloomStrength,historyAlpha;
void main(){if(operation==4){outputColor=vec4(texture(historyImage,vec3(uv,historyLayer)).rgb,historyAlpha);return;}vec3 c=texture(image,uv).rgb;
if(operation==1){float l=max(c.r,max(c.g,c.b));c*=max(l-0.8,0.0)/max(l,0.0001);}
else if(operation==2){c*=0.227027;c+=texture(image,uv+direction*1.384615).rgb*0.316216;c+=texture(image,uv-direction*1.384615).rgb*0.316216;
c+=texture(image,uv+direction*3.230769).rgb*0.070270;c+=texture(image,uv-direction*3.230769).rgb*0.070270;}
else if(operation==0){
c+=texture(bloomImage,uv).rgb*bloomStrength;c=pow(max(c,vec3(0)),vec3(1.0/gammaValue));}outputColor=vec4(c,1);})GLSL";
}

namespace {
using NameHash=RenderNameHash;
using NameEqual=RenderNameEqual;
template<class T> using Names=std::unordered_map<std::string,T,NameHash,NameEqual>;
struct Geometry {
    GLuint vao{},vbo{},ebo{}; GLsizei count{}; GLenum primitive{GL_TRIANGLES},index_type{GL_UNSIGNED_INT};
    std::vector<Vertex> vertices,scratch;
    std::vector<std::uint32_t> owned_indices;
    std::span<const std::uint32_t> indices32;
    std::span<const std::uint16_t> indices16;
    bool posed{},deformed{},owns_vbo{true};
    std::uint64_t generation{};
    GLsizeiptr vertex_capacity{},index_capacity{};
    Geometry()=default;Geometry(const Geometry&)=delete;Geometry&operator=(const Geometry&)=delete;
    Geometry(Geometry&& o) noexcept :vao(std::exchange(o.vao,0)),vbo(std::exchange(o.vbo,0)),ebo(std::exchange(o.ebo,0)),count(o.count),primitive(o.primitive),index_type(o.index_type),vertices(std::move(o.vertices)),scratch(std::move(o.scratch)),owned_indices(std::move(o.owned_indices)),indices32(o.indices32),indices16(o.indices16),posed(o.posed),deformed(o.deformed),owns_vbo(o.owns_vbo),generation(o.generation),vertex_capacity(o.vertex_capacity),index_capacity(o.index_capacity){}
    ~Geometry(){if(vao)glDeleteVertexArrays(1,&vao);if(owns_vbo&&vbo)glDeleteBuffers(1,&vbo);if(ebo)glDeleteBuffers(1,&ebo);}
    void upload(std::span<const Vertex> input,std::span<const std::uint32_t> indices,bool dynamic=false) {
        indices32=indices;upload_data(input,indices.data(),indices.size_bytes(),indices.size(),GL_UNSIGNED_INT,dynamic);
    }
    void upload16(std::span<const Vertex> input,std::span<const std::uint16_t> indices,bool dynamic=false) {
        indices16=indices;upload_data(input,indices.data(),indices.size_bytes(),indices.size(),GL_UNSIGNED_SHORT,dynamic);
    }
    void upload_world(GLuint buffer,std::size_t first,std::span<const std::uint16_t> indices) {
        owns_vbo=false;vbo=buffer;indices16=indices;count=GLsizei(indices.size());index_type=GL_UNSIGNED_SHORT;
        glGenVertexArrays(1,&vao);glGenBuffers(1,&ebo);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,GLsizeiptr(indices.size_bytes()),indices.data(),GL_STATIC_DRAW);
        const int sizes[]{3,2,2,3,4};const std::size_t offsets[]{offsetof(Vertex,position),offsetof(Vertex,uv),offsetof(Vertex,light_uv),offsetof(Vertex,normal),offsetof(Vertex,color)};
        for(GLuint i=0;i<5;++i){glEnableVertexAttribArray(i);glVertexAttribPointer(i,sizes[i],GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(first*sizeof(Vertex)+offsets[i]));}
    }
    void upload_data(std::span<const Vertex> input,const void* indices,std::size_t index_bytes,std::size_t index_count,GLenum type,bool dynamic) {
        if(vertices.data()!=input.data())vertices.assign(input.begin(),input.end());index_type=type;
        glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);glGenBuffers(1,&ebo);glBindVertexArray(vao);
        vertex_capacity=GLsizeiptr(input.size_bytes());index_capacity=GLsizeiptr(index_bytes);
        glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,vertex_capacity,input.data(),dynamic?GL_STREAM_DRAW:GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,index_capacity,indices,GL_STATIC_DRAW);count=static_cast<GLsizei>(index_count);
        const int sizes[]{3,2,2,3,4};const std::size_t offsets[]{offsetof(Vertex,position),offsetof(Vertex,uv),offsetof(Vertex,light_uv),offsetof(Vertex,normal),offsetof(Vertex,color)};
        for(GLuint i=0;i<5;++i){glEnableVertexAttribArray(i);glVertexAttribPointer(i,sizes[i],GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsets[i]));}
    }
};
enum Uniform { Model,ViewProjection,View,NormalMatrix,TcGen,TcMatrix,Image,CubeImage,RgbGen,AlphaTest,UseCube,
    ConstantColor,EntityColor,FogColor,FogParameters,FogMode,FogEnabled,UiTint,LegacyClamp,UniformCount };
constexpr const char* uniform_names[]{"model","viewProjection","view","normalMatrix","tcGen","tcMatrix","image","cubeImage","rgbGen","alphaTest","useCube",
    "constantColor","entityColor","fogColor","fogParameters","fogMode","fogEnabled","uiTint","legacyClamp"};
}
struct TextureResource {
    GLuint id{},fbo{},depth{},capture_color{};bool owned{},capture{},mipmaps{};int width{},height{};Vec3 center{};bool center_set{};
    MaterialTextureKind kind{MaterialTextureKind::none};std::string image_name;bool picmip{},compression{};
    std::unique_ptr<VideoTexture> video;std::vector<std::uint8_t> video_pixels;double timestamp{-std::numeric_limits<double>::infinity()};
    TextureResource()=default;TextureResource(const TextureResource&)=delete;TextureResource&operator=(const TextureResource&)=delete;
    TextureResource(TextureResource&& o) noexcept :id(std::exchange(o.id,0)),fbo(std::exchange(o.fbo,0)),depth(std::exchange(o.depth,0)),capture_color(std::exchange(o.capture_color,0)),owned(o.owned),capture(o.capture),mipmaps(o.mipmaps),width(o.width),height(o.height),center(o.center),center_set(o.center_set),kind(o.kind),image_name(std::move(o.image_name)),picmip(o.picmip),compression(o.compression),video(std::move(o.video)),video_pixels(std::move(o.video_pixels)),timestamp(o.timestamp){}
    ~TextureResource(){if(owned&&id)glDeleteTextures(1,&id);if(capture_color)glDeleteTextures(1,&capture_color);if(fbo)glDeleteFramebuffers(1,&fbo);if(depth)glDeleteRenderbuffers(1,&depth);}
};
struct ObjectKey {
    std::uint64_t id{};const RenderObject* fallback{};
    bool operator==(const ObjectKey&)const=default;
};
struct ObjectKeyHash {
    std::size_t operator()(ObjectKey key)const noexcept{return std::hash<std::uint64_t>{}(key.id)^std::hash<const RenderObject*>{}(key.fallback);}
};
ObjectKey object_key(const RenderObject& object){return {object.lighting_id,object.lighting_id?nullptr:&object};}
struct ObjectState {
    GLuint colors_buffer{};std::vector<std::array<std::uint8_t,4>> colors;
    std::vector<Vec3> positions,normals;std::uint64_t pose_frame{},color_frame{},prepared{};
    Vec3 origin{};Matrix transform{};MaterialLightGrid mode{MaterialLightGrid::off};bool valid{};
    const Mesh* mesh{};std::uint64_t geometry_generation{};
    void bind_mesh(const RenderObject& object) {
        if(mesh==object.mesh&&geometry_generation==object.geometry_generation)return;
        // Equal vertex counts do not make a replacement mesh's pose/colors reusable.
        mesh=object.mesh;geometry_generation=object.geometry_generation;valid=false;
        pose_frame=color_frame=std::numeric_limits<std::uint64_t>::max();
    }
    ObjectState()=default;ObjectState(const ObjectState&)=delete;ObjectState&operator=(const ObjectState&)=delete;
    ~ObjectState(){if(colors_buffer)glDeleteBuffers(1,&colors_buffer);}
};

struct Renderer::State {
    AssetStore& assets;MaterialLibrary& materials;Settings settings;
    OriginalGraphicsOptions original;
    bool resources_dirty{};std::uint64_t generation{std::numeric_limits<std::uint64_t>::max()};
    GLuint geometry_program{},post_program{},screen_vao{},white{},yellow{},black{};
    std::array<GLint,UniformCount> uniforms{};
    GLint post_image{},post_bloom{},post_operation{},post_direction{},post_gamma{},post_strength{};
    GLint post_history{},post_history_layer{},post_history_alpha{};GLuint history_texture{},history_fbo{},history_depth{};int history_size{};
    std::uint8_t history_write{};std::uint32_t history_warmup{},history_order{},history_tick{};std::uint64_t effect_serial{};bool history_capture{};
    int width{},height{},samples{1};float max_anisotropy{1};
    struct RootCull {CullingBounds bounds;std::uint64_t camera_serial{};bool visible{};};
    std::unordered_map<const Bounds*,RootCull> root_culls;
    std::unordered_map<const Mesh*,CullingBounds> part_culls;
    std::vector<unsigned> leaf_culls,node_culls;
    std::vector<std::int32_t> node_parents,leaf_parents;
    std::vector<std::uint32_t> node_stamps,leaf_stamps;
    ModelFrustum frustum;std::uint64_t camera_serial{};
    GLuint scene_fbo{},scene_color{},scene_depth{},resolve_fbo{},resolve_color{},blur_fbo[2]{},blur_color[2]{};
    GLuint world_vertex_buffer{};
    Names<GLuint> textures;
    Names<GLuint> font_atlases;
    std::unordered_map<std::string,std::array<const Material*,2>,ShaderNameHash,ShaderNameEqual> material_cache;
    std::unordered_map<const MaterialTexture*,TextureResource> sources;
    std::unordered_map<const MaterialPass*,std::vector<GLuint>> animations;
    std::vector<TextureResource*> capture_sources;
    std::string reflection_path;
    bool reflections_pending{};
    GLint max_cube_size{1};
    int capture_display_height{1};
    TextureResource* capturing_source{};
    GLuint samplers[18]{};
    GLuint font_sampler{};
    std::unordered_map<const Mesh*,Geometry> meshes;
    std::unordered_map<ObjectKey,ObjectState,ObjectKeyHash> object_states;
    std::uint64_t render_frame{},prepare_frame{};
    struct WorldDraw {Geometry geometry;const Material* material{};GLuint lightmap{};std::int32_t lightmap_index{};Bounds bounds;Vec3 center;};
    const Level* level{};std::vector<WorldDraw> surfaces;std::vector<GLuint> lightmaps;
    std::vector<std::uint32_t> surface_stamps;std::uint32_t frame_stamp{};
    Geometry quad,particle_triangle;
    std::vector<std::size_t> world_order;std::array<Vec3,2> sky_centers{};std::array<float,2> sky_radii{};
    RenderCamera camera;RenderFog fog;double seconds{};std::uint32_t tick_milliseconds{};Matrix view_projection{render_identity};
    State(AssetStore& a,MaterialLibrary& m,const Settings& s):assets(a),materials(m),settings(s){}
    ~State() {
        release_targets();
        glDeleteSamplers(18,samplers);
        if(font_sampler)glDeleteSamplers(1,&font_sampler);
        if(history_texture)glDeleteTextures(1,&history_texture);if(history_fbo)glDeleteFramebuffers(1,&history_fbo);if(history_depth)glDeleteRenderbuffers(1,&history_depth);
        for(auto& t:textures)glDeleteTextures(1,&t.second);
        for(auto t:lightmaps)glDeleteTextures(1,&t);
        const GLuint ts[]{white,yellow,black};glDeleteTextures(3,ts);
        if(world_vertex_buffer)glDeleteBuffers(1,&world_vertex_buffer);
        glDeleteVertexArrays(1,&screen_vao);if(geometry_program)glDeleteProgram(geometry_program);if(post_program)glDeleteProgram(post_program);
    }
    void release_targets() {
        const GLuint fb[]{scene_fbo,resolve_fbo,blur_fbo[0],blur_fbo[1]};glDeleteFramebuffers(4,fb);
        glDeleteRenderbuffers(1,&scene_color);glDeleteRenderbuffers(1,&scene_depth);
        const GLuint tx[]{resolve_color,blur_color[0],blur_color[1]};glDeleteTextures(3,tx);
        scene_fbo=scene_color=scene_depth=resolve_fbo=resolve_color=0;std::fill(std::begin(blur_fbo),std::end(blur_fbo),0);std::fill(std::begin(blur_color),std::end(blur_color),0);
    }
    GLuint color_texture(int w,int h) {
        GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,w,h,0,GL_RGBA,GL_FLOAT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);return t;
    }
    static void check_framebuffer() {if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("OpenGL framebuffer incomplete");}
    void resize(int w,int h) {
        if(w<=0||h<=0)return;if(w==width&&h==height&&scene_fbo)return;
        release_targets();width=w;height=h;GLint maximum=1;glGetIntegerv(GL_MAX_SAMPLES,&maximum);samples=std::clamp(settings.msaa,1,maximum);
        glGenFramebuffers(1,&scene_fbo);glBindFramebuffer(GL_FRAMEBUFFER,scene_fbo);glGenRenderbuffers(1,&scene_color);glBindRenderbuffer(GL_RENDERBUFFER,scene_color);
        if(samples>1)glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples,GL_RGBA16F,w,h);else glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA16F,w,h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,scene_color);
        glGenRenderbuffers(1,&scene_depth);glBindRenderbuffer(GL_RENDERBUFFER,scene_depth);
        if(samples>1)glRenderbufferStorageMultisample(GL_RENDERBUFFER,samples,GL_DEPTH24_STENCIL8,w,h);else glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,w,h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,scene_depth);check_framebuffer();
        glGenFramebuffers(1,&resolve_fbo);glBindFramebuffer(GL_FRAMEBUFFER,resolve_fbo);resolve_color=color_texture(w,h);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,resolve_color,0);check_framebuffer();
        glGenFramebuffers(2,blur_fbo);
        for(int i=0;i<2;++i){glBindFramebuffer(GL_FRAMEBUFFER,blur_fbo[i]);blur_color[i]=color_texture(std::max(1,w/2),std::max(1,h/2));glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,blur_color[i],0);check_framebuffer();}
        setup_history();glBindFramebuffer(GL_FRAMEBUFFER,0);
    }
    GLuint texture(std::string_view name,bool picmip=true,bool compression=true) {
        if(name.empty()||NameEqual{}(name,"$whiteimage")||NameEqual{}(name,"whiteimage"))return white;
        if(NameEqual{}(name,"yellowimage"))return yellow;
        std::string key(name);key+=picmip?"|scaled":"|full";key+=compression?"|compressed":"|raw";
        if(auto it=textures.find(key);it!=textures.end())return it->second;
        auto image=reduce_image(image_surface(assets,name),picmip?original.texture_divisor:1);
        GLuint id;glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D,id);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,image->pitch/4);
        glTexImage2D(GL_TEXTURE_2D,0,compression&&original.texture_compression?GL_COMPRESSED_RGBA:GL_RGBA8,image->w,image->h,0,GL_RGBA,GL_UNSIGNED_BYTE,image->pixels);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glGenerateMipmap(GL_TEXTURE_2D);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        if(GLEW_EXT_texture_filter_anisotropic)glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAX_ANISOTROPY_EXT,std::clamp(float(settings.anisotropy),1.0f,max_anisotropy));
        textures.emplace(std::move(key),id);return id;
    }
    void preload_font(std::string_view name) {
        if(font_atlases.find(name)==font_atlases.end())font_atlases.emplace(std::string(name),texture(name,false,false));
    }
    Geometry& mesh(const Mesh& mesh,std::uint64_t generation=0,std::span<const std::array<std::uint8_t,4>> colors={}) {
        auto it=meshes.find(&mesh);if(it!=meshes.end()&&it->second.generation==generation)return it->second;
        if(!colors.empty()&&colors.size()!=mesh.positions.size())throw std::runtime_error("Borrowed mesh vertex-color size mismatch");
        Geometry fresh;Geometry& geometry=it==meshes.end()?fresh:it->second;geometry.generation=generation;geometry.primitive=mesh.primitive==Primitive::triangle_strip?GL_TRIANGLE_STRIP:GL_TRIANGLES;geometry.vertices.resize(mesh.positions.size());
        for(std::size_t i=0;i<mesh.positions.size();++i){std::array<float,4> color{1,1,1,1};if(!colors.empty())for(int j=0;j<4;++j)color[j]=colors[i][j]/255.0f;
            geometry.vertices[i]={mesh.positions[i],mesh.texcoords.at(i),{},mesh.normals.at(i),color};}
        if(it==meshes.end()){geometry.upload16(geometry.vertices,mesh.indices,true);return meshes.emplace(&mesh,std::move(fresh)).first->second;}
        geometry.indices16=mesh.indices;geometry.count=GLsizei(mesh.indices.size());geometry.posed=geometry.deformed=false;glBindVertexArray(geometry.vao);
        glBindBuffer(GL_ARRAY_BUFFER,geometry.vbo);GLsizeiptr vertex_bytes=GLsizeiptr(geometry.vertices.size()*sizeof(Vertex));
        if(vertex_bytes>geometry.vertex_capacity){geometry.vertex_capacity=std::max(vertex_bytes,geometry.vertex_capacity*2);glBufferData(GL_ARRAY_BUFFER,geometry.vertex_capacity,nullptr,GL_STREAM_DRAW);}
        glBufferSubData(GL_ARRAY_BUFFER,0,vertex_bytes,geometry.vertices.data());glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,geometry.ebo);GLsizeiptr index_bytes=GLsizeiptr(mesh.indices.size()*sizeof(mesh.indices[0]));
        if(index_bytes>geometry.index_capacity){geometry.index_capacity=std::max(index_bytes,geometry.index_capacity*2);glBufferData(GL_ELEMENT_ARRAY_BUFFER,geometry.index_capacity,nullptr,GL_STREAM_DRAW);}
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,0,index_bytes,mesh.indices.data());return geometry;
    }
    void preload_material(std::string_view name,bool object_context=true);
    void preload_material(const Material& material);
    const Material* cached_material(std::string_view name,bool object_context=true) const;
    void world(const Level* next,LevelProgress progress=nullptr,void* context=nullptr);
    void material_draw(const Material* material,Geometry& geometry,const Matrix& model,GLuint lightmap,
                       const std::array<float,4>& color,bool interface=false,GLuint image_override=0,std::int32_t lightmap_selector=-1,bool mesh_lighting=false,bool font_atlas=false);
    TextureResource& source(const MaterialTexture& texture,const Material& material);
    void deform(const Material& material,Geometry& geometry,const Matrix& model);
    void update_video(TextureResource& resource);
    void draw_entities(const RenderScene& scene);
    void draw_world();
    void visit_world(std::int32_t index,std::uint32_t mask,std::size_t depth=0);
    void post();
    void begin_generation(std::uint64_t next,const Level* next_level);
    void setup_history();
    void consume_history(const RenderScene& scene);
    void draw_interface(std::span<const InterfaceQuad> interface,bool capture_only=false);
};

Renderer::Renderer(AssetStore& assets,MaterialLibrary& materials,const Settings& settings)
    :state_(std::make_unique<State>(assets,materials,settings)) {
    auto& s=*state_;s.geometry_program=program(geometry_vertex,geometry_fragment);s.post_program=program(fullscreen_vertex,post_fragment);
    for(std::size_t i=0;i<UniformCount;++i)s.uniforms[i]=glGetUniformLocation(s.geometry_program,uniform_names[i]);
    s.post_image=glGetUniformLocation(s.post_program,"image");s.post_bloom=glGetUniformLocation(s.post_program,"bloomImage");
    s.post_operation=glGetUniformLocation(s.post_program,"operation");s.post_direction=glGetUniformLocation(s.post_program,"direction");
    s.post_gamma=glGetUniformLocation(s.post_program,"gammaValue");s.post_strength=glGetUniformLocation(s.post_program,"bloomStrength");
    s.post_history=glGetUniformLocation(s.post_program,"historyImage");s.post_history_layer=glGetUniformLocation(s.post_program,"historyLayer");s.post_history_alpha=glGetUniformLocation(s.post_program,"historyAlpha");
    glGenVertexArrays(1,&s.screen_vao);
    auto pixel=[](std::uint32_t rgba){GLuint id;glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D,id);const std::uint8_t p[]{std::uint8_t(rgba>>24),std::uint8_t(rgba>>16),std::uint8_t(rgba>>8),std::uint8_t(rgba)};
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,p);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);return id;};
    s.white=pixel(0xffffffffu);s.black=pixel(0x000000ffu);
    std::array<std::uint8_t,64> yellow_pixels{};for(int i=0;i<16;++i){yellow_pixels[i*4]=255;yellow_pixels[i*4+1]=240;yellow_pixels[i*4+2]=150;yellow_pixels[i*4+3]=255;}
    glGenTextures(1,&s.yellow);glBindTexture(GL_TEXTURE_2D,s.yellow);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,yellow_pixels.data());glGenerateMipmap(GL_TEXTURE_2D);
    if(GLEW_EXT_texture_filter_anisotropic)glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&s.max_anisotropy);
    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE,&s.max_cube_size);
    SDL_Window* window=SDL_GL_GetCurrentWindow();if(!window)throw std::runtime_error("Cube preparation requires the supplied SDL GL window");
    int display=SDL_GetWindowDisplayIndex(window);SDL_DisplayMode mode{};
    if(display<0||SDL_GetCurrentDisplayMode(display,&mode)!=0||mode.h<=0)throw std::runtime_error("Cannot determine the current display height for original cube sizing");
    s.capture_display_height=mode.h;
    const Vertex v[4]{{{0,0,0},{0,0},{},{0,0,1},{1,1,1,1}},{{1,0,0},{1,0},{},{0,0,1},{1,1,1,1}},
        {{1,1,0},{1,1},{},{0,0,1},{1,1,1,1}},{{0,1,0},{0,1},{},{0,0,1},{1,1,1,1}}};
    static constexpr std::uint32_t i[]{0,1,2,0,2,3};s.quad.vertices.assign(std::begin(v),std::end(v));s.quad.upload(v,i,true);
    static constexpr std::uint32_t tri_indices[]{0,1,2};s.particle_triangle.upload(std::span(v,3),tri_indices,true);
    glGenSamplers(18,s.samplers);
    const int filters[]{s.original.texture_filter,s.original.lightmap_filter,s.original.reflection_filter};
    for(int i=0;i<18;++i){GLuint sampler=s.samplers[i];GLint wrap=i%3==0?GL_REPEAT:i%3==1?GL_CLAMP_TO_BORDER:GL_CLAMP_TO_EDGE;
        glSamplerParameteri(sampler,GL_TEXTURE_WRAP_S,wrap);glSamplerParameteri(sampler,GL_TEXTURE_WRAP_T,wrap);glSamplerParameteri(sampler,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
        int filter=effective_texture_filter(filters[i/6],i%6<3);
        glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,filter==0?GL_NEAREST:filter==1?GL_LINEAR:filter==2?GL_LINEAR_MIPMAP_NEAREST:GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(sampler,GL_TEXTURE_MAG_FILTER,filter==0?GL_NEAREST:GL_LINEAR);
        if(GLEW_EXT_texture_filter_anisotropic)glSamplerParameterf(sampler,GL_TEXTURE_MAX_ANISOTROPY_EXT,std::clamp(float(settings.anisotropy),1.0f,s.max_anisotropy));}
    glGenSamplers(1,&s.font_sampler);glSamplerParameteri(s.font_sampler,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glSamplerParameteri(s.font_sampler,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glSamplerParameteri(s.font_sampler,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_BORDER);glSamplerParameteri(s.font_sampler,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_BORDER);
    glUseProgram(s.geometry_program);glUniform1i(s.uniforms[Image],0);glUniform1i(s.uniforms[CubeImage],1);
    glUseProgram(s.post_program);glUniform1i(s.post_image,0);glUniform1i(s.post_bloom,1);glUniform1i(s.post_history,2);
    glFrontFace(GL_CW); // Original 0040eef2/0040eef7, shared by world, actors and cube/history captures.
}
Renderer::~Renderer()=default;
void Renderer::resize(int width,int height){state_->resize(width,height);}
void Renderer::apply_settings(const Settings& settings) {
    auto& s=*state_;bool msaa=s.settings.msaa!=settings.msaa,aniso=s.settings.anisotropy!=settings.anisotropy;
    if(s.settings.height!=settings.height){if(settings.height<=0)throw std::runtime_error("Selected cube display height must be positive");
        s.capture_display_height=settings.height;s.resources_dirty=true;}
    s.settings=settings;
    if(msaa&&s.width&&s.height){int w=s.width,h=s.height;s.width=0;s.resize(w,h);}
    if(aniso&&GLEW_EXT_texture_filter_anisotropic)for(auto sampler:s.samplers)glSamplerParameterf(sampler,GL_TEXTURE_MAX_ANISOTROPY_EXT,std::clamp(float(settings.anisotropy),1.0f,s.max_anisotropy));
}
void Renderer::apply_original_options(const OriginalGraphicsOptions& options) {
    auto& s=*state_;if(s.original==options)return;
    if(options.texture_divisor<1||options.lightmap_divisor<1||options.reflection_divisor<1||options.motion_blur_size<1||options.motion_blur_frames<2||options.motion_blur_frames>10)
        throw std::runtime_error("Invalid original graphics options");
    s.resources_dirty=s.resources_dirty||s.original.texture_divisor!=options.texture_divisor||s.original.lightmap_divisor!=options.lightmap_divisor||
        s.original.reflection_divisor!=options.reflection_divisor||s.original.texture_compression!=options.texture_compression||
        s.original.lightmap_compression!=options.lightmap_compression||
        (effective_texture_filter(s.original.reflection_filter,true)>=2)!=(effective_texture_filter(options.reflection_filter,true)>=2);
    bool history_changed=s.original.motion_blur!=options.motion_blur||s.original.motion_blur_size!=options.motion_blur_size||s.original.motion_blur_frames!=options.motion_blur_frames;
    s.original=options;
    const int filters[]{options.texture_filter,options.lightmap_filter,options.reflection_filter};
    for(int domain=0;domain<3;++domain)for(int i=0;i<6;++i){int filter=effective_texture_filter(filters[domain],i<3);
        glSamplerParameteri(s.samplers[domain*6+i],GL_TEXTURE_MIN_FILTER,filter==0?GL_NEAREST:filter==1?GL_LINEAR:filter==2?GL_LINEAR_MIPMAP_NEAREST:GL_LINEAR_MIPMAP_LINEAR);
        glSamplerParameteri(s.samplers[domain*6+i],GL_TEXTURE_MAG_FILTER,filter==0?GL_NEAREST:GL_LINEAR);}
    if(history_changed)s.setup_history();
}
void Renderer::State::world(const Level* next,LevelProgress progress,void* context) {
    if(level==next)return;surfaces.clear();surface_stamps.clear();for(auto id:lightmaps)glDeleteTextures(1,&id);lightmaps.clear();
    if(world_vertex_buffer)glDeleteBuffers(1,&world_vertex_buffer);world_vertex_buffer=0;level=next;if(!level)return;
    constexpr std::size_t map_bytes=128*128*3;
    std::unique_ptr<std::uint8_t[]> normalized_maps(new std::uint8_t[level->lightmaps.size()*map_bytes]);
    for(std::size_t m=0;m<level->lightmaps.size();++m)
        normalize_lightmap(level->lightmaps[m],std::span<std::uint8_t,map_bytes>(normalized_maps.get()+m*map_bytes,map_bytes));
    if(progress)progress(context,0.4f);
    lightmaps.reserve(level->lightmaps.size());int divisor=std::min(original.lightmap_divisor,128),size=128/divisor;
    std::vector<std::uint8_t> reduced;if(divisor!=1)reduced.resize(std::size_t(size)*size*3);
    for(std::size_t m=0;m<level->lightmaps.size();++m){const auto* pixels=normalized_maps.get()+m*map_bytes;
        if(divisor!=1){reduce_lightmap(std::span<const std::uint8_t,map_bytes>(pixels,map_bytes),divisor,reduced);pixels=reduced.data();}
        GLuint id;glActiveTexture(GL_TEXTURE0);glBindSampler(0,0);glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D,id);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glTexImage2D(GL_TEXTURE_2D,0,original.lightmap_compression?GL_COMPRESSED_RGB:GL_RGB8,size,size,0,GL_RGB,GL_UNSIGNED_BYTE,pixels);glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_BORDER);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_BORDER);lightmaps.push_back(id);}
    if(progress)progress(context,0.5f);
    for(const auto& shader:level->shaders)preload_material(shader.name,false);
    std::vector<Vertex> world_vertices;world_vertices.reserve(level->vertices.size());for(const auto& vertex:level->vertices)world_vertices.push_back(from_world(vertex));
    glGenBuffers(1,&world_vertex_buffer);glBindBuffer(GL_ARRAY_BUFFER,world_vertex_buffer);glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(world_vertices.size()*sizeof(Vertex)),world_vertices.data(),GL_STATIC_DRAW);
    if(progress)progress(context,0.6f);
    surfaces.reserve(level->surfaces.size());surface_stamps.resize(level->surfaces.size());
    std::array<Bounds,2> sky_bounds{};std::array<bool,2> has_sky{};
    for(const auto& surface:level->surfaces) {
        WorldDraw draw;draw.material=cached_material(level->shaders.at(surface.shader).name,false);if(!draw.material)throw std::runtime_error("PL surface has no runtime material");
        if(surface.type==SurfaceType::patch){tessellate(*level,surface,draw.geometry.vertices,draw.geometry.owned_indices);draw.geometry.upload(draw.geometry.vertices,draw.geometry.owned_indices);}
        else if(surface.type==SurfaceType::planar||surface.type==SurfaceType::triangle_soup){
            auto indices=std::span(level->indices).subspan(surface.first_index,surface.index_count);for(auto index:indices)if(index>=surface.vertex_count)throw std::runtime_error("PL surface index outside vertex range");
            if(draw.material->deforms.empty())draw.geometry.upload_world(world_vertex_buffer,surface.first_vertex,indices);
            else{draw.geometry.vertices.assign(world_vertices.begin()+surface.first_vertex,world_vertices.begin()+surface.first_vertex+surface.vertex_count);draw.geometry.upload16(draw.geometry.vertices,indices,true);}
        }else throw std::runtime_error("Unsupported typed PL surface");
        if(!draw.material->deforms.empty())draw.geometry.scratch.resize(draw.geometry.vertices.size());
        draw.bounds={Vec3{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity()},
            Vec3{-std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}};
        for(std::uint32_t i=0;i<surface.vertex_count;++i){auto p=level->vertices.at(surface.first_vertex+i).position;
            draw.bounds.minimum={std::min(draw.bounds.minimum.x,p.x),std::min(draw.bounds.minimum.y,p.y),std::min(draw.bounds.minimum.z,p.z)};
            draw.bounds.maximum={std::max(draw.bounds.maximum.x,p.x),std::max(draw.bounds.maximum.y,p.y),std::max(draw.bounds.maximum.z,p.z)};}
        draw.center=(draw.bounds.minimum+draw.bounds.maximum)*0.5f;
        bool has_capture=false;for(const auto& pass:draw.material->passes)if(pass.source_mode==MaterialSourceMode::cube&&source(pass.texture,*draw.material).capture){has_capture=true;break;}
        if(has_capture){Bounds bounds=draw.bounds;
            if(surface.type==SurfaceType::patch){bounds.minimum={INFINITY,INFINITY,INFINITY};bounds.maximum={-INFINITY,-INFINITY,-INFINITY};
                for(const auto& vertex:draw.geometry.vertices){auto p=vertex.position;
                    bounds.minimum={std::min(bounds.minimum.x,p.x),std::min(bounds.minimum.y,p.y),std::min(bounds.minimum.z,p.z)};
                    bounds.maximum={std::max(bounds.maximum.x,p.x),std::max(bounds.maximum.y,p.y),std::max(bounds.maximum.z,p.z)};}}
            Vec3 center=(bounds.minimum+bounds.maximum)*0.5f;
            for(const auto& pass:draw.material->passes)if(pass.source_mode==MaterialSourceMode::cube){auto& resource=source(pass.texture,*draw.material);if(resource.capture){
                resource.center=resource.center_set?cube_capture_center(resource.center,center):center;resource.center_set=true;}}}
        if(draw.material->sky!=MaterialSky::none){unsigned group=draw.material->sky==MaterialSky::normal?0:1;
            if(!has_sky[group]){sky_bounds[group]=draw.bounds;has_sky[group]=true;}
            else{auto& b=sky_bounds[group];b.minimum={std::min(b.minimum.x,draw.bounds.minimum.x),std::min(b.minimum.y,draw.bounds.minimum.y),std::min(b.minimum.z,draw.bounds.minimum.z)};
                b.maximum={std::max(b.maximum.x,draw.bounds.maximum.x),std::max(b.maximum.y,draw.bounds.maximum.y),std::max(b.maximum.z,draw.bounds.maximum.z)};}}
        draw.lightmap_index=surface.lightmap;draw.lightmap=surface.lightmap<0?white:lightmaps.at(surface.lightmap);surfaces.push_back(std::move(draw));
    }
    for(unsigned i=0;i<2;++i){sky_centers[i]=(sky_bounds[i].minimum+sky_bounds[i].maximum)*0.5f;sky_radii[i]=has_sky[i]?original_length(sky_centers[i]-sky_bounds[i].minimum):0;}
    world_order.resize(surfaces.size());for(std::size_t i=0;i<world_order.size();++i)world_order[i]=i;
    std::stable_sort(world_order.begin(),world_order.end(),[&](auto a,auto b){return surfaces[a].material->sort<surfaces[b].material->sort;});
    leaf_culls.resize(level->leaves.size());node_culls.resize(level->nodes.size());node_stamps.assign(level->nodes.size(),0);leaf_stamps.assign(level->leaves.size(),0);
    node_parents.assign(level->nodes.size(),-1);leaf_parents.assign(level->leaves.size(),-1);
    for(std::size_t i=0;i<level->leaves.size();++i){auto cache=level->leaves[i].bounds.type;if(cache>=6)throw std::runtime_error("Serialized BSP leaf rejected-plane cache is outside the original table");leaf_culls[i]=cache;}
    for(std::size_t i=0;i<level->nodes.size();++i){auto cache=level->nodes[i].bounds.type;if(cache>=6)throw std::runtime_error("Serialized BSP node rejected-plane cache is outside the original table");node_culls[i]=cache;
        for(auto child:level->nodes[i].children){if(child>=0)node_parents.at(child)=std::int32_t(i);else leaf_parents.at(~child)=std::int32_t(i);}}
}
void Renderer::prepare(const RenderScene& scene,std::span<const InterfaceQuad> interface) {
    auto& s=*state_;
    glActiveTexture(GL_TEXTURE0);glBindSampler(0,0);
    s.begin_generation(scene.generation,scene.level);
    s.world(scene.level);
    ++s.prepare_frame;
    for(const auto& object:scene.objects){if(object.mesh)s.mesh(*object.mesh,object.geometry_generation,object.vertex_colors);
        if(object.root_bounds){auto& root=s.root_culls[object.root_bounds];if(std::memcmp(&root.bounds.bounds,object.root_bounds,sizeof(Bounds))!=0)root.bounds=cache_bounds(*object.root_bounds);root.camera_serial=0;}
        if(object.mesh){Bounds bounds=object.mesh->bones.empty()?object.mesh->bounds:Bounds{{-11,-49.08399963378906f,-11},{11,49.08399963378906f,11}};
            auto [it,added]=s.part_culls.try_emplace(object.mesh);if(added||std::memcmp(&it->second.bounds,&bounds,sizeof(Bounds))!=0)it->second=cache_bounds(bounds);}
        if(object.material_override)s.preload_material(*object.material_override);else s.preload_material(object.material);
        if(object.mesh){auto* material=object.material_override?object.material_override:s.cached_material(object.material);if(material&&!material->deforms.empty())s.meshes.at(object.mesh).scratch.resize(object.mesh->positions.size());
            bool grid=object.vertex_colors.empty()&&requires_grid_colors(material)&&scene.level&&!scene.level->light_grid.samples.empty();
            if(!object.mesh->bones.empty()||grid){auto& state=s.object_states.try_emplace(object_key(object)).first->second;state.prepared=s.prepare_frame;
                state.bind_mesh(object);
                if(!object.mesh->bones.empty()){state.positions.resize(object.mesh->positions.size());state.normals.resize(object.mesh->positions.size());}
                if(grid&&state.colors.size()!=object.mesh->positions.size()){state.colors.resize(object.mesh->positions.size(),{255,255,255,255});state.valid=false;
                    if(!state.colors_buffer)glGenBuffers(1,&state.colors_buffer);glBindBuffer(GL_ARRAY_BUFFER,state.colors_buffer);
                    glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(state.colors.size()*sizeof(state.colors[0])),nullptr,GL_STREAM_DRAW);}
            }
        }}
    for(const auto& particle:scene.particles)s.preload_material(particle.material);
    for(const auto& q:interface){if(q.font_atlas){s.preload_font(q.shader);continue;}s.preload_material(q.shader,q.shader.find("textures")!=std::string_view::npos);}
    std::erase_if(s.object_states,[&](const auto& entry){return entry.second.prepared!=s.prepare_frame;});
    if(s.reflections_pending&&!s.reflection_path.empty())prepare_reflections(scene,s.reflection_path);
}
void Renderer::State::post() {
    glBindFramebuffer(GL_READ_FRAMEBUFFER,scene_fbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,resolve_fbo);
    glBlitFramebuffer(0,0,width,height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glUseProgram(post_program);glBindVertexArray(screen_vao);
    glBindSampler(0,0);glBindSampler(1,0);glBindSampler(2,0);
    auto draw=[&](GLuint target,GLuint texture,int operation,int w,int h,float dx,float dy){glBindFramebuffer(GL_FRAMEBUFFER,target);glViewport(0,0,w,h);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
        glUniform1i(post_operation,operation);glUniform2f(post_direction,dx,dy);glDrawArrays(GL_TRIANGLES,0,3);};
    if(settings.bloom){int w=std::max(1,width/2),h=std::max(1,height/2);draw(blur_fbo[0],resolve_color,1,w,h,0,0);
        for(int i=0;i<4;++i){draw(blur_fbo[1],blur_color[0],2,w,h,1.0f/w,0);draw(blur_fbo[0],blur_color[1],2,w,h,0,1.0f/h);}}
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,settings.bloom?blur_color[0]:black);
    glUniform1f(post_gamma,settings.gamma);glUniform1f(post_strength,settings.bloom?0.25f:0);draw(0,resolve_color,0,width,height,0,0);
}

void Renderer::State::visit_world(std::int32_t index,std::uint32_t mask,std::size_t depth) {
    if(depth>level->nodes.size())throw std::runtime_error("Cyclic BSP visibility tree");
    if(index>=0){auto ordinal=std::size_t(index);if(node_stamps.at(ordinal)!=frame_stamp)return;const auto& node=level->nodes.at(ordinal);
        if(!frustum.visible(node.bounds,node_culls[ordinal],mask))return;
        visit_world(node.children[0],mask,depth+1);visit_world(node.children[1],mask,depth+1);
    }else{auto ordinal=std::size_t(~index);if(leaf_stamps.at(ordinal)!=frame_stamp)return;const auto& leaf=level->leaves.at(ordinal);
        if(!frustum.visible(leaf.bounds,leaf_culls[ordinal],mask))return;
        for(std::uint32_t i=0;i<leaf.surface_count;++i){auto surface=level->leaf_surfaces.at(leaf.first_surface+i);surface_stamps.at(surface)=frame_stamp;}}
}
void Renderer::State::draw_world() {
    if(!level)return;if(++frame_stamp==0){std::fill(surface_stamps.begin(),surface_stamps.end(),0);std::fill(node_stamps.begin(),node_stamps.end(),0);std::fill(leaf_stamps.begin(),leaf_stamps.end(),0);frame_stamp=1;}
    if(leaf_culls.size()!=level->leaves.size())throw std::runtime_error("Renderer.prepare required after leaf culling data change");
    auto leaf=leaf_at(*level,camera.position);int cluster=leaf<0?-1:level->leaves.at(leaf).cluster;
    if(level->leaves.empty())std::fill(surface_stamps.begin(),surface_stamps.end(),frame_stamp);
    else {for(std::size_t index=0;index<level->leaves.size();++index) {const auto& leaf=level->leaves[index];
            if(cluster>=0&&leaf.cluster>=0&&!level->visibility.visible(cluster,leaf.cluster))continue;leaf_stamps[index]=frame_stamp;
            for(std::int32_t parent=leaf_parents[index];parent>=0;parent=node_parents.at(parent)){if(node_stamps.at(parent)==frame_stamp)break;node_stamps[parent]=frame_stamp;}}
        if(level->nodes.empty())for(std::size_t i=0;i<level->leaves.size();++i)visit_world(~std::int32_t(i),63);
        else visit_world(0,63);}
    for(std::size_t i:world_order)if(surface_stamps[i]==frame_stamp||surfaces[i].material->sky!=MaterialSky::none) {
        auto& draw=surfaces[i];Matrix model=render_identity;
        if(draw.material->sky!=MaterialSky::none){unsigned group=draw.material->sky==MaterialSky::normal?0:1;float radius=sky_radii[0]>0?sky_radii[0]:sky_radii[group];
            if(radius<=0)continue;float scale=camera.far_plane/radius-0.0001f,zscale=group==1?scale*0.5f:scale;
            model[0]=model[5]=scale;model[10]=zscale;model[12]=camera.position.x-scale*sky_centers[group].x;model[13]=camera.position.y-scale*sky_centers[group].y;model[14]=camera.position.z-zscale*sky_centers[group].z;}
        material_draw(draw.material,draw.geometry,model,draw.lightmap,{1,1,1,1},false,0,draw.lightmap_index);
    }
}
void Renderer::render(const RenderScene& scene,std::span<const InterfaceQuad> interface) {
    auto& s=*state_;if(s.width<=0||s.height<=0)return;
    if(s.resources_dirty||s.generation!=scene.generation)prepare(scene,interface);
    if(s.level!=scene.level)throw std::runtime_error("Renderer.prepare required after level change");
    s.camera=scene.camera;s.fog=scene.fog;s.seconds=scene.time_seconds;s.tick_milliseconds=scene.tick_milliseconds;++s.render_frame;
    if(s.effect_serial!=scene.player_effect_serial){s.effect_serial=scene.player_effect_serial;s.history_write=0;s.history_warmup=0;}
    float fov=scene.camera.reference_fov_degrees>0?scene.camera.reference_fov_degrees:s.settings.reference_fov,aspect=float(s.width)/s.height;
    s.view_projection=multiply(perspective(fov,aspect,scene.camera.near_plane,1.0e11f),scene.camera.view);
    constexpr float half_degree=std::bit_cast<float>(std::uint32_t{0x3c0efa35});
    s.frustum=model_frustum(s.camera,std::atan(std::tan(static_cast<long double>(fov)*half_degree)*0.75L*aspect),1/aspect);++s.camera_serial;
    glBindFramebuffer(GL_FRAMEBUFFER,s.scene_fbo);glViewport(0,0,s.width,s.height);glDisable(GL_SCISSOR_TEST);glDisable(GL_FRAMEBUFFER_SRGB);
    glClearColor(scene.clear_color[0],scene.clear_color[1],scene.clear_color[2],scene.clear_color[3]);glDepthMask(GL_TRUE);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    glEnable(GL_MULTISAMPLE);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glUseProgram(s.geometry_program);
    s.draw_world();s.draw_entities(scene);s.consume_history(scene);
    s.draw_interface(interface);s.post();
}
void Renderer::State::draw_interface(std::span<const InterfaceQuad> interface,bool capture_only) {
    auto& s=*this;const Matrix projection=s.view_projection;
    auto ui_extent=interface_extent(s.width,s.height);int ui_width=ui_extent[0],ui_height=ui_extent[1];glViewport((s.width-ui_width)/2,(s.height-ui_height)/2,ui_width,ui_height);
    s.view_projection=render_identity;s.view_projection[0]=2.0f/1024;s.view_projection[5]=-2.0f/768;s.view_projection[12]=-1;s.view_projection[13]=1;
    bool full_viewport=false,pixel_projection=false;
    for(const auto& q:interface) {
        if(capture_only&&!q.history_capture_visible)continue;
        RenderRect rect=interface_rect(q,s.width,s.height);
        if(rect.width<=0||rect.height<=0)continue;
        if(q.mask_legacy_margins){auto bands=interface_margin_bands(s.width,s.height,ui_width,ui_height);
            glEnable(GL_SCISSOR_TEST);glClearColor(0,0,0,1);for(const auto& band:bands)if(band[2]>0&&band[3]>0){glScissor(band[0],band[1],band[2],band[3]);glClear(GL_COLOR_BUFFER_BIT);}glDisable(GL_SCISSOR_TEST);}
        bool native_viewport=q.full_viewport||q.native_bar_fraction.has_value()||q.drawable_pixel_coordinates;
        if(native_viewport!=full_viewport){full_viewport=native_viewport;if(full_viewport)glViewport(0,0,s.width,s.height);else glViewport((s.width-ui_width)/2,(s.height-ui_height)/2,ui_width,ui_height);}
        if(pixel_projection!=q.drawable_pixel_coordinates){pixel_projection=q.drawable_pixel_coordinates;
            s.view_projection[0]=2.0f/(pixel_projection?s.width:1024);s.view_projection[5]=-2.0f/(pixel_projection?s.height:768);}
        Matrix model=interface_model(q,rect);
        const Vec2 uv[4]{{q.uv.x,q.uv.y},{q.uv.x+q.uv.width,q.uv.y},{q.uv.x+q.uv.width,q.uv.y+q.uv.height},{q.uv.x,q.uv.y+q.uv.height}};
        for(int i=0;i<4;++i)s.quad.vertices[i].uv=uv[i];glBindBuffer(GL_ARRAY_BUFFER,s.quad.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,4*sizeof(Vertex),s.quad.vertices.data());
        if(q.font_atlas){auto image=s.font_atlases.find(q.shader);if(image==s.font_atlases.end())throw std::runtime_error("Renderer.prepare required after font atlas change");
            s.material_draw(nullptr,s.quad,model,s.white,q.color,true,image->second,-1,false,true);
        }else s.material_draw(s.cached_material(q.shader,q.shader.find("textures")!=std::string_view::npos),s.quad,model,s.white,q.color,true);
    }
    const Vec2 uv[4]{{0,0},{1,0},{1,1},{0,1}};for(int i=0;i<4;++i)s.quad.vertices[i].uv=uv[i];
    glBindBuffer(GL_ARRAY_BUFFER,s.quad.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,4*sizeof(Vertex),s.quad.vertices.data());s.view_projection=projection;
}
void Renderer::State::begin_generation(std::uint64_t next,const Level* next_level) {
    if(generation==next&&level==next_level&&!resources_dirty)return;
    if(generation!=next||level!=next_level)reflection_path.clear();
    meshes.clear();level=nullptr;capture_sources.clear();sources.clear();animations.clear();object_states.clear();reflections_pending=true;
    root_culls.clear();part_culls.clear();leaf_culls.clear();node_culls.clear();node_parents.clear();leaf_parents.clear();leaf_stamps.clear();node_stamps.clear();
    if(resources_dirty){material_cache.clear();font_atlases.clear();for(auto& t:textures)glDeleteTextures(1,&t.second);textures.clear();}
    generation=next;resources_dirty=false;
}
void Renderer::prepare_level(const Level& level,std::uint64_t generation,LevelProgress progress,void* context) {
    auto& s=*state_;s.begin_generation(generation,&level);s.world(&level,progress,context);
}
void Renderer::render_loading(std::span<const InterfaceQuad> interface) {
    auto& s=*state_;if(s.width<=0||s.height<=0)return;
    for(const auto& q:interface){if(q.font_atlas)s.preload_font(q.shader);else s.preload_material(q.shader,q.shader.find("textures")!=std::string_view::npos);}
    glBindFramebuffer(GL_FRAMEBUFFER,s.scene_fbo);glViewport(0,0,s.width,s.height);glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    s.draw_interface(interface);s.post();
}
void Renderer::State::setup_history() {
    if(width<=0||height<=0)return;int side=original.motion_blur_size;while(side>width)side/=2;while(side>height)side/=2;side=std::max(side,1);
    if(history_texture)glDeleteTextures(1,&history_texture);if(history_fbo)glDeleteFramebuffers(1,&history_fbo);if(history_depth)glDeleteRenderbuffers(1,&history_depth);
    history_size=side;glGenTextures(1,&history_texture);glBindTexture(GL_TEXTURE_2D_ARRAY,history_texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RGB8,side,side,original.motion_blur_frames,0,GL_RGB,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_BORDER);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_BORDER);
    glGenFramebuffers(1,&history_fbo);glGenRenderbuffers(1,&history_depth);glBindRenderbuffer(GL_RENDERBUFFER,history_depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,side,side);
    glBindFramebuffer(GL_FRAMEBUFFER,history_fbo);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,history_depth);
    history_write=0;history_warmup=0;
}
void Renderer::State::consume_history(const RenderScene& scene) {
    const auto count=std::uint32_t(original.motion_blur_frames);
    if(!original.motion_blur||!scene.player_effect||!scene.gameplay_frame||history_capture||capturing_source||history_warmup<count)return;
    glUseProgram(post_program);glBindVertexArray(screen_vao);glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D_ARRAY,history_texture);glBindSampler(2,0);glUniform1i(post_operation,4);glUniform1f(post_history_alpha,original.motion_blur_alpha/float(count));
    for(std::uint32_t k=history_order,end=k+count;k<end;++k){glUniform1i(post_history_layer,GLint(k%count));glDrawArrays(GL_TRIANGLES,0,3);}
}
void Renderer::after_present(const RenderScene& scene,std::span<const InterfaceQuad> interface) {
    auto& s=*state_;if(!s.original.motion_blur||!scene.player_effect||!scene.gameplay_frame||s.history_capture||s.capturing_source||!s.history_texture)return;
    if(!(float(std::uint32_t(scene.tick_milliseconds-s.history_tick))>1000*s.original.motion_blur_wait_seconds))return;
    s.history_tick=scene.tick_milliseconds;s.history_capture=true;const int width=s.width,height=s.height;GLint target;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&target);
    s.width=s.height=s.history_size;glBindFramebuffer(GL_FRAMEBUFFER,s.history_fbo);glFramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,s.history_texture,0,s.history_write%std::uint32_t(s.original.motion_blur_frames));s.check_framebuffer();
    glViewport(0,0,s.history_size,s.history_size);glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);glClearColor(scene.clear_color[0],scene.clear_color[1],scene.clear_color[2],1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    s.draw_world();s.draw_entities(scene);s.draw_interface(interface,true);
    ++s.history_write;++s.history_warmup;++s.history_order;s.width=width;s.height=height;s.history_capture=false;
    glBindFramebuffer(GL_FRAMEBUFFER,GLuint(target));glViewport(0,0,width,height);
}

namespace {


GLenum blend_factor(MaterialBlendFactor factor) {
    switch(factor){case MaterialBlendFactor::zero:return GL_ZERO;case MaterialBlendFactor::one:return GL_ONE;
        case MaterialBlendFactor::src_color:return GL_SRC_COLOR;case MaterialBlendFactor::one_minus_src_color:return GL_ONE_MINUS_SRC_COLOR;
        case MaterialBlendFactor::dst_color:return GL_DST_COLOR;case MaterialBlendFactor::one_minus_dst_color:return GL_ONE_MINUS_DST_COLOR;
        case MaterialBlendFactor::src_alpha:return GL_SRC_ALPHA;case MaterialBlendFactor::one_minus_src_alpha:return GL_ONE_MINUS_SRC_ALPHA;}
    throw std::runtime_error("Invalid parsed material blend factor");
}
}
void Renderer::State::preload_material(std::string_view name,bool object_context) {
    if(name.empty())return;
    auto cached=material_cache.find(name);
    const Material* material=materials.find_instance(name);
    if(!material&&cached!=material_cache.end())material=cached->second[object_context];
    if(!material)material=&materials.resolve(name,object_context);
    if(cached==material_cache.end())
        cached=material_cache.try_emplace(std::string(name),std::array<const Material*,2>{}).first;
    cached->second[object_context]=material;
    preload_material(*material);
}
void Renderer::State::preload_material(const Material& material) {
    for(const auto& pass:material.passes){
        source(pass.texture,material);
        if(pass.animation&&animations.find(&pass)==animations.end()){auto& ids=animations[&pass];for(const auto& frame:pass.animation->frames)ids.push_back(texture(frame,material.picmip,material.compression));}
    }
}
const Material* Renderer::State::cached_material(std::string_view name,bool object_context)const {
    if(name.empty())return nullptr;auto it=material_cache.find(name);
    if(it==material_cache.end()||!it->second[object_context])throw std::runtime_error("Renderer.prepare did not preload material "+std::string(name));
    return it->second[object_context];
}
TextureResource& Renderer::State::source(const MaterialTexture& input,const Material& material) {
    if(auto it=sources.find(&input);it!=sources.end()){
        auto& resource=it->second;
        if(resource.kind==input.kind&&input.kind==MaterialTextureKind::image){
            if(resource.image_name!=input.image||resource.picmip!=material.picmip||resource.compression!=material.compression){
                resource.id=texture(input.image,material.picmip,material.compression);resource.image_name=input.image;resource.picmip=material.picmip;resource.compression=material.compression;
            }
            return resource;
        }
        if(resource.kind==input.kind&&input.kind!=MaterialTextureKind::avi)return resource;
        if(resource.kind==input.kind&&resource.image_name==input.image)return resource;
        if(resource.capture)std::erase(capture_sources,&resource);
        sources.erase(it);
    }
    TextureResource resource;resource.kind=input.kind;resource.image_name=input.image;resource.picmip=material.picmip;resource.compression=material.compression;
    switch(input.kind) {
        case MaterialTextureKind::none:case MaterialTextureKind::white:case MaterialTextureKind::lightmap:resource.id=white;break;
        case MaterialTextureKind::image:resource.id=texture(input.image,material.picmip,material.compression);break;
        case MaterialTextureKind::combined:{
            auto atlas=combined_image(assets,input);
            glGenTextures(1,&resource.id);resource.owned=true;glBindTexture(GL_TEXTURE_2D,resource.id);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,atlas->pitch/4);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,atlas->w,atlas->h,0,GL_RGBA,GL_UNSIGNED_BYTE,atlas->pixels);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glGenerateMipmap(GL_TEXTURE_2D);break;}
        case MaterialTextureKind::avi:
            glGenTextures(1,&resource.id);resource.owned=true;glBindTexture(GL_TEXTURE_2D,resource.id);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGB5,1,1,0,GL_RGB,GL_UNSIGNED_BYTE,nullptr);break;
        case MaterialTextureKind::cube_capture:case MaterialTextureKind::cube_faces:{
            if(input.cube_size<=0)throw std::runtime_error("Invalid cubemap capture size");
            glGenTextures(1,&resource.id);resource.owned=true;glBindTexture(GL_TEXTURE_CUBE_MAP,resource.id);
            int size=std::min(input.cube_size,int(max_cube_size));
            while(size>capture_display_height)size>>=1;
            resource.width=resource.height=std::max(1,size/(material.picmip?original.reflection_divisor:1));
            resource.mipmaps=effective_texture_filter(original.reflection_filter,material.mipmaps)>=2;
            for(unsigned face=0;face<6;++face) {
                if(input.kind==MaterialTextureKind::cube_faces){DecodedLayout layout;auto image=image_surface(assets,input.cube_faces[face],input.cube_size,&layout);
                    std::size_t needed=std::size_t(resource.width)*resource.height*3;
                    std::vector<std::uint8_t> raw(needed);cube_face_bytes(*image,layout,raw);
                    glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
                    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,GL_RGB5,resource.width,resource.height,0,GL_RGB,GL_UNSIGNED_BYTE,raw.data());
                }else glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,GL_RGB5,resource.width,resource.height,0,GL_RGB,GL_UNSIGNED_BYTE,nullptr);
            }
            int filter=effective_texture_filter(original.reflection_filter,material.mipmaps);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,filter==0?GL_NEAREST:filter==1?GL_LINEAR:filter==2?GL_LINEAR_MIPMAP_NEAREST:GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,filter==0?GL_NEAREST:GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            if(input.kind==MaterialTextureKind::cube_faces){if(resource.mipmaps)glGenerateMipmap(GL_TEXTURE_CUBE_MAP);}
            else resource.capture=true;
            break;}
    }
    auto& added=sources.emplace(&input,std::move(resource)).first->second;
    // A shader/object born after scene publication still needs its cube faces and mipmaps.
    if(added.capture){capture_sources.push_back(&added);reflections_pending=true;}
    return added;
}
void Renderer::State::material_draw(const Material* material,Geometry& geometry,const Matrix& model,GLuint lightmap,
                                   const std::array<float,4>& color,bool interface,GLuint image_override,std::int32_t lightmap_selector,bool mesh_lighting,bool font_atlas) {
    if(material&&material->passes.empty())return;glUseProgram(geometry_program);glBindVertexArray(geometry.vao);
    glUniformMatrix4fv(uniforms[Model],1,GL_FALSE,model.data());glUniformMatrix4fv(uniforms[ViewProjection],1,GL_FALSE,view_projection.data());
    glUniformMatrix4fv(uniforms[View],1,GL_FALSE,interface?render_identity.data():camera.view.data());
    if(material&&std::any_of(material->passes.begin(),material->passes.end(),[](const MaterialPass& p){return p.tc_gen==MaterialTcGen::environment||p.tc_gen==MaterialTcGen::cube;})){
        auto normal_matrix=material_normal_matrix(model);
        glUniformMatrix3fv(uniforms[NormalMatrix],1,GL_FALSE,normal_matrix.data());
    }
    glUniform4fv(uniforms[EntityColor],1,color.data());
    glUniform1i(uniforms[FogEnabled],!interface&&fog.enabled&&(!material||material->fog));glUniform1i(uniforms[FogMode],fog.mode);
    glUniform4fv(uniforms[FogColor],1,fog.color.data());glUniform3f(uniforms[FogParameters],fog.start,fog.end,fog.density);
    if(interface||!material||material->cull==MaterialCull::none)glDisable(GL_CULL_FACE);else{glEnable(GL_CULL_FACE);glCullFace(material->cull==MaterialCull::front?GL_FRONT:GL_BACK);}
    // Original 0040f579–0040f591: glPolygonOffset(authored * 0.5f, authored).
    if(material&&material->polygon_offset!=0){glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(material->polygon_offset*0.5f,material->polygon_offset);}else glDisable(GL_POLYGON_OFFSET_FILL);
    if(material)deform(*material,geometry,model);
    MaterialPass untextured;untextured.blend=interface;untextured.blend_source=MaterialBlendFactor::src_alpha;untextured.blend_destination=MaterialBlendFactor::one_minus_src_alpha;
    untextured.depth_write=!interface;untextured.rgb_gen=MaterialRgbGen::entity;
    std::span<const MaterialPass> passes=material?std::span<const MaterialPass>(material->passes):std::span<const MaterialPass>(&untextured,1);
    for(const auto& pass:passes) {
        if(pass.source_mode==MaterialSourceMode::cube&&(!original.reflections||capturing_source))continue;
        if(!interface&&pass.depth_test)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
        glDepthFunc(pass.depth_function==MaterialDepthFunc::equal?GL_EQUAL:GL_LEQUAL);glDepthMask(!interface&&pass.depth_write?GL_TRUE:GL_FALSE);
        if(pass.blend||interface){glEnable(GL_BLEND);glBlendFunc(blend_factor(pass.blend_source),blend_factor(pass.blend_destination));}else glDisable(GL_BLEND);
        int alpha=0;if(pass.alpha_test){switch(*pass.alpha_test){case MaterialAlphaTest::gt0:alpha=1;break;case MaterialAlphaTest::lt128:alpha=2;break;case MaterialAlphaTest::ge128:alpha=3;break;case MaterialAlphaTest::eq0:alpha=4;break;}}
        glUniform1i(uniforms[AlphaTest],alpha);int rgb=material?0:3;std::array<float,4> constant{pass.color[0]/255.0f,pass.color[1]/255.0f,pass.color[2]/255.0f,pass.color[3]/255.0f};
        switch(pass.rgb_gen){case MaterialRgbGen::vertex:rgb=1;break;case MaterialRgbGen::lighting_diffuse:rgb=mesh_lighting?1:0;break;
            case MaterialRgbGen::wave:{float w=float(original_integer(std::clamp(wave_value(pass.rgb_wave,seconds),0.0f,1.0f)*255))/255;constant[0]=constant[1]=constant[2]=w;break;}
            default:break;}
        glUniform1i(uniforms[UiTint],interface);
        glUniform1i(uniforms[RgbGen],rgb);glUniform4fv(uniforms[ConstantColor],1,constant.data());
        auto matrix=texture_matrix(pass,seconds,interface?render_identity:camera.view);glUniformMatrix4fv(uniforms[TcMatrix],1,GL_FALSE,matrix.data());
        bool is_lightmap=pass.source_mode==MaterialSourceMode::lightmap,cube=pass.source_mode==MaterialSourceMode::cube;
        bool has_lightmap=lightmap_selector>=0&&(is_lightmap||(pass.source_mode==MaterialSourceMode::environment&&pass.texture.kind==MaterialTextureKind::lightmap));
        bool white_fallback=is_lightmap&&!has_lightmap&&(pass.texture.kind==MaterialTextureKind::lightmap||pass.texture.kind==MaterialTextureKind::none);
        glUniform1i(uniforms[TcGen],pass.tc_gen==MaterialTcGen::environment?2:pass.tc_gen==MaterialTcGen::cube?3:is_lightmap&&!white_fallback?1:0);
        GLuint image=image_override?image_override:white;
        if(material) {
            auto it=sources.find(&pass.texture);if(it==sources.end())throw std::runtime_error("Material texture was not preloaded");auto& resource=it->second;
            if(resource.kind==MaterialTextureKind::avi&&!has_lightmap&&!white_fallback&&pass.source_mode!=MaterialSourceMode::none)update_video(resource);
            image=has_lightmap?lightmap:white_fallback||pass.source_mode==MaterialSourceMode::none?white:resource.id;
            if(pass.animation&&!cube&&!has_lightmap&&!white_fallback){const auto& frames=animations.at(&pass);if(!frames.empty()){double index=std::floor(seconds*pass.animation->frequency);auto count=static_cast<long long>(frames.size());auto frame=static_cast<long long>(std::fmod(index,double(count)));if(frame<0)frame+=count;image=frames[frame];}}
        }
        glUniform1i(uniforms[UseCube],cube);
        int sampler=(has_lightmap?6:cube?12:0)+(material&&!material->mipmaps?3:0)+static_cast<int>(pass.animation&&!cube&&!has_lightmap&&!white_fallback?pass.animation->wrap:pass.texture.wrap);
        if(has_lightmap)sampler=6+(material&&!material->mipmaps?3:0)+1;
        if(cube)sampler=material&&!material->mipmaps?17:14;
        if(pass.texture.kind==MaterialTextureKind::avi)sampler=3+static_cast<int>(pass.texture.wrap);
        glUniform1i(uniforms[LegacyClamp],font_atlas||(!cube&&sampler%3==1));
        if(cube){glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_CUBE_MAP,image);glBindSampler(1,samplers[sampler]);}
        else{glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,image);glBindSampler(0,font_atlas?font_sampler:samplers[!material?5:sampler]);}
        glDrawElements(geometry.primitive,geometry.count,geometry.index_type,nullptr);
    }
}

void Renderer::State::draw_entities(const RenderScene& scene) {
    auto& s=*this;
    for(const auto& object:scene.objects)if(object.visible&&object.mesh&&!object.mesh->indices.empty()) {
        if(object.culling_enabled&&object.culling_transform){
            if(object.root_bounds){auto& root=s.root_culls.at(object.root_bounds);if(root.camera_serial!=s.camera_serial){root.visible=s.frustum.visible(root.bounds,*object.culling_transform);root.camera_serial=s.camera_serial;}if(!root.visible)continue;}
            if(object.frustum_cull&&!s.frustum.visible(s.part_culls.at(object.mesh),*object.culling_transform))continue;
        }
        auto it=s.meshes.find(object.mesh);if(it==s.meshes.end())throw std::runtime_error("Renderer.prepare required after mesh change");
        auto& geometry=it->second;
        if(object.geometry_generation&&geometry.generation!=object.geometry_generation)s.mesh(*object.mesh,object.geometry_generation,object.vertex_colors);
        const auto* material=object.material_override?object.material_override:s.cached_material(object.material);
        bool mesh_lighting=object.vertex_colors.empty()&&requires_grid_colors(material)&&scene.level&&!scene.level->light_grid.samples.empty();
        ObjectState* state=nullptr;if(!object.mesh->bones.empty()||mesh_lighting){auto cached=s.object_states.find(object_key(object));
            if(cached==s.object_states.end())throw std::runtime_error("Renderer.prepare required after object lighting/pose change");state=&cached->second;state->bind_mesh(object);}
        if(!object.bones.empty()||geometry.posed) {
            if(!object.bones.empty()&&state->pose_frame!=s.render_frame){skin_mesh(*object.mesh,object.bones,state->positions,state->normals);state->pose_frame=s.render_frame;}
            for(std::size_t i=0;i<geometry.vertices.size();++i){
                geometry.vertices[i].position=object.bones.empty()?object.mesh->positions[i]:state->positions[i];
                geometry.vertices[i].normal=object.bones.empty()?object.mesh->normals[i]:state->normals[i];
            }
            glBindBuffer(GL_ARRAY_BUFFER,geometry.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,static_cast<GLsizeiptr>(geometry.vertices.size()*sizeof(Vertex)),geometry.vertices.data());geometry.posed=!object.bones.empty();geometry.deformed=false;
        }
        glBindVertexArray(geometry.vao);
        if(mesh_lighting){
            Vec3 origin=object.lighting_origin.value_or(Vec3{object.transform[12],object.transform[13],object.transform[14]});auto mode=material->lightgrid;
            bool moved=origin.x!=state->origin.x||origin.y!=state->origin.y||origin.z!=state->origin.z;
            bool changed=!state->valid||state->mode!=mode||moved||(!object.lighting_id&&state->transform!=object.transform)||
                (!object.mesh->bones.empty()&&state->color_frame!=s.render_frame);
            if(changed){std::array<std::uint8_t,4> uniform{255,255,255,255};
                if(mode==MaterialLightGrid::center||mode==MaterialLightGrid::interpolate_once)uniform=grid_diffuse(scene.level->light_grid,scene.level->bounds.minimum,origin,{0,0,1});
                if(mode==MaterialLightGrid::interpolate_once){constexpr float gain=std::bit_cast<float>(std::uint32_t{0x3b9a33ce});Vec3 c{uniform[0]*gain,uniform[1]*gain,uniform[2]*gain};c=c/std::max({1.0f,c.x,c.y,c.z});
                    uniform[0]=std::uint8_t(original_integer(c.x*255));uniform[1]=std::uint8_t(original_integer(c.y*255));uniform[2]=std::uint8_t(original_integer(c.z*255));}
                for(std::size_t i=0;i<state->colors.size();++i){auto color=mode==MaterialLightGrid::interpolate?
                    grid_diffuse(scene.level->light_grid,scene.level->bounds.minimum,transform_point(object.transform,geometry.vertices[i].position),transform_vector(object.transform,geometry.vertices[i].normal)):uniform;
                    if(mode==MaterialLightGrid::interpolate_once)color[3]=state->colors[i][3];state->colors[i]=color;}
                glBindBuffer(GL_ARRAY_BUFFER,state->colors_buffer);glBufferSubData(GL_ARRAY_BUFFER,0,GLsizeiptr(state->colors.size()*sizeof(state->colors[0])),state->colors.data());
                state->origin=origin;state->transform=object.transform;state->mode=mode;state->valid=true;state->color_frame=s.render_frame;
            }
            glBindBuffer(GL_ARRAY_BUFFER,state->colors_buffer);glVertexAttribPointer(4,4,GL_UNSIGNED_BYTE,GL_TRUE,4,nullptr);
        }else{glBindBuffer(GL_ARRAY_BUFFER,geometry.vbo);glVertexAttribPointer(4,4,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offsetof(Vertex,color)));}
        s.material_draw(material,geometry,object.transform,s.white,object.color,false,0,-1,mesh_lighting);
    }
    Matrix camera_axes=inverse_rigid(s.camera.view);
    for(const auto& particle:scene.particles) {
        if(particle.triangle) {
            auto& geometry=s.particle_triangle;Vec3 normal=original_normalized(cross(particle.triangle_positions[1]-particle.triangle_positions[0],particle.triangle_positions[2]-particle.triangle_positions[0]));
            for(int i=0;i<3;++i){geometry.vertices[i].position=particle.triangle_positions[i];geometry.vertices[i].uv=particle.triangle_uv[i];geometry.vertices[i].normal=normal;geometry.vertices[i].color=particle.color;}
            glBindBuffer(GL_ARRAY_BUFFER,geometry.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,3*sizeof(Vertex),geometry.vertices.data());
            s.material_draw(s.cached_material(particle.material),geometry,render_identity,s.white,{1,1,1,1});continue;
        }
        if(particle.size.x<=0||particle.size.y<=0)continue;
        Matrix axes=particle.autosprite?camera_axes:particle.orientation;float c=std::cos(particle.rotation),sn=std::sin(particle.rotation);
        Vec3 right{axes[0],axes[1],axes[2]},up{axes[4],axes[5],axes[6]};
        Vec3 r=(right*c+up*sn)*particle.size.x,u=(up*c-right*sn)*particle.size.y;
        Matrix model=render_identity;model[0]=r.x;model[1]=r.y;model[2]=r.z;model[4]=u.x;model[5]=u.y;model[6]=u.z;
        Vec3 normal=original_normalized(cross(right,up));model[8]=normal.x;model[9]=normal.y;model[10]=normal.z;
        Vec3 corner=particle.position-(r+u)*0.5f;model[12]=corner.x;model[13]=corner.y;model[14]=corner.z;
        for(auto& vertex:s.quad.vertices)vertex.color=particle.color;glBindBuffer(GL_ARRAY_BUFFER,s.quad.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,4*sizeof(Vertex),s.quad.vertices.data());
        s.material_draw(s.cached_material(particle.material),s.quad,model,s.white,{1,1,1,1});
    }
    for(auto& vertex:s.quad.vertices)vertex.color={1,1,1,1};glBindBuffer(GL_ARRAY_BUFFER,s.quad.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,4*sizeof(Vertex),s.quad.vertices.data());
}
void Renderer::State::deform(const Material& material,Geometry& geometry,const Matrix& model) {
    if(material.deforms.empty()){if(geometry.deformed){glBindBuffer(GL_ARRAY_BUFFER,geometry.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,static_cast<GLsizeiptr>(geometry.vertices.size()*sizeof(Vertex)),geometry.vertices.data());geometry.deformed=false;}return;}
    const auto primitive=geometry.primitive==GL_TRIANGLE_STRIP?Primitive::triangle_strip:Primitive::triangles;
    if(geometry.index_type==GL_UNSIGNED_SHORT)
        deform_vertices(material,primitive,geometry.indices16,geometry.vertices,geometry.scratch,model,camera,seconds);
    else deform_vertices(material,primitive,geometry.indices32,geometry.vertices,geometry.scratch,model,camera,seconds);
    glBindBuffer(GL_ARRAY_BUFFER,geometry.vbo);glBufferSubData(GL_ARRAY_BUFFER,0,static_cast<GLsizeiptr>(geometry.scratch.size()*sizeof(Vertex)),geometry.scratch.data());geometry.deformed=true;
}
void Renderer::State::update_video(TextureResource& resource) {
    if(!resource.video)resource.video=std::make_unique<VideoTexture>(assets,resource.image_name,tick_milliseconds);
    resource.video->update_clock(tick_milliseconds);auto frame=resource.video->frame();
    int w=avi_atlas_side(frame.width),h=avi_atlas_side(frame.height);
    if(frame.width>w||frame.height>h)throw std::runtime_error("Original AVI material upload exceeds its fixed atlas");
    if(resource.width==w&&resource.height==h&&(frame.rgba.empty()||resource.timestamp==frame.seconds))return;
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,resource.id);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    if(resource.width!=w||resource.height!=h){resource.video_pixels.assign(std::size_t(w)*h*3,0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGB5,w,h,0,GL_RGB,GL_UNSIGNED_BYTE,resource.video_pixels.data());resource.width=w;resource.height=h;}
    if(!frame.rgba.empty()&&resource.timestamp!=frame.seconds){
        avi_frame_bytes(frame,resource.video_pixels);
        glTexSubImage2D(GL_TEXTURE_2D,0,(w-frame.width)/2,(h-frame.height)/2,frame.width,frame.height,GL_RGB,GL_UNSIGNED_BYTE,resource.video_pixels.data());resource.timestamp=frame.seconds;
    }
}
void Renderer::prepare_reflections(const RenderScene& scene,std::string_view bsp_path) {
    auto& s=*state_;
    if(s.level!=scene.level||s.generation!=scene.generation||s.resources_dirty)
        throw std::runtime_error("Cube cache preparation requires the committed prepared level and scene");
    std::string key(bsp_path);for(char& c:key){if(c=='\\')c='/';else if(c>='A'&&c<='Z')c=char(c-'A'+'a');}
    if(key.ends_with(".pl"))key.resize(key.size()-3);
    if(!s.reflections_pending&&s.reflection_path==key)return;
    s.reflection_path=key;
    if(!s.original.reflections)return;
    // Separate clockwise captures from previously generated counterclockwise pixels.
    std::string shape="cw/q"+std::to_string(s.original.reflection_divisor),part="s";
    for(std::size_t first=0;first<s.capture_sources.size();){std::size_t end=first+1;int side=s.capture_sources[first]->width;
        while(end<s.capture_sources.size()&&s.capture_sources[end]->width==side)++end;
        std::string run=std::to_string(side)+"x"+std::to_string(end-first);
        if(part.size()+run.size()+1>200){shape+="/"+part;part="s";}
        if(part.size()>1)part+="_";part+=run;first=end;
    }
    shape+="/"+(part=="s"?std::string("empty"):part);
    PrivateCubeCache private_cache(user_data_directory()/"cache",key,shape);
    auto cache=private_cache.read(s.original.reflection_divisor);
    bool owned_cache=cache.has_value();
    if(cache){try{validate_cube_cache_geometry(*cache);
            if(cache->entries.size()!=s.capture_sources.size())
                throw CubeCacheValidationError("Owned cube cache record count differs from its capture registry");
            for(std::size_t i=0;i<s.capture_sources.size();++i){const auto* resource=s.capture_sources[i];
                owned_cube_cache_faces(*cache,i,resource->center,resource->width);}}
        catch(const CubeCacheValidationError& error){std::cerr<<"Invalid owned cube cache payload for "<<key<<": "<<error.what()<<'\n';cache.reset();}}
    if(!cache){owned_cache=false;cache=read_cube_cache(s.assets,"textures/cubemaps/"+key,s.original.reflection_divisor);
        if(cache){validate_cube_cache_geometry(*cache);bool compatible=true;
            for(const auto* resource:s.capture_sources){auto offset=cube_cache_offset(*cache,resource->center);
                if(cube_cache_region_bytes(*cache,offset)!=std::size_t(resource->width)*resource->height*18){compatible=false;break;}}
            if(!compatible){std::cerr<<"Original cube cache record size differs from effective capture size for "<<key<<'\n';cache.reset();}
        }
    }
    if(cache){
        glActiveTexture(GL_TEXTURE0);glBindSampler(0,0);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        for(std::size_t i=0;i<s.capture_sources.size();++i){auto* resource=s.capture_sources[i];
            auto bytes=owned_cache?cube_cache_faces_at_offset(*cache,cache->entries[i].offset,resource->width):
                cube_cache_faces(*cache,resource->center,resource->width);
            std::size_t face_bytes=std::size_t(resource->width)*resource->height*3;glBindTexture(GL_TEXTURE_CUBE_MAP,resource->id);
            for(unsigned face=0;face<6;++face)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,GL_RGB5,resource->width,resource->height,0,GL_RGB,GL_UNSIGNED_BYTE,bytes.data()+face*face_bytes);
            if(resource->mipmaps)glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        }
        s.reflections_pending=false;return;
    }
    auto output=private_cache.begin_output();
    std::vector<std::uint8_t> pixels;std::size_t largest=0;
    for(const auto* resource:s.capture_sources)largest=std::max(largest,std::size_t(resource->width)*resource->height*3);
    pixels.resize(largest);
    const auto saved_camera=s.camera;const auto saved_projection=s.view_projection;const auto saved_frustum=s.frustum;
    const auto saved_fog=s.fog;const double saved_seconds=s.seconds;
    const auto saved_tick=s.tick_milliseconds;
    GLint saved_draw{},saved_read{},saved_viewport[4]{},saved_program{},saved_active{},saved_pack{},saved_pack_row{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&saved_draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&saved_read);
    glGetIntegerv(GL_VIEWPORT,saved_viewport);glGetIntegerv(GL_CURRENT_PROGRAM,&saved_program);glGetIntegerv(GL_ACTIVE_TEXTURE,&saved_active);
    glGetIntegerv(GL_PACK_ALIGNMENT,&saved_pack);glGetIntegerv(GL_PACK_ROW_LENGTH,&saved_pack_row);
    auto restore=[&](){s.capturing_source=nullptr;s.camera=saved_camera;s.view_projection=saved_projection;s.frustum=saved_frustum;
        s.fog=saved_fog;s.seconds=saved_seconds;s.tick_milliseconds=saved_tick;++s.camera_serial;glBindFramebuffer(GL_DRAW_FRAMEBUFFER,saved_draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,saved_read);
        glViewport(saved_viewport[0],saved_viewport[1],saved_viewport[2],saved_viewport[3]);glUseProgram(saved_program);glActiveTexture(saved_active);
        glPixelStorei(GL_PACK_ALIGNMENT,saved_pack);glPixelStorei(GL_PACK_ROW_LENGTH,saved_pack_row);};
    try {
        s.camera=scene.camera;s.fog=scene.fog;s.seconds=scene.time_seconds;s.tick_milliseconds=scene.tick_milliseconds;
        s.camera.near_plane=4;s.camera.far_plane=20000;s.camera.reference_fov_degrees=90;
        glDisable(GL_SCISSOR_TEST);glDisable(GL_FRAMEBUFFER_SRGB);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glUseProgram(s.geometry_program);
        glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);
        const Vec3 directions[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        const Vec3 ups[6]{{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}};
        for(auto* pointer:s.capture_sources){auto& resource=*pointer;s.capturing_source=&resource;output.begin_cube(resource.center);
            if(!resource.fbo){glGenFramebuffers(1,&resource.fbo);glGenRenderbuffers(1,&resource.depth);
                glBindRenderbuffer(GL_RENDERBUFFER,resource.depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,resource.width,resource.height);
                glGenTextures(1,&resource.capture_color);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,resource.capture_color);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,resource.width,resource.height,0,GL_RGB,GL_UNSIGNED_BYTE,nullptr);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER,resource.fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,resource.capture_color,0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,resource.depth);State::check_framebuffer();}
            for(unsigned face=0;face<6;++face){Vec3 f=directions[face],r=cross(f,ups[face]),u=cross(r,f),eye=resource.center;
                s.camera.position=eye;s.camera.view={r.x,u.x,-f.x,0,r.y,u.y,-f.y,0,r.z,u.z,-f.z,0,-dot(r,eye),-dot(u,eye),dot(f,eye),1};
                s.camera.frame0={r.x,r.y,r.z,0,u.x,u.y,u.z,0,-f.x,-f.y,-f.z,0,eye.x,eye.y,eye.z,1};
                s.frustum=model_frustum(s.camera,90.0L*std::bit_cast<float>(std::uint32_t{0x3c0efa35}),1);++s.camera_serial;
                auto projection=perspective(90,4.0f/3,4,1.0e11f);projection[0]=projection[5]=1;s.view_projection=multiply(projection,s.camera.view);
                glBindFramebuffer(GL_FRAMEBUFFER,resource.fbo);glViewport(0,0,resource.width,resource.height);glDepthMask(GL_TRUE);
                glClearColor(scene.clear_color[0],scene.clear_color[1],scene.clear_color[2],1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
                s.draw_world();s.draw_entities(scene);
                glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_CUBE_MAP,resource.id);glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,0,0,0,0,resource.width,resource.height);
                auto bytes=std::span(pixels).first(std::size_t(resource.width)*resource.height*3);
                glReadPixels(0,0,resource.width,resource.height,GL_RGB,GL_UNSIGNED_BYTE,bytes.data());output.face(bytes);
            }
            glBindTexture(GL_TEXTURE_CUBE_MAP,resource.id);if(resource.mipmaps)glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        }
        output.commit(s.original.reflection_divisor);s.reflections_pending=false;
    }catch(...){restore();throw;}
    restore();
}

} // namespace pusu

