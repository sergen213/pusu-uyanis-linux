#include "renderer_vulkan_private.hpp"
#include "renderer_vulkan_shaders.hpp"
#include <squish.h>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>

namespace pusu {
namespace {
using namespace vk_detail;
void checked(VkResult result,const char* operation) { if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": Vulkan error "+std::to_string(result)); }
std::uint64_t hash_bytes(std::span<const std::byte> bytes,std::uint64_t h=1469598103934665603ull) { for(auto b:bytes)h=(h^std::to_integer<unsigned char>(b))*1099511628211ull;return h; }
template<class T> std::uint64_t hash_value(const T& v,std::uint64_t h) { return hash_bytes(std::as_bytes(std::span(&v,1)),h); }
std::uint64_t hash_text(std::string_view s,std::uint64_t h) { return hash_bytes(std::as_bytes(std::span(s.data(),s.size())),hash_value(s.size(),h)); }
std::uint64_t texture_signature(const MaterialTexture& t) {
    std::uint64_t h=1469598103934665603ull;h=hash_value(t.kind,h);h=hash_value(t.wrap,h);h=hash_text(t.image,h);h=hash_text(t.fallback_image,h);h=hash_value(t.cube_size,h);h=hash_value(t.width,h);h=hash_value(t.height,h);
    for(auto& f:t.cube_faces)h=hash_text(f,h);for(auto& l:t.layers){h=hash_text(l.name,h);h=hash_text(l.image,h);h=hash_value(l.x,h);h=hash_value(l.y,h);}return h;
}
std::uint64_t material_signature(const Material& m) {
    auto h=hash_text(m.name,1469598103934665603ull);h=hash_value(m.cull,h);h=hash_value(m.lightgrid,h);h=hash_value(m.sort,h);h=hash_value(m.sky,h);h=hash_value(m.polygon_offset,h);
    h=hash_value(m.mipmaps,h);h=hash_value(m.picmip,h);h=hash_value(m.compression,h);h=hash_value(m.fog,h);
    for(auto& d:m.deforms){h=hash_value(d.kind,h);h=hash_value(d.spread,h);h=hash_value(d.wave,h);}
    for(auto& p:m.passes){h=hash_value(texture_signature(p.texture),h);h=hash_value(p.source_mode,h);h=hash_value(p.blend,h);h=hash_value(p.depth_test,h);h=hash_value(p.depth_write,h);h=hash_value(p.blend_source,h);h=hash_value(p.blend_destination,h);h=hash_value(p.depth_function,h);h=hash_value(p.alpha_test.has_value(),h);if(p.alpha_test)h=hash_value(*p.alpha_test,h);h=hash_value(p.rgb_gen,h);h=hash_value(p.color,h);h=hash_value(p.rgb_wave,h);h=hash_value(p.tc_gen,h);for(auto& t:p.tc_mods){h=hash_value(t.kind,h);h=hash_value(t.values,h);}h=hash_value(p.animation.has_value(),h);if(p.animation){h=hash_value(p.animation->frequency,h);h=hash_value(p.animation->wrap,h);for(auto& f:p.animation->frames)h=hash_text(f,h);}}
    return h;
}
std::uint32_t mip_count(VkExtent2D e,bool enabled) { return enabled?std::bit_width(std::max(e.width,e.height)):1; }
std::uint8_t rgb5(std::uint8_t c) { unsigned v=(unsigned(c)*31+127)/255;return std::uint8_t((v*255+15)/31); }
VkFormat texture_format(VkPhysicalDevice device,bool compressed,bool alpha) {
    if(!compressed)return VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat candidate=alpha?VK_FORMAT_BC3_UNORM_BLOCK:VK_FORMAT_BC1_RGB_UNORM_BLOCK;VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(device,candidate,&p);
    constexpr auto needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((p.optimalTilingFeatures&needed)!=needed)throw std::runtime_error(alpha?"Requested BC3 texture compression is unsupported by the Vulkan device":"Requested BC1 texture compression is unsupported by the Vulkan device");return candidate;
}
VkBlendFactor blend_factor(MaterialBlendFactor f) {
    constexpr VkBlendFactor factors[]{VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_SRC_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_FACTOR_DST_COLOR,VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    auto n=static_cast<unsigned>(f);if(n>=std::size(factors))throw std::runtime_error("Invalid material blend factor");return factors[n];
}
// Vulkan disallows SRC_COLOR/DST_COLOR in alpha slots; their GL alpha component is exactly SRC_ALPHA/DST_ALPHA.
VkBlendFactor alpha_factor(MaterialBlendFactor f) { if(f==MaterialBlendFactor::src_color)return VK_BLEND_FACTOR_SRC_ALPHA;if(f==MaterialBlendFactor::one_minus_src_color)return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;if(f==MaterialBlendFactor::dst_color)return VK_BLEND_FACTOR_DST_ALPHA;if(f==MaterialBlendFactor::one_minus_dst_color)return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;return blend_factor(f); }
template<class S,class T> Buffer immutable_buffer(S& s,std::span<const T> data,VkBufferUsageFlags usage) {
    if(data.empty())return {};if(s.rt_enabled&&(usage&VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))usage|=VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;auto result=s.create_buffer(data.size_bytes(),usage|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,true);
    auto staging=s.create_buffer(data.size_bytes(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(staging.mapped,data.data(),data.size_bytes());s.flush_buffer(staging,0,data.size_bytes());
    auto cmd=s.begin_commands();VkBufferCopy copy{0,0,data.size_bytes()};vkCmdCopyBuffer(cmd,staging.handle,result.handle,1,&copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT|VK_ACCESS_SHADER_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=result.handle;barrier.size=VK_WHOLE_SIZE;
    if(s.rt_enabled&&(usage&VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))barrier.dstAccessMask|=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,1,&barrier,0,nullptr);s.finish_commands(cmd,true);s.destroy_buffer(staging);return result;
}
BufferRange range(const Buffer& b) { return {b.handle,0,b.size,b.address}; }
template<class S> std::uint32_t resolve_material(S& s,std::string_view name,bool context=true) {
    if(name.empty())return 0;auto* m=s.materials.find_instance(name);if(!m)m=&s.materials.resolve(name,context);auto id=s.admit_material(*m);auto found=s.named_material_ids.find(name);if(found==s.named_material_ids.end())found=s.named_material_ids.emplace(std::string(name),std::array<std::uint32_t,2>{invalid_id,invalid_id}).first;found->second[context]=id;return id;
}
template<class S> std::uint32_t prepared_material(const S& s,std::string_view name,bool context=true) {
    if(name.empty())return 0;auto found=s.named_material_ids.find(name);if(found==s.named_material_ids.end()||found->second[context]==invalid_id)throw std::runtime_error("Renderer.prepare required after material identity change");return found->second[context];
}
std::array<RenderVertex,4> unit_quad() { return {{{{0,0,0},{0,0},{},{0,0,1},{1,1,1,1}},{{1,0,0},{1,0},{},{0,0,1},{1,1,1,1}},{{1,1,0},{1,1},{},{0,0,1},{1,1,1,1}},{{0,1,0},{0,1},{},{0,0,1},{1,1,1,1}}}}; }
template<class S> void publish_geometry(S& s,DrawGeometry& g) {
    if(g.dynamic)g.final_vertices=s.upload_bytes(std::as_bytes(std::span(g.final_pose)),4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    else g.final_vertices=range(g.vertices);
    if(g.indices.handle)g.index_range=range(g.indices);g.vertex_count=std::uint32_t(g.final_pose.size());g.index_count=std::uint32_t(g.index_type==VK_INDEX_TYPE_UINT16?g.indices16.size():g.indices32.size());
    if(g.dynamic||!g.payload_signature)g.payload_signature=hash_bytes(std::as_bytes(std::span(g.final_pose)));g.identity.pose_revision=s.pose_revision;
}
template<class S> void append_instance(S& s,PreparedInstance i,bool grid=false) {
    auto instance_id=std::uint32_t(s.prepared_instances.size());auto& g=s.geometry.at(i.geometry);auto& m=s.pass_materials.at(i.material);g.material=i.material;g.instance=instance_id;g.sky=m.sky!=MaterialSky::none;g.overlay=m.polygon_offset!=0;g.camera_dependent=std::any_of(m.deforms.begin(),m.deforms.end(),[](auto& d){return d.kind!=MaterialDeformKind::wave;});
    i.first_pass=std::uint32_t(s.gpu_passes.size());bool base=false;
    for(std::size_t ordinal=0;ordinal<m.passes.size();++ordinal) {
        auto& p=m.passes[ordinal];bool cube=p.source_mode==MaterialSourceMode::cube;
        if(cube&&(!s.original.reflections||s.capture_mode==CaptureMode::cube))continue;
        GpuPass out;out.tc_matrix=texture_matrix(p,s.seconds,i.interface?render_identity:s.camera.view);
        for(unsigned c=0;c<4;++c)out.constant_color[c]=p.color[c]/255.0f;
        unsigned rgb=i.material==0?3:0;if(p.rgb_gen==MaterialRgbGen::vertex||(p.rgb_gen==MaterialRgbGen::lighting_diffuse&&grid))rgb=1;
        if(p.rgb_gen==MaterialRgbGen::wave){float w=float(original_integer(std::clamp(wave_value(p.rgb_wave,s.seconds),0.0f,1.0f)*255))/255;out.constant_color[0]=out.constant_color[1]=out.constant_color[2]=w;}
        bool lm=p.source_mode==MaterialSourceMode::lightmap,has_lm=i.lightmap_selector>=0&&(lm||(p.source_mode==MaterialSourceMode::environment&&p.texture.kind==MaterialTextureKind::lightmap));
        bool white=lm&&!has_lm&&(p.texture.kind==MaterialTextureKind::lightmap||p.texture.kind==MaterialTextureKind::none);
        unsigned tc=p.tc_gen==MaterialTcGen::environment?2:p.tc_gen==MaterialTcGen::cube?3:lm&&!white?1:0;
        auto image=has_lm?i.lightmap:white||p.source_mode==MaterialSourceMode::none?0:m.image_ids.at(ordinal);
        bool animated=p.animation&&!cube&&!has_lm&&!white;
        if(animated&&!m.animation_ids.at(ordinal).empty()){auto& ids=m.animation_ids[ordinal];double n=std::floor(s.seconds*p.animation->frequency);auto k=static_cast<long long>(std::fmod(n,double(ids.size())));if(k<0)k+=static_cast<long long>(ids.size());image=ids[std::size_t(k)];}
        unsigned sampler=(has_lm?6:cube?12:0)+(m.mipmaps?0:3)+unsigned(animated?p.animation->wrap:p.texture.wrap);
        if(has_lm)sampler=6+(m.mipmaps?0:3)+1;if(cube)sampler=m.mipmaps?14:17;if(p.texture.kind==MaterialTextureKind::avi)sampler=3+unsigned(p.texture.wrap);
        if(i.material==0)sampler=5;
        unsigned flags=(i.interface?1u:0u)|(!cube&&sampler%3==1?2u:0u)|(!i.interface&&s.fog.enabled&&m.fog?4u:0u)|(((i.material!=0&&p.blend)||i.interface)?32u:0u);
        bool standard_cube=p.tc_gen==MaterialTcGen::cube&&std::count_if(p.tc_mods.begin(),p.tc_mods.end(),[](auto& tc){return tc.kind==MaterialTcModKind::cube;})==1&&std::all_of(p.tc_mods.begin(),p.tc_mods.end(),[](auto& tc){return tc.kind==MaterialTcModKind::cube;});
        if(cube&&p.texture.kind==MaterialTextureKind::cube_capture&&standard_cube&&!i.interface&&s.capture_mode==CaptureMode::ordinary)flags|=8;
        if(!base&&!i.interface&&!i.particle&&!g.sky&&!g.overlay&&!g.camera_dependent&&!p.blend&&p.depth_test&&p.depth_write&&p.depth_function==MaterialDepthFunc::lequal){flags|=16;base=true;}
        out.binding={image,sampler,cube?1u:0u,tc};out.modes={rgb,p.alpha_test?unsigned(*p.alpha_test)+1:0,flags,unsigned(p.source_mode)};
        out.blend={unsigned(p.blend_source),unsigned(p.blend_destination),(!i.interface&&p.depth_test?1u:0u)|(!i.interface&&p.depth_write?2u:0u)|(p.depth_function==MaterialDepthFunc::equal?4u:0u),unsigned(i.interface?MaterialCull::none:m.cull)};
        out.parameters[0]=m.polygon_offset;s.gpu_passes.push_back(out);
    }
    i.pass_count=std::uint32_t(s.gpu_passes.size())-i.first_pass;
    const auto& classified=s.material_coverage.at(i.material);Coverage c;c.kind=classified.kind;c.cull=classified.cull;if(i.interface||i.particle||g.sky||g.overlay||g.camera_dependent)c.kind=Coverage::Kind::excluded;
    i.coverage=std::uint32_t(s.coverage.size());g.coverage=i.coverage;s.coverage.push_back(std::move(c));
    GpuCoverage gc;gc.passes={std::uint32_t(s.coverage_passes.size()),0,unsigned(m.cull),unsigned(s.coverage.back().kind)};
    // Match original ordinals after capture/cube exclusions, not raw pass offsets.
    unsigned compact=0;for(unsigned original=0;original<m.passes.size();++original){auto& p=m.passes[original];if(p.source_mode==MaterialSourceMode::cube&&(!s.original.reflections||s.capture_mode==CaptureMode::cube))continue;if(s.coverage.back().kind!=Coverage::Kind::excluded&&std::find(classified.pass_ordinals.begin(),classified.pass_ordinals.end(),original)!=classified.pass_ordinals.end()){s.coverage_passes.push_back(i.first_pass+compact);++gc.passes[1];}++compact;}
    s.gpu_coverage.push_back(gc);
    GpuInstance gi;gi.model=i.model;auto nm=material_normal_matrix(i.model);for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)gi.normal_matrix[col*4+row]=nm[col*3+row];gi.entity_color=i.color;gi.ranges={i.first_pass,i.pass_count,0,std::uint32_t(s.gpu_triangles.size())};gi.metadata={i.geometry,i.coverage,i.lightmap,i.interface?1u:0u};
    auto emit=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t original){GpuTriangle t;t.vertices={a,b,c,instance_id};t.source={original,i.geometry,i.coverage,0};s.gpu_triangles.push_back(t);};
    auto expand=[&](auto& indices){if(g.primitive==Primitive::triangle_strip){for(std::size_t k=2;k<indices.size();++k){auto a=indices[k-2],b=indices[k-1],c=indices[k];if(k&1)std::swap(a,b);emit(a,b,c,std::uint32_t(k-2));}}else for(std::size_t k=0;k+2<indices.size();k+=3)emit(indices[k],indices[k+1],indices[k+2],std::uint32_t(k/3));};
    if(g.index_type==VK_INDEX_TYPE_UINT16)expand(g.indices16);else expand(g.indices32);
    assert(s.gpu_triangles.size()-gi.ranges[3]==(g.primitive==Primitive::triangle_strip?(g.index_count>=2?g.index_count-2:0):g.index_count/3));
    assert(s.gpu_instances.size()==s.prepared_instances.size());
    s.gpu_instances.push_back(gi);s.prepared_instances.push_back(i);
    if(s.gpu_geometry.size()<=i.geometry)s.gpu_geometry.resize(i.geometry+1);GpuGeometry gg;gg.addresses={std::uint32_t(g.final_vertices.address),std::uint32_t(g.final_vertices.address>>32),std::uint32_t(g.index_range.address),std::uint32_t(g.index_range.address>>32)};gg.counts={g.vertex_count,g.index_count,g.index_type==VK_INDEX_TYPE_UINT16?1u:0u,14};s.gpu_geometry[i.geometry]=gg;
}
}

void VulkanRenderer::State::initialize_raster() {
    auto module=[&](auto& words){VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=words.size()*sizeof(std::uint32_t);info.pCode=words.data();VkShaderModule result{};checked(vkCreateShaderModule(device,&info,nullptr,&result),"Create embedded shader");return result;};
    shaders.material_vertex=module(vk_shaders::material_vertex_spv);shaders.material_fragment=module(vk_shaders::material_fragment_spv);if(rt_enabled)shaders.material_ray_fragment=module(vk_shaders::material_ray_fragment_spv);shaders.fullscreen_vertex=module(vk_shaders::fullscreen_vertex_spv);shaders.post_fragment=module(vk_shaders::post_fragment_spv);shaders.history_fragment=module(vk_shaders::history_fragment_spv);
    auto& limits=properties.limits;unsigned storage=rt_enabled?8:6;unsigned resource_reserve=storage+20+(rt_enabled?1:0);
    if(limits.maxPerStageResources<=resource_reserve||limits.maxPerStageDescriptorSamplers<19||limits.maxDescriptorSetSamplers<19||limits.maxPerStageDescriptorStorageBuffers<storage)throw std::runtime_error("Vulkan material descriptor limits cannot hold original raster tables");
    image_descriptor_capacity=std::min({limits.maxPerStageDescriptorSampledImages/2,limits.maxDescriptorSetSampledImages/2,(limits.maxPerStageResources-resource_reserve)/2,4096u});
    if(image_descriptor_capacity<2)throw std::runtime_error("Vulkan sampled-image table capacity is insufficient");
    std::array<VkDescriptorSetLayoutBinding,13> binding{};unsigned count=rt_enabled?13:10;
    for(unsigned k=0;k<count;++k){binding[k].binding=k;binding[k].descriptorCount=1;binding[k].stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;binding[k].descriptorType=k==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:k==5||k==6?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:k==7?VK_DESCRIPTOR_TYPE_SAMPLER:k==10?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;}
    binding[5].descriptorCount=binding[6].descriptorCount=image_descriptor_capacity;binding[7].descriptorCount=19;
    std::array<VkDescriptorBindingFlags,13> flags{};flags[5]=flags[6]=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo indexed{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};indexed.bindingCount=count;indexed.pBindingFlags=flags.data();VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};info.pNext=&indexed;info.bindingCount=count;info.pBindings=binding.data();checked(vkCreateDescriptorSetLayout(device,&info,nullptr,&material_descriptors),"Create original material descriptors");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(DrawPush)};VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout_info.setLayoutCount=1;layout_info.pSetLayouts=&material_descriptors;layout_info.pushConstantRangeCount=1;layout_info.pPushConstantRanges=&push;checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&material_layout),"Create original material layout");
    refresh_samplers();
    int display=SDL_GetWindowDisplayIndex(window);SDL_DisplayMode mode{};if(display<0||SDL_GetCurrentDisplayMode(display,&mode)!=0||mode.h<=0)throw std::runtime_error("Cannot determine cube display height");capture_display_height=mode.h;
    admit_texture("$whiteimage",false,false);admit_texture("yellowimage",false,false);
    static constexpr std::uint32_t quad_indices[]{0,1,2,0,2,3};interface_index_buffer=immutable_buffer(*this,std::span<const std::uint32_t>(quad_indices),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    static constexpr std::uint32_t triangle_indices[]{0,1,2};particle_triangle_index_buffer=immutable_buffer(*this,std::span<const std::uint32_t>(triangle_indices),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    Material untextured;untextured.name="";untextured.cull=MaterialCull::none;untextured.mipmaps=false;untextured.fog=true;MaterialPass p;p.blend=true;p.blend_source=MaterialBlendFactor::src_alpha;p.blend_destination=MaterialBlendFactor::one_minus_src_alpha;p.rgb_gen=MaterialRgbGen::entity;p.depth_write=true;untextured.passes.push_back(p);admit_material(untextured);material_ids.clear();
}
void VulkanRenderer::State::refresh_samplers() {
    const int filters[]{original.texture_filter,original.lightmap_filter,original.reflection_filter};
    for(unsigned k=0;k<19;++k) {
        bool font=k==18;auto& destination=font?font_sampler:samplers[k];
        if(destination)vkDestroySampler(device,destination,nullptr);destination={};
        unsigned wrap=font?1:k%3;int filter=font?1:effective_texture_filter(filters[k/6],k%6<3);
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=filter==0?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
        sampler.mipmapMode=filter==3?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=wrap==0?VK_SAMPLER_ADDRESS_MODE_REPEAT:wrap==1?VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        sampler.maxLod=filter>=2?VK_LOD_CLAMP_NONE:0;sampler.anisotropyEnable=!font&&settings.anisotropy>1;sampler.maxAnisotropy=font?1:float(settings.anisotropy);
        checked(vkCreateSampler(device,&sampler,nullptr,&destination),"Create original sampler");
    }
}
void VulkanRenderer::State::destroy_raster() noexcept {
    for(auto& g:geometry){destroy_buffer(g.vertices);destroy_buffer(g.indices);}destroy_buffer(world_vertex_buffer);for(auto& i:images)destroy_image(i.image);
    destroy_buffer(interface_index_buffer);
    destroy_buffer(particle_triangle_index_buffer);
    for(auto& p:pipelines)if(p.pipeline)vkDestroyPipeline(device,p.pipeline,nullptr);pipelines.clear();for(auto& s:samplers){if(s)vkDestroySampler(device,s,nullptr);s={};}if(font_sampler)vkDestroySampler(device,font_sampler,nullptr);font_sampler={};
    VkShaderModule* modules[]{&shaders.material_vertex,&shaders.material_fragment,&shaders.material_ray_fragment,&shaders.fullscreen_vertex,&shaders.post_fragment,&shaders.history_fragment};for(auto* m:modules){if(*m)vkDestroyShaderModule(device,*m,nullptr);*m={};}
    if(material_layout)vkDestroyPipelineLayout(device,material_layout,nullptr);if(material_descriptors)vkDestroyDescriptorSetLayout(device,material_descriptors,nullptr);material_layout={};material_descriptors={};geometry.clear();images.clear();
    pass_materials.clear();image_names.clear();font_atlases.clear();material_ids.clear();named_material_ids.clear();mesh_ids.clear();object_lighting.clear();transient_lighting.clear();interface_geometry.clear();world_vertices.clear();world_geometry.clear();world_order.clear();world_bounds.clear();capture_images.clear();lightmap_images.clear();level=nullptr;generation=std::numeric_limits<std::uint64_t>::max();
    material_coverage.clear();
}
void VulkanRenderer::State::upload_image(ImageResource& resource,std::span<const std::uint8_t> input,std::uint32_t components,std::uint32_t layer) {
    auto& image=resource.image;if(components!=3&&components!=4)throw std::runtime_error("Unsupported original image components");std::size_t pixels=std::size_t(image.extent.width)*image.extent.height;if(input.size()!=pixels*components||layer>=image.layers)throw std::runtime_error("Original image payload size mismatch");
    auto& rgba=image_upload_pixels;rgba.resize(pixels*4);for(std::size_t k=0;k<pixels;++k){std::copy_n(input.data()+k*components,3,rgba.data()+k*4);rgba[k*4+3]=components==4?input[k*4+3]:255;}
    if(resource.kind==MaterialTextureKind::cube_capture||resource.kind==MaterialTextureKind::cube_faces||resource.kind==MaterialTextureKind::avi)for(std::size_t k=0;k<rgba.size();k+=4)for(unsigned c=0;c<3;++c)rgba[k+c]=rgb5(rgba[k+c]);
    auto& encoded=image_upload_encoded;auto& regions=image_upload_regions;encoded.clear();regions.clear();std::uint32_t w=image.extent.width,h=image.extent.height;
    for(unsigned mip=0;mip<image.mip_levels;++mip){VkBufferImageCopy r{};r.bufferOffset=encoded.size();r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,layer,1};r.imageExtent={w,h,1};regions.push_back(r);
        bool bc=image.format==VK_FORMAT_BC1_RGB_UNORM_BLOCK||image.format==VK_FORMAT_BC3_UNORM_BLOCK;auto start=encoded.size();auto size=bc?std::size_t(squish::GetStorageRequirements(int(w),int(h),image.format==VK_FORMAT_BC3_UNORM_BLOCK?squish::kDxt5:squish::kDxt1)):std::size_t(w)*h*4;encoded.resize(start+size);
        if(bc)squish::CompressImage(rgba.data(),int(w),int(h),encoded.data()+start,(image.format==VK_FORMAT_BC3_UNORM_BLOCK?squish::kDxt5:squish::kDxt1)|squish::kColourClusterFit);else std::memcpy(encoded.data()+start,rgba.data(),size);
        if(mip+1<image.mip_levels){unsigned nw=std::max(1u,w/2),nh=std::max(1u,h/2);for(unsigned y=0;y<nh;++y)for(unsigned x=0;x<nw;++x)for(unsigned c=0;c<4;++c){unsigned sum=0,n=0;for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx){unsigned sx=std::min(w-1,x*2+dx),sy=std::min(h-1,y*2+dy);sum+=rgba[(std::size_t(sy)*w+sx)*4+c];++n;}auto value=std::uint8_t(sum/n);if(c<3&&(resource.kind==MaterialTextureKind::cube_capture||resource.kind==MaterialTextureKind::cube_faces))value=rgb5(value);rgba[(std::size_t(y)*nw+x)*4+c]=value;}w=nw;h=nh;rgba.resize(std::size_t(w)*h*4);}
    }
    auto staging=upload_bytes(std::as_bytes(std::span(encoded)),16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);for(auto& region:regions)region.bufferOffset+=staging.offset;auto cmd=begin_commands();
    transition_image(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);vkCmdCopyBufferToImage(cmd,staging.buffer,image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,std::uint32_t(regions.size()),regions.data());transition_image(cmd,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);finish_commands(cmd,true);
}
std::uint32_t VulkanRenderer::State::admit_texture(std::string_view name,bool picmip,bool compression,bool font) {
    bool white=name.empty()||RenderNameEqual{}(name,"$whiteimage")||RenderNameEqual{}(name,"whiteimage"),yellow=RenderNameEqual{}(name,"yellowimage");std::string key(white?"$whiteimage":name);key+=font?"|font":picmip?"|scaled":"|full";key+=compression?"|compressed":"|raw";
    if(auto found=image_names.find(key);found!=image_names.end())return found->second;if(images.size()>=image_descriptor_capacity)throw std::runtime_error("Original texture table exceeds Vulkan descriptor limits");
    ImageResource r;r.name=std::string(name);r.kind=MaterialTextureKind::image;r.picmip=picmip;r.compression=compression;r.font=font;r.mipmaps=!white&&!font;std::vector<std::uint8_t> data;VkExtent2D size{};
    if(white||yellow){size=yellow?VkExtent2D{4,4}:VkExtent2D{1,1};data.resize(std::size_t(size.width)*size.height*4);for(std::size_t k=0;k<data.size();k+=4){data[k]=255;data[k+1]=yellow?240:255;data[k+2]=yellow?150:255;data[k+3]=255;}r.mipmaps=yellow;}
    else {auto surface=reduce_image(image_surface(assets,name),picmip?original.texture_divisor:1);size={std::uint32_t(surface->w),std::uint32_t(surface->h)};data.resize(std::size_t(size.width)*size.height*4);for(unsigned y=0;y<size.height;++y)std::memcpy(data.data()+std::size_t(y)*size.width*4,static_cast<std::uint8_t*>(surface->pixels)+std::size_t(y)*surface->pitch,size.width*4);}
    auto format=texture_format(physical,compression&&original.texture_compression&&!font,true);r.image=create_image(size,format,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_SAMPLE_COUNT_1_BIT,1,mip_count(size,r.mipmaps));upload_image(r,data,4);auto id=std::uint32_t(images.size());images.push_back(std::move(r));image_names.emplace(std::move(key),id);if(font)font_atlases[std::string(name)]=id;return id;
}
std::uint32_t VulkanRenderer::State::admit_image(const MaterialTexture& texture,const Material& material) {
    if(texture.kind==MaterialTextureKind::none||texture.kind==MaterialTextureKind::white||texture.kind==MaterialTextureKind::lightmap)return 0;
    if(texture.kind==MaterialTextureKind::image)return admit_texture(texture.image,material.picmip,material.compression);
    auto signature=texture_signature(texture);signature=hash_value(material.picmip,signature);signature=hash_value(material.mipmaps,signature);signature=hash_value(material.compression,signature);
    std::string key="|source|"+std::to_string(signature);
    if(texture.kind==MaterialTextureKind::cube_capture||texture.kind==MaterialTextureKind::avi){unsigned ordinal=0;while(ordinal<material.passes.size() && &material.passes[ordinal].texture!=&texture)++ordinal;if(ordinal==material.passes.size())throw std::runtime_error("Original mutable source lacks its owned material pass");key+="|material|"+std::to_string(material_ids.at(&material))+"|pass|"+std::to_string(ordinal);}
    if(auto it=image_names.find(key);it!=image_names.end())return it->second;if(images.size()>=image_descriptor_capacity)throw std::runtime_error("Original source table exceeds Vulkan descriptor limits");
    ImageResource r;r.name=texture.image;r.kind=texture.kind;r.signature=signature;r.picmip=material.picmip;r.compression=material.compression;r.mipmaps=material.mipmaps;
    if(texture.kind==MaterialTextureKind::combined){auto surface=combined_image(assets,texture);VkExtent2D size{std::uint32_t(surface->w),std::uint32_t(surface->h)};r.image=create_image(size,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_SAMPLE_COUNT_1_BIT,1,mip_count(size,true));std::vector<std::uint8_t> rgb(std::size_t(size.width)*size.height*3);for(unsigned y=0;y<size.height;++y)for(unsigned x=0;x<size.width;++x)std::memcpy(rgb.data()+(std::size_t(y)*size.width+x)*3,static_cast<std::uint8_t*>(surface->pixels)+std::size_t(y)*surface->pitch+x*4,3);upload_image(r,rgb,3);}
    else if(texture.kind==MaterialTextureKind::avi){r.mipmaps=false;r.image=create_image({1,1},VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);std::array<std::uint8_t,3> black{};upload_image(r,black,3);}
    else {if(texture.cube_size<=0)throw std::runtime_error("Invalid original cube size");int side=std::min(texture.cube_size,int(properties.limits.maxImageDimensionCube));while(side>capture_display_height)side>>=1;side=std::max(1,side/(material.picmip?original.reflection_divisor:1));r.mipmaps=effective_texture_filter(original.reflection_filter,material.mipmaps)>=2;r.capture=texture.kind==MaterialTextureKind::cube_capture;VkExtent2D size{unsigned(side),unsigned(side)};r.image=create_image(size,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_SAMPLE_COUNT_1_BIT,6,mip_count(size,r.mipmaps),true);std::vector<std::uint8_t> raw(std::size_t(side)*side*3);
        for(unsigned face=0;face<6;++face){std::fill(raw.begin(),raw.end(),0);if(!r.capture){DecodedLayout layout;auto surface=image_surface(assets,texture.cube_faces[face],texture.cube_size,&layout);cube_face_bytes(*surface,layout,raw);}upload_image(r,raw,3,face);}}
    auto id=std::uint32_t(images.size());images.push_back(std::move(r));image_names.emplace(std::move(key),id);
    // Initialization is not publication; a genuinely new source invalidates the remembered registry once.
    if(images[id].capture){capture_images.push_back(id);if(!reflection_path.empty())reflections_pending=true;}return id;
}
std::uint32_t VulkanRenderer::State::admit_material(const Material& material) {
    auto signature=material_signature(material);std::uint32_t id=invalid_id;
    if(auto it=material_ids.find(&material);it!=material_ids.end())id=it->second;
    if(id==invalid_id){id=std::uint32_t(pass_materials.size());pass_materials.emplace_back();material_coverage.emplace_back();}
    material_ids[&material]=id;
    auto& owned=pass_materials[id];if(owned.signature!=signature){owned.passes=material.passes;owned.deforms=material.deforms;owned.cull=material.cull;owned.lightgrid=material.lightgrid;owned.sort=material.sort;owned.sky=material.sky;owned.polygon_offset=material.polygon_offset;owned.mipmaps=material.mipmaps;owned.picmip=material.picmip;owned.compression=material.compression;owned.fog=material.fog;owned.name=material.name;owned.signature=signature;owned.image_ids.resize(material.passes.size());owned.animation_ids.resize(material.passes.size());
        material_coverage[id]=classify_coverage(owned);
        for(unsigned k=0;k<material.passes.size();++k){auto& p=material.passes[k];owned.image_ids[k]=admit_image(p.texture,material);auto& animation=owned.animation_ids[k];animation.clear();if(p.animation)for(auto& frame:p.animation->frames)animation.push_back(admit_texture(frame,material.picmip,material.compression));}}
    owned.prepared_revision=prepare_revision;return id;
}
void VulkanRenderer::State::update_video(std::uint32_t id) {
    auto& r=images.at(id);if(r.kind!=MaterialTextureKind::avi)return;if(!r.video)r.video=std::make_unique<VideoTexture>(assets,r.name,tick_milliseconds);r.video->update_clock(tick_milliseconds);auto frame=r.video->frame();if(frame.rgba.empty()||frame.seconds==r.timestamp)return;
    int w=avi_atlas_side(frame.width),h=avi_atlas_side(frame.height);if(frame.width>w||frame.height>h)throw std::runtime_error("Original AVI frame exceeds fixed atlas");
    // A video update never changes an image used by either pending frame slot.
    wait_idle();if(r.image.extent.width!=unsigned(w)||r.image.extent.height!=unsigned(h)){
        destroy_image(r.image);r.image=create_image({unsigned(w),unsigned(h)},VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        r.pixels.assign(std::size_t(w)*h*3,0);upload_image(r,r.pixels,3);
    }
    avi_frame_bytes(frame,r.pixels);auto& rgba=image_upload_pixels;rgba.resize(std::size_t(frame.width)*frame.height*4);
    for(std::size_t k=0;k<r.pixels.size()/3;++k){for(unsigned c=0;c<3;++c)rgba[k*4+c]=rgb5(r.pixels[k*3+c]);rgba[k*4+3]=255;}
    auto staging=upload_bytes(std::as_bytes(std::span(rgba)),16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);auto cmd=begin_commands();
    transition_image(cmd,r.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};region.bufferOffset=staging.offset;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageOffset={(w-frame.width)/2,(h-frame.height)/2,0};region.imageExtent={unsigned(frame.width),unsigned(frame.height),1};
    vkCmdCopyBufferToImage(cmd,staging.buffer,r.image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    transition_image(cmd,r.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);finish_commands(cmd,true);r.timestamp=frame.seconds;
}

void VulkanRenderer::State::begin_generation(std::uint64_t next,const Level* next_level) {
    if(generation==next&&level==next_level&&!resources_dirty)return;
    // A dirty-resource rebuild retains this scene's remembered BSP; a real transition (including null) cannot.
    wait_idle();if(generation!=next||level!=next_level)reflection_path.clear();
    for(auto& g:geometry){destroy_buffer(g.vertices);destroy_buffer(g.indices);}geometry.clear();destroy_buffer(world_vertex_buffer);world_vertices.clear();
    for(auto& i:images)destroy_image(i.image);images.clear();image_names.clear();font_atlases.clear();material_ids.clear();named_material_ids.clear();mesh_ids.clear();object_lighting.clear();transient_lighting.clear();
    if(pass_materials.size()>1)pass_materials.resize(1);
    if(material_coverage.size()>1)material_coverage.resize(1);interface_geometry.clear();
    prepared_instances.clear();coverage.clear();capture_images.clear();lightmap_images.clear();world_order.clear();world_geometry.clear();world_bounds.clear();
    leaf_culls.clear();node_culls.clear();node_parents.clear();leaf_parents.clear();surface_stamps.clear();node_stamps.clear();leaf_stamps.clear();
    sky_centers={};sky_radii={};generation=next;level=next_level;resources_dirty=false;reflections_pending=true;reset_history();
    admit_texture("$whiteimage",false,false);admit_texture("yellowimage",false,false);
}
void VulkanRenderer::State::prepare_level_resources(const Level& next,std::uint64_t next_generation,LevelProgress progress,void* context) {
    begin_generation(next_generation,&next);if(!world_geometry.empty()||preparing_level)return;
    struct Guard { bool& value;Guard(bool& v):value(v){value=true;}~Guard(){value=false;} } guard(preparing_level);
    auto notify=[&](float value){if(progress)progress(context,value);};
    constexpr std::size_t map_bytes=128*128*3;
    std::vector<std::uint8_t> normalized(next.lightmaps.size()*map_bytes);
    for(std::size_t k=0;k<next.lightmaps.size();++k)normalize_lightmap(next.lightmaps[k],std::span<std::uint8_t,map_bytes>(normalized.data()+k*map_bytes,map_bytes));
    notify(0.4f);
    int divisor=std::min(original.lightmap_divisor,128),side=128/divisor;std::vector<std::uint8_t> reduced(std::size_t(side)*side*3);
    for(std::size_t k=0;k<next.lightmaps.size();++k){
        if(images.size()>=image_descriptor_capacity)throw std::runtime_error("Original lightmaps exceed Vulkan descriptor limits");
        auto source=std::span<const std::uint8_t,map_bytes>(normalized.data()+k*map_bytes,map_bytes);if(divisor!=1)reduce_lightmap(source,divisor,reduced);
        ImageResource r;r.kind=MaterialTextureKind::lightmap;r.mipmaps=true;r.compression=original.lightmap_compression;VkExtent2D size{unsigned(side),unsigned(side)};
        r.image=create_image(size,texture_format(physical,original.lightmap_compression,false),VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_SAMPLE_COUNT_1_BIT,1,mip_count(size,true));
        upload_image(r,divisor==1?std::span<const std::uint8_t>(source):std::span<const std::uint8_t>(reduced),3);lightmap_images.push_back(std::uint32_t(images.size()));images.push_back(std::move(r));
    }
    notify(0.5f);
    for(auto& shader:next.shaders)resolve_material(*this,shader.name,false);
    world_vertices.reserve(next.vertices.size());for(auto& v:next.vertices)world_vertices.push_back(from_world(v));
    world_vertex_buffer=immutable_buffer(*this,std::span<const RenderVertex>(world_vertices),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    notify(0.6f);
    world_geometry.reserve(next.surfaces.size());world_bounds.reserve(next.surfaces.size());surface_stamps.resize(next.surfaces.size());
    std::array<Bounds,2> sky_bounds{};std::array<bool,2> has_sky{};
    for(std::uint32_t ordinal=0;ordinal<next.surfaces.size();++ordinal){
        const auto& surface=next.surfaces[ordinal];auto material_id=resolve_material(*this,next.shaders.at(surface.shader).name,false);DrawGeometry g;g.identity.generation=generation;g.identity.surface=ordinal;g.world=true;g.material=material_id;
        if(surface.type==SurfaceType::patch){tessellate(next,surface,g.final_pose,g.indices32);g.index_type=VK_INDEX_TYPE_UINT32;}
        else if(surface.type==SurfaceType::planar||surface.type==SurfaceType::triangle_soup){auto indices=std::span(next.indices).subspan(surface.first_index,surface.index_count);g.indices16.assign(indices.begin(),indices.end());g.index_type=VK_INDEX_TYPE_UINT16;
            g.final_pose.assign(world_vertices.begin()+surface.first_vertex,world_vertices.begin()+surface.first_vertex+surface.vertex_count);
            for(auto index:g.indices16)if(index>=g.final_pose.size())throw std::runtime_error("PL surface index outside local vertex range");
        }else throw std::runtime_error("Unsupported typed PL surface");
        auto& m=pass_materials.at(material_id);g.dynamic=!m.deforms.empty();if(g.dynamic){g.rest_pose=g.final_pose;g.scratch.resize(g.final_pose.size());}
        if(g.index_type==VK_INDEX_TYPE_UINT16)g.indices=immutable_buffer(*this,std::span<const std::uint16_t>(g.indices16),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        else g.indices=immutable_buffer(*this,std::span<const std::uint32_t>(g.indices32),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if(!g.dynamic){if(surface.type==SurfaceType::patch)g.vertices=immutable_buffer(*this,std::span<const RenderVertex>(g.final_pose),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            else{g.final_vertices={world_vertex_buffer.handle,VkDeviceSize(surface.first_vertex)*sizeof(RenderVertex),VkDeviceSize(surface.vertex_count)*sizeof(RenderVertex),world_vertex_buffer.address+VkDeviceSize(surface.first_vertex)*sizeof(RenderVertex)};}}
        g.topology_signature=g.index_type==VK_INDEX_TYPE_UINT16?hash_bytes(std::as_bytes(std::span(g.indices16))):hash_bytes(std::as_bytes(std::span(g.indices32)));
        Bounds bounds{{INFINITY,INFINITY,INFINITY},{-INFINITY,-INFINITY,-INFINITY}};for(std::uint32_t k=0;k<surface.vertex_count;++k){auto p=next.vertices.at(surface.first_vertex+k).position;bounds.minimum={std::min(bounds.minimum.x,p.x),std::min(bounds.minimum.y,p.y),std::min(bounds.minimum.z,p.z)};bounds.maximum={std::max(bounds.maximum.x,p.x),std::max(bounds.maximum.y,p.y),std::max(bounds.maximum.z,p.z)};}g.bounds=bounds;g.sky=m.sky!=MaterialSky::none;
        if(g.sky){unsigned group=m.sky==MaterialSky::normal?0:1;if(!has_sky[group]){has_sky[group]=true;sky_bounds[group]=bounds;}else{auto& b=sky_bounds[group];b.minimum={std::min(b.minimum.x,bounds.minimum.x),std::min(b.minimum.y,bounds.minimum.y),std::min(b.minimum.z,bounds.minimum.z)};b.maximum={std::max(b.maximum.x,bounds.maximum.x),std::max(b.maximum.y,bounds.maximum.y),std::max(b.maximum.z,bounds.maximum.z)};}}
        Bounds capture_bounds=bounds;if(surface.type==SurfaceType::patch){capture_bounds={{INFINITY,INFINITY,INFINITY},{-INFINITY,-INFINITY,-INFINITY}};for(auto& v:g.final_pose){auto p=v.position;capture_bounds.minimum={std::min(capture_bounds.minimum.x,p.x),std::min(capture_bounds.minimum.y,p.y),std::min(capture_bounds.minimum.z,p.z)};capture_bounds.maximum={std::max(capture_bounds.maximum.x,p.x),std::max(capture_bounds.maximum.y,p.y),std::max(capture_bounds.maximum.z,p.z)};}}
        Vec3 center=(capture_bounds.minimum+capture_bounds.maximum)*0.5f;for(unsigned k=0;k<m.passes.size();++k)if(m.passes[k].source_mode==MaterialSourceMode::cube){auto& r=images[m.image_ids[k]];if(r.capture){r.center=r.center_set?cube_capture_center(r.center,center):center;r.center_set=true;}}
        world_geometry.push_back(std::uint32_t(geometry.size()));world_bounds.push_back(cache_bounds(bounds));geometry.push_back(std::move(g));
    }
    for(unsigned group=0;group<2;++group){sky_centers[group]=(sky_bounds[group].minimum+sky_bounds[group].maximum)*0.5f;sky_radii[group]=has_sky[group]?original_length(sky_centers[group]-sky_bounds[group].minimum):0;}
    world_order.resize(next.surfaces.size());for(std::uint32_t k=0;k<world_order.size();++k)world_order[k]=k;
    std::stable_sort(world_order.begin(),world_order.end(),[&](auto a,auto b){return pass_materials[geometry[world_geometry[a]].material].sort<pass_materials[geometry[world_geometry[b]].material].sort;});
    leaf_culls.resize(next.leaves.size());node_culls.resize(next.nodes.size());node_stamps.assign(next.nodes.size(),0);leaf_stamps.assign(next.leaves.size(),0);node_parents.assign(next.nodes.size(),-1);leaf_parents.assign(next.leaves.size(),-1);
    for(unsigned k=0;k<next.leaves.size();++k){leaf_culls[k]=next.leaves[k].bounds.type;if(leaf_culls[k]>=6)throw std::runtime_error("PL leaf rejected-plane cache outside table");}
    for(unsigned k=0;k<next.nodes.size();++k){node_culls[k]=next.nodes[k].bounds.type;if(node_culls[k]>=6)throw std::runtime_error("PL node rejected-plane cache outside table");for(auto child:next.nodes[k].children){if(child>=0)node_parents.at(child)=int(k);else leaf_parents.at(~child)=int(k);}}
}
void VulkanRenderer::State::prepare_interface_resources(std::span<const InterfaceQuad> quads) {
    // Loading borrows UI only: retain world/source identities until a real scene preparation replaces them.
    for(auto& q:quads){
        if(q.font_atlas)admit_texture(q.shader,false,false,true);
        else {
            auto id=resolve_material(*this,q.shader,q.shader.find("textures")!=std::string_view::npos);
            if(capture_mode==CaptureMode::loading)for(auto image:pass_materials[id].image_ids)if(images[image].kind==MaterialTextureKind::avi)update_video(image);
        }
    }
    while(interface_geometry.size()<quads.size()){DrawGeometry g;g.dynamic=true;g.identity.generation=generation;g.identity.transient=true;g.identity.surface=invalid_id-2;g.indices32={0,1,2,0,2,3};g.index_type=VK_INDEX_TYPE_UINT32;g.final_pose.resize(4);interface_geometry.push_back(std::uint32_t(geometry.size()));geometry.push_back(std::move(g));}
}
void VulkanRenderer::State::prepare_resources(const RenderScene& scene,std::span<const InterfaceQuad> quads) {
    begin_generation(scene.generation,scene.level);if(scene.level&&world_geometry.empty()&&!preparing_level)prepare_level_resources(*scene.level,scene.generation,nullptr,nullptr);
    ++prepare_revision;
    // A parser reallocation or in-place override is resolved and signed at each borrow.
    bool reorder=false;if(scene.level&&!preparing_level){for(unsigned k=0;k<world_geometry.size();++k){auto& surface=scene.level->surfaces[k];auto& g=geometry[world_geometry[k]];auto previous=pass_materials[g.material].sort;g.material=resolve_material(*this,scene.level->shaders.at(surface.shader).name,false);reorder=reorder||previous!=pass_materials[g.material].sort;}}
    if(reorder){for(unsigned k=0;k<world_order.size();++k)world_order[k]=k;std::stable_sort(world_order.begin(),world_order.end(),[&](auto a,auto b){return pass_materials[geometry[world_geometry[a]].material].sort<pass_materials[geometry[world_geometry[b]].material].sort;});}
    for(auto& object:scene.objects){
        if(object.material_override)admit_material(*object.material_override);else resolve_material(*this,object.material);
        if(!object.mesh)continue;auto found=mesh_ids.find(object.mesh);bool changed=found==mesh_ids.end()||geometry[found->second].identity.geometry_generation!=object.geometry_generation;
        if(!changed)continue;DrawGeometry g;g.identity.generation=generation;g.identity.object=reinterpret_cast<std::uintptr_t>(object.mesh);g.identity.geometry_generation=object.geometry_generation;g.primitive=object.mesh->primitive;g.bounds=object.mesh->bounds;g.indices16=object.mesh->indices;
        g.final_pose.resize(object.mesh->positions.size());for(unsigned k=0;k<g.final_pose.size();++k){g.final_pose[k]={object.mesh->positions[k],object.mesh->texcoords[k],{},object.mesh->normals[k],{1,1,1,1}};}
        g.vertices=immutable_buffer(*this,std::span<const RenderVertex>(g.final_pose),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);g.indices=immutable_buffer(*this,std::span<const std::uint16_t>(g.indices16),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);g.topology_signature=hash_bytes(std::as_bytes(std::span(g.indices16)));
        if(found==mesh_ids.end()){mesh_ids[object.mesh]=std::uint32_t(geometry.size());geometry.push_back(std::move(g));}
        else{wait_idle();auto& previous=geometry[found->second];destroy_buffer(previous.vertices);destroy_buffer(previous.indices);previous=std::move(g);}
    }
    for(auto& particle:scene.particles)resolve_material(*this,particle.material);
    prepare_interface_resources(quads);
    std::erase_if(object_lighting,[&](auto& entry){return std::none_of(scene.objects.begin(),scene.objects.end(),[&](auto& object){return object.lighting_id==entry.first&&object.mesh;});});
    if(!preparing_level)std::erase_if(material_ids,[&](auto& entry){return pass_materials[entry.second].prepared_revision!=prepare_revision;});
    std::erase_if(mesh_ids,[&](auto& entry){return std::none_of(scene.objects.begin(),scene.objects.end(),[&](auto& object){return object.mesh==entry.first;});});
}

namespace {
template<class S> void deform_owned(S& s,DrawGeometry& g,PassMaterial& m,const Matrix& model) {
    if(m.deforms.empty())return;g.scratch.resize(g.final_pose.size());
    // Borrow only the owned deform vector into the shared exact kernel; no allocation/copy.
    Material temporary;temporary.cull=m.cull;temporary.deforms.swap(m.deforms);struct Restore { Material& a;PassMaterial& b;~Restore(){a.deforms.swap(b.deforms);} } restore{temporary,m};
    if(g.index_type==VK_INDEX_TYPE_UINT16)deform_vertices(temporary,g.primitive,std::span<const std::uint16_t>(g.indices16),g.final_pose,g.scratch,model,s.camera,s.seconds);
    else deform_vertices(temporary,g.primitive,std::span<const std::uint32_t>(g.indices32),g.final_pose,g.scratch,model,s.camera,s.seconds);
    g.final_pose.swap(g.scratch);
}
template<class S> std::uint32_t dynamic_geometry(S& s,const GeometryIdentity& identity,std::uint64_t source_signature,std::span<const RenderVertex> base,std::span<const std::uint16_t> indices16,std::span<const std::uint32_t> indices32,Primitive primitive,BufferRange shared_indices,bool posed=false) {
    std::uint32_t id=invalid_id;
    for(std::uint32_t k=0;k<s.geometry.size();++k){auto& g=s.geometry[k];if(g.dynamic&&!g.world&&g.identity.object==identity.object&&g.identity.part==identity.part&&g.identity.transient==identity.transient&&g.identity.geometry_generation==identity.geometry_generation&&g.identity.surface==identity.surface){id=k;break;}}
    if(id==invalid_id)for(std::uint32_t k=0;k<s.geometry.size();++k){auto& previous=s.geometry[k];if(previous.dynamic&&!previous.world&&previous.identity.surface!=invalid_id-2&&previous.last_seen!=s.raster_frame_ordinal&&previous.last_submission<=s.completed_serial){id=k;break;}}
    if(id==invalid_id){id=std::uint32_t(s.geometry.size());s.geometry.emplace_back();}
    auto& g=s.geometry[id];bool replacement=g.final_pose.size()!=base.size()||g.identity.generation!=identity.generation||g.identity.surface!=identity.surface||g.identity.geometry_generation!=identity.geometry_generation||g.topology_signature!=source_signature;
    g.dynamic=true;g.identity=identity;g.primitive=primitive;g.last_seen=s.raster_frame_ordinal;
    if(replacement){g.final_pose.assign(base.begin(),base.end());g.indices16.assign(indices16.begin(),indices16.end());g.indices32.assign(indices32.begin(),indices32.end());g.index_type=indices32.empty()?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;
        g.topology_signature=source_signature;
    }
    g.index_range=shared_indices;
    if(!replacement){if(identity.surface==invalid_id-1)std::copy(base.begin(),base.end(),g.final_pose.begin());else if(!posed)for(unsigned k=0;k<base.size();++k){g.final_pose[k].position=base[k].position;g.final_pose[k].normal=base[k].normal;}}
    return id;
}
}
void VulkanRenderer::State::prepare_frame_tables() {
    gpu_frame={};gpu_frame.view=camera.view;gpu_frame.view_projection=view_projection;gpu_frame.camera_time={camera.position.x,camera.position.y,camera.position.z,float(seconds)};gpu_frame.fog_color=fog.color;gpu_frame.fog_parameters={fog.start,fog.end,fog.density,float(fog.mode)};gpu_frame.quality={settings.gamma,settings.exposure_ev,settings.bloom?0.25f:0,0.01f};gpu_frame.flags[0]=settings.hdr_tonemapping&&capture_mode!=CaptureMode::cube;
    prepared_instances.clear();coverage.clear();gpu_passes.clear();gpu_instances.clear();gpu_triangles.clear();gpu_geometry.clear();gpu_coverage.clear();coverage_passes.clear();
}
void VulkanRenderer::State::prepare_frame_geometry(const RenderScene& scene) {
    camera=scene.camera;fog=scene.fog;seconds=scene.time_seconds;tick_milliseconds=scene.tick_milliseconds;++pose_revision;++raster_frame_ordinal;
    float aspect=extent.height?float(extent.width)/extent.height:1;float fov=camera.reference_fov_degrees>0?camera.reference_fov_degrees:settings.reference_fov;
    view_projection=multiply(perspective(fov,aspect,camera.near_plane,1.0e11f),camera.view);
    constexpr float half_degree=std::bit_cast<float>(std::uint32_t{0x3c0efa35});frustum=model_frustum(camera,std::atan(std::tan(static_cast<long double>(fov)*half_degree)*0.75L*aspect),1/aspect);
    prepare_frame_tables();
    // Decode clocks before any owned dynamic ranges or render-pass recording exist.
    for(unsigned k=0;k<images.size();++k)if(images[k].kind==MaterialTextureKind::avi)update_video(k);
    for(auto surface_id:world_order){
        auto id=world_geometry[surface_id];auto& g=geometry[id];auto& m=pass_materials[g.material];PreparedInstance i;i.geometry=id;i.material=g.material;i.world=true;i.visible=true;i.lightmap_selector=level->surfaces[surface_id].lightmap;i.lightmap=i.lightmap_selector<0?0:lightmap_images.at(i.lightmap_selector);i.part_bounds=g.bounds;
        if(m.sky!=MaterialSky::none){unsigned group=m.sky==MaterialSky::normal?0:1;float radius=sky_radii[0]>0?sky_radii[0]:sky_radii[group];if(radius<=0)i.visible=false;else{float scale=camera.far_plane/radius-0.0001f,zscale=group?scale*0.5f:scale;i.model[0]=i.model[5]=scale;i.model[10]=zscale;i.model[12]=camera.position.x-scale*sky_centers[group].x;i.model[13]=camera.position.y-scale*sky_centers[group].y;i.model[14]=camera.position.z-zscale*sky_centers[group].z;}}
        if(!m.deforms.empty()||!g.rest_pose.empty()){if(g.rest_pose.empty())g.rest_pose=g.final_pose;g.final_pose.assign(g.rest_pose.begin(),g.rest_pose.end());g.dynamic=true;deform_owned(*this,g,m,i.model);}
        if(g.dynamic)publish_geometry(*this,g);else{if(g.vertices.handle)g.final_vertices=range(g.vertices);g.index_range=range(g.indices);g.vertex_count=std::uint32_t(g.final_pose.size());g.index_count=std::uint32_t(g.index_type==VK_INDEX_TYPE_UINT16?g.indices16.size():g.indices32.size());if(!g.payload_signature)g.payload_signature=hash_bytes(std::as_bytes(std::span(g.final_pose)));}
        append_instance(*this,i);
    }
    transient_lighting.resize(scene.objects.size());
    for(unsigned ordinal=0;ordinal<scene.objects.size();++ordinal){
        const auto& object=scene.objects[ordinal];if(!object.visible||!object.mesh||object.mesh->indices.empty())continue;
        auto material_id=object.material_override?material_ids.at(object.material_override):prepared_material(*this,object.material);auto& m=pass_materials[material_id];
        auto mesh_id=mesh_ids.at(object.mesh);auto& base=geometry[mesh_id];bool grid=object.vertex_colors.empty()&&level&&!level->light_grid.samples.empty()&&std::any_of(m.passes.begin(),m.passes.end(),[](auto& p){return p.rgb_gen==MaterialRgbGen::lighting_diffuse||p.rgb_gen==MaterialRgbGen::vertex;});
        bool dynamic=!object.bones.empty()||!object.vertex_colors.empty()||grid||!m.deforms.empty();auto geometry_id=mesh_id;
        unsigned part=ordinal;if(object.lighting_id){part=0;for(unsigned previous=0;previous<ordinal;++previous)if(scene.objects[previous].lighting_id==object.lighting_id)++part;}
        if(dynamic){GeometryIdentity identity;identity.generation=generation;identity.object=object.lighting_id;identity.geometry_generation=object.geometry_generation;identity.part=part;identity.transient=object.lighting_id==0;identity.surface=mesh_id;geometry_id=dynamic_geometry(*this,identity,base.topology_signature,base.final_pose,base.indices16,{},base.primitive,range(base.indices),!object.bones.empty());}
        auto& g=geometry[geometry_id];ObjectLighting* lighting=nullptr;
        if(dynamic&&object.vertex_colors.empty()&&!grid)for(auto& v:g.final_pose)v.color={1,1,1,1};
        if(dynamic){if(object.lighting_id){auto& parts=object_lighting[object.lighting_id];if(parts.size()<=part)parts.resize(part+1);lighting=&parts[part];}else{lighting=&transient_lighting[ordinal];lighting->valid=false;}
            if(lighting->mesh!=object.mesh||lighting->geometry_generation!=object.geometry_generation){lighting->mesh=object.mesh;lighting->geometry_generation=object.geometry_generation;lighting->valid=false;lighting->colors.assign(object.mesh->positions.size(),{255,255,255,255});}
            if(!object.bones.empty()){lighting->positions.resize(g.final_pose.size());lighting->normals.resize(g.final_pose.size());skin_mesh(*object.mesh,object.bones,lighting->positions,lighting->normals);for(unsigned k=0;k<g.final_pose.size();++k){g.final_pose[k].position=lighting->positions[k];g.final_pose[k].normal=lighting->normals[k];}}
            if(!object.vertex_colors.empty()){if(object.vertex_colors.size()!=g.final_pose.size())throw std::runtime_error("Object vertex-color count differs from mesh");for(unsigned k=0;k<g.final_pose.size();++k)for(unsigned c=0;c<4;++c)g.final_pose[k].color[c]=object.vertex_colors[k][c]/255.0f;}
            if(grid){lighting->colors.resize(g.final_pose.size(),{255,255,255,255});Vec3 origin=object.lighting_origin.value_or(Vec3{object.transform[12],object.transform[13],object.transform[14]});bool moved=origin.x!=lighting->origin.x||origin.y!=lighting->origin.y||origin.z!=lighting->origin.z;
                bool changed=!lighting->valid||lighting->mode!=m.lightgrid||moved||(!object.lighting_id&&lighting->transform!=object.transform)||!object.mesh->bones.empty();
                if(changed){std::array<std::uint8_t,4> uniform{255,255,255,255};if(m.lightgrid==MaterialLightGrid::center||m.lightgrid==MaterialLightGrid::interpolate_once)uniform=grid_diffuse(level->light_grid,level->bounds.minimum,origin,{0,0,1});
                    if(m.lightgrid==MaterialLightGrid::interpolate_once){constexpr float gain=std::bit_cast<float>(std::uint32_t{0x3b9a33ce});Vec3 c{uniform[0]*gain,uniform[1]*gain,uniform[2]*gain};c=c/std::max({1.0f,c.x,c.y,c.z});uniform[0]=std::uint8_t(original_integer(c.x*255));uniform[1]=std::uint8_t(original_integer(c.y*255));uniform[2]=std::uint8_t(original_integer(c.z*255));}
                    for(unsigned k=0;k<g.final_pose.size();++k){auto c=m.lightgrid==MaterialLightGrid::interpolate?grid_diffuse(level->light_grid,level->bounds.minimum,transform_point(object.transform,g.final_pose[k].position),transform_vector(object.transform,g.final_pose[k].normal)):uniform;if(m.lightgrid==MaterialLightGrid::interpolate_once)c[3]=lighting->colors[k][3];lighting->colors[k]=c;}
                    lighting->origin=origin;lighting->transform=object.transform;lighting->mode=m.lightgrid;lighting->valid=true;
                }for(unsigned k=0;k<g.final_pose.size();++k)for(unsigned c=0;c<4;++c)g.final_pose[k].color[c]=lighting->colors[k][c]/255.0f;
            }
            deform_owned(*this,g,m,object.transform);
        }
        publish_geometry(*this,g);PreparedInstance i;i.geometry=geometry_id;i.material=material_id;i.model=object.transform;i.color=object.color;i.visible=true;i.lighting_id=object.lighting_id;i.frame_ordinal=ordinal;i.lightmap=0;i.part_bounds=object.mesh->bones.empty()?object.mesh->bounds:Bounds{{-11,-49.08399963378906f,-11},{11,49.08399963378906f,11}};i.has_culling=object.culling_transform!=nullptr;if(i.has_culling)i.culling_transform=*object.culling_transform;i.has_root=object.root_bounds!=nullptr;if(i.has_root)i.root_bounds=*object.root_bounds;i.culling_enabled=object.culling_enabled;i.frustum_cull=object.frustum_cull;append_instance(*this,i,grid);
    }
    auto axes=inverse_rigid(camera.view);auto quad=unit_quad();static constexpr std::uint32_t quad_indices[]{0,1,2,0,2,3},triangle_indices[]{0,1,2};
    for(unsigned ordinal=0;ordinal<scene.particles.size();++ordinal){auto& p=scene.particles[ordinal];if(!p.triangle&&(p.size.x<=0||p.size.y<=0))continue;GeometryIdentity identity;identity.generation=generation;identity.transient=true;identity.object=0;identity.part=ordinal;identity.surface=invalid_id-1;std::uint32_t material_id=prepared_material(*this,p.material);std::uint32_t id;Matrix model=render_identity;
        if(p.triangle){auto n=original_normalized(cross(p.triangle_positions[1]-p.triangle_positions[0],p.triangle_positions[2]-p.triangle_positions[0]));for(unsigned k=0;k<3;++k){quad[k].position=p.triangle_positions[k];quad[k].uv=p.triangle_uv[k];quad[k].normal=n;quad[k].color=p.color;}id=dynamic_geometry(*this,identity,3,std::span<const RenderVertex>(quad.data(),3),{},triangle_indices,Primitive::triangles,range(particle_triangle_index_buffer));}
        else{quad=unit_quad();for(auto& v:quad)v.color=p.color;auto orientation=p.autosprite?axes:p.orientation;float c=std::cos(p.rotation),sn=std::sin(p.rotation);Vec3 right{orientation[0],orientation[1],orientation[2]},up{orientation[4],orientation[5],orientation[6]};auto r=(right*c+up*sn)*p.size.x,u=(up*c-right*sn)*p.size.y,n=original_normalized(cross(right,up)),corner=p.position-(r+u)*0.5f;model={r.x,r.y,r.z,0,u.x,u.y,u.z,0,n.x,n.y,n.z,0,corner.x,corner.y,corner.z,1};id=dynamic_geometry(*this,identity,6,quad,{},quad_indices,Primitive::triangles,range(interface_index_buffer));}
        auto& g=geometry[id];deform_owned(*this,g,pass_materials[material_id],model);publish_geometry(*this,g);PreparedInstance i;i.geometry=id;i.material=material_id;i.model=model;i.visible=true;i.particle=true;i.lightmap=0;append_instance(*this,i);
    }
}

void VulkanRenderer::State::upload_material_tables(VkCommandBuffer) {
    auto upload=[&]<class T>(const std::vector<T>& values,VkBufferUsageFlags usage){const T empty{};auto data=values.empty()?std::span<const T>(&empty,1):std::span<const T>(values);if(data.size_bytes()>properties.limits.maxStorageBufferRange)throw std::runtime_error("Original material table exceeds Vulkan storage range");return upload_bytes(std::as_bytes(data),std::max<VkDeviceSize>(16,properties.limits.minStorageBufferOffsetAlignment),usage);};
    frame_range=upload_bytes(std::as_bytes(std::span(&gpu_frame,1)),properties.limits.minUniformBufferOffsetAlignment,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    passes_range=upload(gpu_passes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);instances_range=upload(gpu_instances,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);geometry_range=upload(gpu_geometry,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);triangles_range=upload(gpu_triangles,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);coverage_range=upload(gpu_coverage,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);coverage_passes_range=upload(coverage_passes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if(rt_enabled){ray_instances_range=upload(gpu_ray_instances,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);primitive_map_range=upload(primitive_map,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);}
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=frames[frame_index].descriptors;allocation.descriptorSetCount=1;allocation.pSetLayouts=&material_descriptors;checked(vkAllocateDescriptorSets(device,&allocation,&active_material_set),"Allocate slot-owned material set");
    std::array<BufferRange,13> ranges{};ranges[0]=frame_range;ranges[1]=passes_range;ranges[2]=instances_range;ranges[3]=geometry_range;ranges[4]=triangles_range;ranges[8]=coverage_range;ranges[9]=coverage_passes_range;ranges[11]=ray_instances_range;ranges[12]=primitive_map_range;
    std::array<VkDescriptorBufferInfo,13> buffers{};std::array<VkWriteDescriptorSet,13> writes{};unsigned count=0;
    for(unsigned k=0;k<(rt_enabled?13u:10u);++k){if(k>=5&&k<=7||k==10)continue;buffers[k]={ranges[k].buffer,ranges[k].offset,ranges[k].size};auto& w=writes[count++];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=active_material_set;w.dstBinding=k;w.descriptorCount=1;w.descriptorType=k==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&buffers[k];}
    descriptor_images_2d.resize(images.size());descriptor_images_cube.resize(images.size());
    VkImageView white=images.at(0).image.view,cube{};
    for(auto& r:images)if(r.image.layers==6){cube=r.image.view;break;}
    // Every written descriptor is valid for its view type; unused cube holes remain partially bound.
    for(unsigned k=0;k<images.size();++k){bool is_cube=images[k].image.layers==6;descriptor_images_2d[k]={{},is_cube?white:images[k].image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};descriptor_images_cube[k]={{},is_cube?images[k].image.view:cube,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};}
    auto image_write=[&](unsigned binding,const VkDescriptorImageInfo* info,unsigned n,VkDescriptorType type){auto& w=writes[count++];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=active_material_set;w.dstBinding=binding;w.descriptorCount=n;w.descriptorType=type;w.pImageInfo=info;};
    if(!images.empty())image_write(5,descriptor_images_2d.data(),std::uint32_t(images.size()),VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    if(cube&&!images.empty())image_write(6,descriptor_images_cube.data(),std::uint32_t(images.size()),VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    std::array<VkDescriptorImageInfo,19> sampler_info{};for(unsigned k=0;k<18;++k)sampler_info[k].sampler=samplers[k];sampler_info[18].sampler=font_sampler;image_write(7,sampler_info.data(),19,VK_DESCRIPTOR_TYPE_SAMPLER);
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    if(rt_enabled&&tlas[frame_index].handle){acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&tlas[frame_index].handle;auto& w=writes[count++];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.pNext=&acceleration;w.dstSet=active_material_set;w.dstBinding=10;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;}
    vkUpdateDescriptorSets(device,count,writes.data(),0,nullptr);
}
VkPipeline VulkanRenderer::State::material_pipeline(const PipelineKey& key) {
    for(auto& cached:pipelines)if(cached.key==key)return cached.pipeline;
    if(key.rays&&(!rt_enabled||!shaders.material_ray_fragment))throw std::runtime_error("Requested ray-query material pipeline is unavailable");
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=shaders.material_vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=key.rays?shaders.material_ray_fragment:shaders.material_fragment;
    VkVertexInputBindingDescription vertex{0,sizeof(RenderVertex),VK_VERTEX_INPUT_RATE_VERTEX};constexpr std::array<VkVertexInputAttributeDescription,5> attributes{{{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(RenderVertex,position)},{1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(RenderVertex,uv)},{2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(RenderVertex,light_uv)},{3,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(RenderVertex,normal)},{4,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(RenderVertex,color)}}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};input.vertexBindingDescriptionCount=1;input.pVertexBindingDescriptions=&vertex;input.vertexAttributeDescriptionCount=std::uint32_t(attributes.size());input.pVertexAttributeDescriptions=attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=key.primitive==Primitive::triangle_strip?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
    // Preserve GL_CW for world, actors and cube/history captures: the negative-height
    // viewport reverses the shoelace sign, and Vulkan's signed area is -shoelace/2.
    // These reversals cancel; changing to CCW would invert every authored cull mode.
    // Original 0040f579–0040f591 uses authored * 0.5f for slope and authored for units.
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;raster.frontFace=VK_FRONT_FACE_CLOCKWISE;raster.cullMode=key.cull==MaterialCull::none?VK_CULL_MODE_NONE:key.cull==MaterialCull::front?VK_CULL_MODE_FRONT_BIT:VK_CULL_MODE_BACK_BIT;raster.depthBiasEnable=key.polygon_offset!=0;raster.depthBiasSlopeFactor=key.polygon_offset*0.5f;raster.depthBiasConstantFactor=key.polygon_offset;
    VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};msaa.rasterizationSamples=key.samples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depth.depthTestEnable=key.depth_test;depth.depthWriteEnable=key.depth_write;depth.depthCompareOp=key.equal?VK_COMPARE_OP_EQUAL:VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};attachment.blendEnable=key.blend;attachment.srcColorBlendFactor=blend_factor(key.source);attachment.dstColorBlendFactor=blend_factor(key.destination);attachment.colorBlendOp=VK_BLEND_OP_ADD;attachment.srcAlphaBlendFactor=alpha_factor(key.source);attachment.dstAlphaBlendFactor=alpha_factor(key.destination);attachment.alphaBlendOp=VK_BLEND_OP_ADD;attachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
    constexpr VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamics{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamics.dynamicStateCount=2;dynamics.pDynamicStates=dynamic;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pipeline.stageCount=2;pipeline.pStages=stages.data();pipeline.pVertexInputState=&input;pipeline.pInputAssemblyState=&assembly;pipeline.pViewportState=&viewport;pipeline.pRasterizationState=&raster;pipeline.pMultisampleState=&msaa;pipeline.pDepthStencilState=&depth;pipeline.pColorBlendState=&blend;pipeline.pDynamicState=&dynamics;pipeline.layout=material_layout;pipeline.renderPass=key.render_pass;
    VkPipeline result{};checked(vkCreateGraphicsPipelines(device,{},1,&pipeline,nullptr,&result),"Create original material pipeline");pipelines.push_back({key,result});return result;
}
void VulkanRenderer::State::draw_instance(VkCommandBuffer command,std::uint32_t instance_id,bool interface) {
    if(!active_target)throw std::runtime_error("Material draw requires active target");auto& i=prepared_instances.at(instance_id);auto& g=geometry.at(i.geometry);if(!g.index_count||!i.pass_count)return;
    auto offset=g.final_vertices.offset;vkCmdBindVertexBuffers(command,0,1,&g.final_vertices.buffer,&offset);vkCmdBindIndexBuffer(command,g.index_range.buffer,g.index_range.offset,g.index_type);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,material_layout,0,1,&active_material_set,0,nullptr);
    for(std::uint32_t ordinal=0;ordinal<i.pass_count;++ordinal){auto id=i.first_pass+ordinal;auto& p=gpu_passes[id];PipelineKey key;key.render_pass=active_target->render_pass;key.samples=active_target->samples;key.primitive=g.primitive;key.cull=MaterialCull(p.blend[3]);key.source=MaterialBlendFactor(p.blend[0]);key.destination=MaterialBlendFactor(p.blend[1]);key.blend=p.modes[2]&32;key.depth_test=p.blend[2]&1;key.depth_write=p.blend[2]&2;key.equal=p.blend[2]&4;key.interface=interface;key.hdr=gpu_frame.flags[0];key.rays=rt_enabled&&!interface&&capture_mode==CaptureMode::ordinary;key.polygon_offset=p.parameters[0];
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,material_pipeline(key));bool pixels=interface&&(gpu_instances[instance_id].metadata[3]&2u);if(pixels&&active_target->extent.width>(std::numeric_limits<std::uint32_t>::max()>>2))throw std::runtime_error("Pixel UI width exceeds push-coordinate range");DrawPush push{instance_id,id,interface?(pixels?(active_target->extent.width<<2)|3u:1u):0u,pixels?std::bit_cast<std::uint32_t>(float(active_target->extent.height)):0u};vkCmdPushConstants(command,material_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);vkCmdDrawIndexed(command,g.index_count,1,0,0,0);
    }
    g.last_submission=submission_serial+1;
}
void VulkanRenderer::State::record_world_entities(VkCommandBuffer command,const RenderScene&,CaptureMode) {
    if(level){
        if(++frame_stamp==0){std::fill(surface_stamps.begin(),surface_stamps.end(),0);std::fill(node_stamps.begin(),node_stamps.end(),0);std::fill(leaf_stamps.begin(),leaf_stamps.end(),0);frame_stamp=1;}
        auto leaf=leaf_at(*level,camera.position);int cluster=leaf<0?-1:level->leaves.at(leaf).cluster;
        if(level->leaves.empty())std::fill(surface_stamps.begin(),surface_stamps.end(),frame_stamp);
        else{
            for(unsigned ordinal=0;ordinal<level->leaves.size();++ordinal){auto& l=level->leaves[ordinal];if(cluster>=0&&l.cluster>=0&&!level->visibility.visible(cluster,l.cluster))continue;leaf_stamps[ordinal]=frame_stamp;for(auto p=leaf_parents.at(ordinal);p>=0;p=node_parents.at(p)){if(node_stamps[p]==frame_stamp)break;node_stamps[p]=frame_stamp;}}
            auto visit=[&](auto&& self,std::int32_t index,std::uint32_t mask,std::size_t depth)->void{if(depth>level->nodes.size())throw std::runtime_error("Cyclic BSP visibility tree");if(index>=0){auto n=unsigned(index);if(node_stamps.at(n)!=frame_stamp)return;auto& node=level->nodes.at(n);if(!frustum.visible(node.bounds,node_culls.at(n),mask))return;self(self,node.children[0],mask,depth+1);self(self,node.children[1],mask,depth+1);}
                else{auto n=unsigned(~index);if(leaf_stamps.at(n)!=frame_stamp)return;auto& l=level->leaves.at(n);if(!frustum.visible(l.bounds,leaf_culls.at(n),mask))return;for(unsigned k=0;k<l.surface_count;++k)surface_stamps.at(level->leaf_surfaces.at(l.first_surface+k))=frame_stamp;}};
            if(level->nodes.empty())for(unsigned k=0;k<level->leaves.size();++k)visit(visit,~int(k),63,0);else visit(visit,0,63,0);
        }
    }
    for(unsigned id=0;id<prepared_instances.size();++id){auto& i=prepared_instances[id];if(!i.visible||i.interface)continue;auto& g=geometry[i.geometry];
        if(i.world){if(!g.sky&&surface_stamps.at(g.identity.surface)!=frame_stamp)continue;}
        else if(!i.particle&&i.culling_enabled&&i.has_culling){if(i.has_root){auto root=cache_bounds(i.root_bounds);if(!frustum.visible(root,i.culling_transform))continue;}if(i.frustum_cull){auto part=cache_bounds(i.part_bounds);if(!frustum.visible(part,i.culling_transform))continue;}}
        draw_instance(command,id);
    }
}
void VulkanRenderer::State::record_interface(VkCommandBuffer command,std::span<const InterfaceQuad> quads,bool capture_only) {
    if(!active_target)throw std::runtime_error("Interface draw requires active target");auto size=active_target->extent;int width=int(size.width),height=int(size.height);auto ui=interface_extent(width,height);unsigned first=std::uint32_t(prepared_instances.size());
    if(interface_geometry.size()<quads.size())throw std::runtime_error("Renderer.prepare required after interface geometry change");
    for(unsigned ordinal=0;ordinal<quads.size();++ordinal){auto& q=quads[ordinal];if(capture_only&&!q.history_capture_visible)continue;auto rect=interface_rect(q,width,height);if(rect.width<=0||rect.height<=0)continue;auto id=interface_geometry[ordinal];auto& g=geometry.at(id);auto v=unit_quad();v[0].uv={q.uv.x,q.uv.y};v[1].uv={q.uv.x+q.uv.width,q.uv.y};v[2].uv={q.uv.x+q.uv.width,q.uv.y+q.uv.height};v[3].uv={q.uv.x,q.uv.y+q.uv.height};g.final_pose.assign(v.begin(),v.end());g.final_vertices=upload_bytes(std::as_bytes(std::span(g.final_pose)),4,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);g.index_range=range(interface_index_buffer);g.vertex_count=4;g.index_count=6;g.index_type=VK_INDEX_TYPE_UINT32;
        PreparedInstance i;i.geometry=id;i.interface=true;i.visible=true;i.color=q.color;i.model=interface_model(q,rect);i.frame_ordinal=ordinal;i.lightmap=0;
        i.material=q.font_atlas?0:prepared_material(*this,q.shader,q.shader.find("textures")!=std::string_view::npos);append_instance(*this,i);
        auto instance_id=std::uint32_t(prepared_instances.size()-1);gpu_instances.back().metadata[3]=q.drawable_pixel_coordinates?3u:1u;
        if(q.font_atlas){auto font=font_atlases.find(q.shader);if(font==font_atlases.end())throw std::runtime_error("Renderer.prepare required after font atlas change");auto& p=gpu_passes[prepared_instances[instance_id].first_pass];p.binding={font->second,18,0,0};p.modes[2]|=2;}
    }
    auto saved=gpu_frame.flags;if(active_target==&output_target)gpu_frame.flags[0]=0;gpu_frame.flags[1]=gpu_frame.flags[2]=gpu_frame.flags[3]=0;
    if(prepared_instances.size()>first)upload_material_tables(command);
    for(unsigned id=first;id<prepared_instances.size();++id){auto& i=prepared_instances[id];auto& q=quads[i.frame_ordinal];
        if(q.mask_legacy_margins){auto bands=interface_margin_bands(width,height,ui[0],ui[1]);VkClearAttachment clear{};clear.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;clear.colorAttachment=0;clear.clearValue.color.float32[3]=1;for(auto& band:bands)if(band[2]>0&&band[3]>0){VkClearRect rect{{{band[0],height-band[1]-band[3]},{unsigned(band[2]),unsigned(band[3])}},0,1};vkCmdClearAttachments(command,1,&clear,1,&rect);}}
        bool native=q.full_viewport||q.native_bar_fraction.has_value()||q.drawable_pixel_coordinates;int x=native?0:(width-ui[0])/2,y=native?0:(height-ui[1])/2,w=native?width:ui[0],h=native?height:ui[1];VkViewport viewport{float(x),float(y+h),float(w),-float(h),0,1};VkRect2D scissor{{x,y},{unsigned(w),unsigned(h)}};vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);draw_instance(command,id,true);
    }
    gpu_frame.flags=saved;VkViewport viewport{0,float(height),float(width),-float(height),0,1};VkRect2D scissor{{0,0},size};vkCmdSetViewport(command,0,1,&viewport);vkCmdSetScissor(command,0,1,&scissor);
}
} // namespace pusu
