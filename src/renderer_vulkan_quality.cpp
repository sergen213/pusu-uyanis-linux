#include "renderer_vulkan_private.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace pusu {
namespace {
using namespace vk_detail;
void checked(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" failed ("+std::to_string(result)+")");
}
struct QualityPush {
    std::uint32_t operation{},hdr{},layer{},adapt{};
    float dx{},dy{},gamma{1},exposure{};
    float strength{},alpha{1};std::uint32_t padding[2]{};
};
static_assert(sizeof(QualityPush)==48&&offsetof(QualityPush,dx)==16&&offsetof(QualityPush,strength)==32);
Image& sampled(Target& target) {return target.samples==VK_SAMPLE_COUNT_1_BIT?target.color:target.resolve;}
bool srgb_format(VkFormat format) {
    return format==VK_FORMAT_B8G8R8A8_SRGB||format==VK_FORMAT_R8G8B8A8_SRGB||format==VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}
template<class State> void release_target(State& s,Target& target) noexcept {
    if(target.framebuffer)vkDestroyFramebuffer(s.device,target.framebuffer,nullptr);
    if(target.render_pass)vkDestroyRenderPass(s.device,target.render_pass,nullptr);
    s.destroy_image(target.color);s.destroy_image(target.resolve);s.destroy_image(target.depth);target={};
}
VkRenderPass render_pass(VkDevice device,VkFormat color,VkFormat depth,VkSampleCountFlagBits samples) {
    const bool has_depth=depth!=VK_FORMAT_UNDEFINED,resolve=samples!=VK_SAMPLE_COUNT_1_BIT;
    std::array<VkAttachmentDescription,3> attachments{};
    attachments[0].format=color;attachments[0].samples=samples;
    attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachments[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout=attachments[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    std::uint32_t count=1;VkAttachmentReference color_ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depth_ref{},resolve_ref{};
    if(has_depth) {
        depth_ref={count,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};auto& a=attachments[count++];
        a.format=depth;a.samples=samples;a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if(resolve) {
        resolve_ref={count,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};auto& a=attachments[count++];
        a=attachments[0];a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color_ref;
    if(has_depth)subpass.pDepthStencilAttachment=&depth_ref;
    if(resolve)subpass.pResolveAttachments=&resolve_ref;
    std::array<VkSubpassDependency,2> dependencies{};
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=dependencies[0].dstStageMask;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask=dependencies[0].dstAccessMask;dependencies[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=count;info.pAttachments=attachments.data();
    info.subpassCount=1;info.pSubpasses=&subpass;info.dependencyCount=2;info.pDependencies=dependencies.data();
    VkRenderPass result{};checked(vkCreateRenderPass(device,&info,nullptr,&result),"Creating quality render pass");return result;
}
template<class State> void make_target(State& s,Target& target,VkExtent2D extent,VkFormat color,VkSampleCountFlagBits samples,bool depth) {
    target.extent=extent;target.samples=samples;
    try {
        const auto usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|(samples==VK_SAMPLE_COUNT_1_BIT?
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT:0);
        target.color=s.create_image(extent,color,usage,samples);
        if(samples!=VK_SAMPLE_COUNT_1_BIT)target.resolve=s.create_image(extent,color,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if(depth)target.depth=s.create_image(extent,s.depth_format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,samples);
        target.render_pass=render_pass(s.device,color,depth?s.depth_format:VK_FORMAT_UNDEFINED,samples);
        std::array<VkImageView,3> views{target.color.view};std::uint32_t count=1;
        if(depth)views[count++]=target.depth.view;if(samples!=VK_SAMPLE_COUNT_1_BIT)views[count++]=target.resolve.view;
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};info.renderPass=target.render_pass;info.attachmentCount=count;info.pAttachments=views.data();
        info.width=extent.width;info.height=extent.height;info.layers=1;
        checked(vkCreateFramebuffer(s.device,&info,nullptr,&target.framebuffer),"Creating quality framebuffer");
    } catch(...) {release_target(s,target);throw;}
}
template<class State> VkPipeline screen_pipeline(State& s,VkRenderPass pass,VkSampleCountFlagBits samples,VkShaderModule fragment,bool blend) {
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};
    for(auto& stage:stages){stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stage.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=s.shaders.fullscreen_vertex;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;
    raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=samples;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;attachment.blendEnable=blend;
    attachment.srcColorBlendFactor=attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor=attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp=attachment.alphaBlendOp=VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo color{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};color.attachmentCount=1;color.pAttachments=&attachment;
    const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamic_states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};info.stageCount=2;info.pStages=stages.data();
    info.pVertexInputState=&vertex;info.pInputAssemblyState=&assembly;info.pViewportState=&viewport;info.pRasterizationState=&raster;
    info.pMultisampleState=&multisample;info.pDepthStencilState=&depth;info.pColorBlendState=&color;info.pDynamicState=&dynamic;
    info.layout=s.post_layout;info.renderPass=pass;
    VkPipeline result{};checked(vkCreateGraphicsPipelines(s.device,VK_NULL_HANDLE,1,&info,nullptr,&result),"Creating quality pipeline");return result;
}
template<class State> void screen_draw(State& s,VkCommandBuffer command,VkPipeline pipeline,Image& source,Image& bloom,QualityPush push) {
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=s.frames[s.frame_index].descriptors;
    allocate.descriptorSetCount=1;allocate.pSetLayouts=&s.post_descriptors;VkDescriptorSet set{};
    checked(vkAllocateDescriptorSets(s.device,&allocate,&set),"Allocating quality descriptors");
    const VkDescriptorImageInfo infos[]{
        {s.post_sampler,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.post_sampler,bloom.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.history_sampler,s.history.image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    std::array<VkWriteDescriptorSet,3> writes{};
    for(std::uint32_t i=0;i<writes.size();++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstBinding=i;
        w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&infos[i];}
    vkUpdateDescriptorSets(s.device,std::uint32_t(writes.size()),writes.data(),0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,s.post_layout,0,1,&set,0,nullptr);
    vkCmdPushConstants(command,s.post_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);vkCmdDraw(command,3,1,0,0);
}
template<class State> void sampled_layout(State& s,VkCommandBuffer cmd,Image& image) {
    s.transition_image(cmd,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
}
void viewport(VkCommandBuffer command,VkExtent2D extent) {
    VkViewport value{0,float(extent.height),float(extent.width),-float(extent.height),0,1};VkRect2D scissor{{0,0},extent};
    vkCmdSetViewport(command,0,1,&value);vkCmdSetScissor(command,0,1,&scissor);
}
template<class State> void invalidate(State& s,const Buffer& buffer) {
    if(!buffer.coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=buffer.memory;range.size=VK_WHOLE_SIZE;
        checked(vkInvalidateMappedMemoryRanges(s.device,1,&range),"Invalidating cube readback");}
}
} // namespace

void VulkanRenderer::State::initialize_quality() {
    std::array<VkDescriptorSetLayoutBinding,3> bindings{};
    for(std::uint32_t i=0;i<bindings.size();++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}
    VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptors.bindingCount=3;descriptors.pBindings=bindings.data();
    checked(vkCreateDescriptorSetLayout(device,&descriptors,nullptr,&post_descriptors),"Creating quality descriptor layout");
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(QualityPush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&post_descriptors;
    layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    checked(vkCreatePipelineLayout(device,&layout,nullptr,&post_layout),"Creating quality pipeline layout");
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;sampler.maxLod=0;
    checked(vkCreateSampler(device,&sampler,nullptr,&post_sampler),"Creating quality sampler");
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    checked(vkCreateSampler(device,&sampler,nullptr,&history_sampler),"Creating original history sampler");
}
void VulkanRenderer::State::destroy_quality() noexcept {
    destroy_targets();
    if(post_sampler)vkDestroySampler(device,post_sampler,nullptr);
    if(history_sampler)vkDestroySampler(device,history_sampler,nullptr);
    if(post_layout)vkDestroyPipelineLayout(device,post_layout,nullptr);
    if(post_descriptors)vkDestroyDescriptorSetLayout(device,post_descriptors,nullptr);
    post_sampler={};history_sampler={};post_layout={};post_descriptors={};
}
void VulkanRenderer::State::create_targets(VkExtent2D size,VkSampleCountFlagBits sample_count) {
    if(!size.width||!size.height)return;
    extent=size;samples=sample_count;
    try {
        make_target(*this,scene_target,size,VK_FORMAT_R16G16B16A16_SFLOAT,samples,true);
        make_target(*this,output_target,size,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,false);
        VkExtent2D half{std::max(1u,size.width/2),std::max(1u,size.height/2)};
        for(auto& target:blur_targets)make_target(*this,target,half,VK_FORMAT_R16G16B16A16_SFLOAT,VK_SAMPLE_COUNT_1_BIT,false);
        output_render_pass=render_pass(device,swap_format,VK_FORMAT_UNDEFINED,VK_SAMPLE_COUNT_1_BIT);
        for(auto& image:swap_images){VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};info.renderPass=output_render_pass;
            info.attachmentCount=1;info.pAttachments=&image.view;info.width=size.width;info.height=size.height;info.layers=1;
            checked(vkCreateFramebuffer(device,&info,nullptr,&image.framebuffer),"Creating swapchain quality framebuffer");}
        post_pipeline=screen_pipeline(*this,output_target.render_pass,VK_SAMPLE_COUNT_1_BIT,shaders.post_fragment,false);
        blur_pipeline=screen_pipeline(*this,blur_targets[0].render_pass,VK_SAMPLE_COUNT_1_BIT,shaders.post_fragment,false);
        swap_pipeline=screen_pipeline(*this,output_render_pass,VK_SAMPLE_COUNT_1_BIT,shaders.post_fragment,false);
        history_pipeline=screen_pipeline(*this,scene_target.render_pass,samples,shaders.history_fragment,true);
        reset_history();
    }catch(...){destroy_targets();throw;}
}
void VulkanRenderer::State::destroy_targets() noexcept {
    // Caller has drained submissions before any render-pass/pipeline lifetime cutover.
    active_target=nullptr;
    for(auto& pipeline:pipelines)if(pipeline.pipeline)vkDestroyPipeline(device,pipeline.pipeline,nullptr);pipelines.clear();
    for(auto* pipeline:{&post_pipeline,&blur_pipeline,&swap_pipeline,&history_pipeline}){if(*pipeline)vkDestroyPipeline(device,*pipeline,nullptr);*pipeline={};}
    for(auto& image:swap_images){if(image.framebuffer)vkDestroyFramebuffer(device,image.framebuffer,nullptr);image.framebuffer={};}
    if(output_render_pass)vkDestroyRenderPass(device,output_render_pass,nullptr);output_render_pass={};
    release_target(*this,scene_target);release_target(*this,output_target);release_target(*this,cube_target);
    for(auto& target:blur_targets)release_target(*this,target);
    release_target(*this,history.target);release_target(*this,history.display_target);
    if(history.layer_view)vkDestroyImageView(device,history.layer_view,nullptr);
    destroy_image(history.image);
    const auto tick=history.tick,order=history.order;const auto serial=history.effect_serial;
    history={};history.tick=tick;history.order=order;history.effect_serial=serial;
}
void VulkanRenderer::State::begin_target(VkCommandBuffer cmd,Target& target,std::array<float,4> clear,bool clear_depth) {
    if(active_target)throw std::logic_error("Nested Vulkan render target");
    if(!clear_depth&&target.depth.handle)throw std::logic_error("Quality depth target requires an explicit fresh clear");
    transition_image(cmd,target.color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if(target.resolve.handle)transition_image(cmd,target.resolve,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if(target.depth.handle)transition_image(cmd,target.depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    if(settings.hdr_tonemapping&&capture_mode!=CaptureMode::cube&&(&target==&scene_target||&target==&history.target))
        for(unsigned i=0;i<3;++i)clear[i]=clear[i]<=0.04045f?clear[i]/12.92f:std::pow((clear[i]+0.055f)/1.055f,2.4f);
    std::array<VkClearValue,3> values{};std::copy(clear.begin(),clear.end(),values[0].color.float32);std::uint32_t count=1;
    if(target.depth.handle)values[count++].depthStencil={1,0};if(target.resolve.handle)values[count++].color=values[0].color;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=target.render_pass;begin.framebuffer=target.framebuffer;
    begin.renderArea={{0,0},target.extent};begin.clearValueCount=count;begin.pClearValues=values.data();
    vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);active_target=&target;viewport(cmd,target.extent);
}
void VulkanRenderer::State::end_target(VkCommandBuffer cmd,Target& target) {
    if(active_target!=&target)throw std::logic_error("Mismatched Vulkan render target end");
    vkCmdEndRenderPass(cmd);active_target=nullptr;sampled_layout(*this,cmd,sampled(target));
}
void VulkanRenderer::State::record_post(VkCommandBuffer cmd) {
    if(active_target)throw std::logic_error("Post processing requires resolved scene");
    auto& scene=sampled(scene_target);QualityPush push;push.hdr=settings.hdr_tonemapping;push.gamma=settings.gamma;push.exposure=settings.exposure_ev;
    if(settings.bloom) {
        push.operation=1;begin_target(cmd,blur_targets[0],{0,0,0,1});screen_draw(*this,cmd,blur_pipeline,scene,scene,push);end_target(cmd,blur_targets[0]);
        push.operation=2;const auto half=blur_targets[0].extent;
        for(unsigned i=0;i<4;++i){push.dx=1.0f/half.width;push.dy=0;
            begin_target(cmd,blur_targets[1],{0,0,0,1});screen_draw(*this,cmd,blur_pipeline,blur_targets[0].color,scene,push);end_target(cmd,blur_targets[1]);
            push.dx=0;push.dy=1.0f/half.height;begin_target(cmd,blur_targets[0],{0,0,0,1});screen_draw(*this,cmd,blur_pipeline,blur_targets[1].color,scene,push);end_target(cmd,blur_targets[0]);}
    }
    push.operation=0;push.dx=push.dy=0;push.strength=settings.bloom?0.25f:0;
    begin_target(cmd,output_target,{0,0,0,1});screen_draw(*this,cmd,post_pipeline,scene,settings.bloom?blur_targets[0].color:scene,push);
    // Intentionally open: HDR ordinary UI composites here, untouched by world post.
}
void VulkanRenderer::State::record_output_to_swapchain(VkCommandBuffer cmd) {
    if(active_target)throw std::logic_error("Output must close before swapchain composition");
    auto& image=swap_images.at(image_index);
    VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};acquire.oldLayout=image.layout;acquire.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    acquire.srcQueueFamilyIndex=acquire.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    if(graphics_family!=present_family&&image.layout!=VK_IMAGE_LAYOUT_UNDEFINED){acquire.srcQueueFamilyIndex=present_family;acquire.dstQueueFamilyIndex=graphics_family;}
    acquire.image=image.handle;acquire.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};acquire.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&acquire);
    VkClearValue clear{};clear.color.float32[3]=1;VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=output_render_pass;
    begin.framebuffer=image.framebuffer;begin.renderArea={{0,0},extent};begin.clearValueCount=1;begin.pClearValues=&clear;
    vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);viewport(cmd,extent);
    QualityPush push;push.operation=4;push.adapt=srgb_format(swap_format);
    screen_draw(*this,cmd,swap_pipeline,output_target.color,output_target.color,push);vkCmdEndRenderPass(cmd);
    VkImageMemoryBarrier release=acquire;release.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;release.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    release.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;release.dstAccessMask=0;
    release.srcQueueFamilyIndex=release.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    if(graphics_family!=present_family){release.srcQueueFamilyIndex=graphics_family;release.dstQueueFamilyIndex=present_family;}
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&release);
    image.layout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

void VulkanRenderer::State::reset_history() {
    if(!extent.width||!extent.height)return;
    if(active_target)throw std::logic_error("History reset inside render pass");
    wait_idle();
    // HDR changes this pass's format. Never reuse a cached pipeline by a recycled handle.
    std::erase_if(pipelines,[&](const PipelineRecord& entry) {
        if(entry.key.render_pass!=history.target.render_pass)return false;
        vkDestroyPipeline(device,entry.pipeline,nullptr);return true;
    });
    release_target(*this,history.target);release_target(*this,history.display_target);
    if(history.layer_view)vkDestroyImageView(device,history.layer_view,nullptr);
    destroy_image(history.image);
    // Resizing/options invalidate samples, not the original unsigned wait clock.
    const auto tick=history.tick,order=history.order;const auto serial=history.effect_serial;
    history={};history.tick=tick;history.order=order;history.effect_serial=serial;
    unsigned side=std::uint32_t(original.motion_blur_size);
    while(side>extent.width)side/=2;while(side>extent.height)side/=2;side=std::max(side,1u);history.side=side;
    // RGB8 logical history: RGBA8 storage is universally renderable; alpha is always one.
    history.image=create_image({side,side},VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_SAMPLE_COUNT_1_BIT,std::uint32_t(original.motion_blur_frames));
    make_target(*this,history.target,{side,side},settings.hdr_tonemapping?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,true);
    make_target(*this,history.display_target,{side,side},VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,false);
    auto cmd=begin_commands();transition_image(cmd,history.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue clear{};clear.float32[3]=1;VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,history.image.layers};
    vkCmdClearColorImage(cmd,history.image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);sampled_layout(*this,cmd,history.image);finish_commands(cmd,true);
}
void VulkanRenderer::State::record_history_consume(VkCommandBuffer cmd,const RenderScene& scene) {
    if(history.effect_serial!=scene.player_effect_serial){history.effect_serial=scene.player_effect_serial;history.write=history.warmup=0;}
    const auto count=std::uint32_t(original.motion_blur_frames);
    if(!original.motion_blur||!scene.player_effect||!scene.gameplay_frame||capture_mode!=CaptureMode::ordinary||history.warmup<count)return;
    if(active_target!=&scene_target)throw std::logic_error("History consumption requires ordinary scene target");
    QualityPush push;push.hdr=settings.hdr_tonemapping;push.alpha=original.motion_blur_alpha/float(count);
    for(std::uint32_t i=0;i<count;++i){push.layer=(history.order+i)%count;
        // History shader only accesses binding2; scene is not sampled while attached.
        screen_draw(*this,cmd,history_pipeline,history.display_target.color,history.display_target.color,push);}
}
void VulkanRenderer::State::record_history_after_present(const RenderScene& scene,std::span<const InterfaceQuad> quads) {
    if(!presented||!original.motion_blur||!scene.player_effect||!scene.gameplay_frame||capture_mode!=CaptureMode::ordinary||!history.image.handle)return;
    if(history.effect_serial!=scene.player_effect_serial){history.effect_serial=scene.player_effect_serial;history.write=history.warmup=0;}
    if(!(float(std::uint32_t(scene.tick_milliseconds-history.tick))>1000*original.motion_blur_wait_seconds))return;
    const VkExtent2D main_extent=extent;const auto mode=capture_mode;
    const auto main_projection=view_projection;const auto main_frustum=frustum;
    retire_frame(frames[frame_index]);
    auto restore=[&](){extent=main_extent;capture_mode=mode;prepare_frame_geometry(scene);};
    try {
        capture_mode=CaptureMode::history;
        // Rebuild camera-dependent geometry with the main aspect/culling view.
        prepare_frame_geometry(scene);
        // Original history compresses the main view to a square, then stretches it back.
        // Keep its projection/cull frustum; only camera-dependent geometry is re-evaluated.
        view_projection=main_projection;gpu_frame.view_projection=main_projection;frustum=main_frustum;
        extent={history.side,history.side}; // Square viewport and selected-UI coordinates only.
        // Preparation may upload new posed topology/AVI synchronously; record only afterward.
        auto cmd=begin_commands();
        upload_material_tables(cmd);begin_target(cmd,history.target,scene.clear_color);
        record_world_entities(cmd,scene,capture_mode);record_interface(cmd,quads,true);end_target(cmd,history.target);
        QualityPush push;push.operation=3;push.hdr=settings.hdr_tonemapping;
        begin_target(cmd,history.display_target,{0,0,0,1});screen_draw(*this,cmd,post_pipeline,history.target.color,history.target.color,push);end_target(cmd,history.display_target);
        transition_image(cmd,history.display_target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        transition_image(cmd,history.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,history.write%std::uint32_t(original.motion_blur_frames),1};
        copy.extent={history.side,history.side,1};vkCmdCopyImage(cmd,history.display_target.color.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,history.image.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        sampled_layout(*this,cmd,history.image);sampled_layout(*this,cmd,history.display_target.color);
        finish_commands(cmd,true);restore();
        history.tick=scene.tick_milliseconds;history.write=(history.write+1)%std::uint32_t(original.motion_blur_frames);
        history.warmup=std::min(history.warmup+1,std::uint32_t(original.motion_blur_frames));history.order=(history.order+1)%std::uint32_t(original.motion_blur_frames);
    }catch(...){extent=main_extent;capture_mode=mode;throw;}
}

void VulkanRenderer::State::prepare_cube_resources(const RenderScene& scene,std::string_view bsp_path) {
    if(level!=scene.level||generation!=scene.generation||resources_dirty)throw std::runtime_error("Cube cache preparation requires committed prepared Vulkan scene");
    if(acquired||immediate_recording)throw std::runtime_error("Cube cache preparation crossed an unfinished Vulkan frame/upload");
    std::string key(bsp_path);for(char& c:key){if(c=='\\')c='/';else if(c>='A'&&c<='Z')c=char(c-'A'+'a');}
    if(key.ends_with(".pl"))key.resize(key.size()-3);
    if(!reflections_pending&&reflection_path==key)return;reflection_path=key;if(!original.reflections)return;
    // cw2 excludes owned captures generated before the GL_CW raster-facing cutover.
    std::string shape="vk-legacy/cw2/q"+std::to_string(original.reflection_divisor),part="s";
    for(std::size_t first=0;first<capture_images.size();) {
        std::size_t end=first+1;unsigned side=images.at(capture_images[first]).image.extent.width;
        while(end<capture_images.size()&&images.at(capture_images[end]).image.extent.width==side)++end;
        const auto run=std::to_string(side)+"x"+std::to_string(end-first);
        if(part.size()+run.size()+1>200){shape+="/"+part;part="s";}if(part.size()>1)part+="_";part+=run;first=end;
    }
    shape+="/"+(part=="s"?std::string("empty"):part);PrivateCubeCache private_cache(user_data_directory()/"cache",key,shape);
    auto cache=private_cache.read(original.reflection_divisor);bool owned=cache.has_value();
    if(cache)try {
        validate_cube_cache_geometry(*cache);if(cache->entries.size()!=capture_images.size())throw CubeCacheValidationError("Owned cube registry count differs");
        for(std::size_t i=0;i<capture_images.size();++i){const auto& image=images.at(capture_images[i]);owned_cube_cache_faces(*cache,i,image.center,int(image.image.extent.width));}
    }catch(const CubeCacheValidationError& error){std::cerr<<"Invalid owned Vulkan cube cache for "<<key<<": "<<error.what()<<'\n';cache.reset();}
    if(!cache){owned=false;cache=read_cube_cache(assets,"textures/cubemaps/"+key,original.reflection_divisor);
        if(cache){validate_cube_cache_geometry(*cache);for(auto id:capture_images){const auto& image=images.at(id);
            if(cube_cache_region_bytes(*cache,cube_cache_offset(*cache,image.center))!=std::size_t(image.image.extent.width)*image.image.extent.height*18){cache.reset();break;}}}}
    if(cache) {
        // Cached publication also mutates sampled images: neither frame/history slot may still use them.
        wait_idle();
        for(std::size_t i=0;i<capture_images.size();++i){auto& image=images.at(capture_images[i]);
            const auto bytes=owned?owned_cube_cache_faces(*cache,i,image.center,int(image.image.extent.width)):cube_cache_faces(*cache,image.center,int(image.image.extent.width));
            const auto face_size=std::size_t(image.image.extent.width)*image.image.extent.height*3;
            for(unsigned face=0;face<6;++face) {
                retire_frame(frames[frame_index]);upload_image(image,bytes.subspan(face*face_size,face_size),3,face);
            }
        }
        reflections_pending=false;return;
    }
    auto output=private_cache.begin_output();std::vector<std::uint8_t> rgb;
    const auto main_extent=extent;const auto mode=capture_mode;const Vec3 directions[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const Vec3 ups[6]{{0,-1,0},{0,-1,0},{0,0,1},{0,0,-1},{0,-1,0},{0,-1,0}};
    Buffer readback_buffer;
    try {
        wait_idle();capture_mode=CaptureMode::cube;
        for(auto id:capture_images) {
            // No resource admission/progress callbacks while a pass or command batch is live.
            const auto side=images.at(id).image.extent.width;const auto center=images.at(id).center;
            if(cube_target.extent.width!=side) {
                // Prior faces/uploads completed synchronously; retire this pass's pipelines before handle reuse.
                std::erase_if(pipelines,[&](const PipelineRecord& entry) {
                    if(entry.key.render_pass!=cube_target.render_pass)return false;
                    vkDestroyPipeline(device,entry.pipeline,nullptr);return true;
                });
                release_target(*this,cube_target);make_target(*this,cube_target,{side,side},VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,true);
            }
            const auto byte_size=VkDeviceSize(side)*side*4;
            if(readback_buffer.size<byte_size){destroy_buffer(readback_buffer);readback_buffer=create_buffer(byte_size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);}
            rgb.resize(std::size_t(side)*side*3);output.begin_cube(center);extent={side,side};
            for(unsigned face=0;face<6;++face) {
                RenderScene view=scene;auto& camera=view.camera;const auto f=directions[face],r=cross(f,ups[face]),u=cross(r,f);
                camera.position=center;camera.near_plane=4;camera.far_plane=20000;camera.reference_fov_degrees=90;
                camera.view={r.x,u.x,-f.x,0,r.y,u.y,-f.y,0,r.z,u.z,-f.z,0,-dot(r,center),-dot(u,center),dot(f,center),1};
                camera.frame0={r.x,r.y,r.z,0,u.x,u.y,u.z,0,-f.x,-f.y,-f.z,0,center.x,center.y,center.z,1};
                retire_frame(frames[frame_index]);prepare_frame_geometry(view);
                auto projection=perspective(90,4.0f/3,4,1.0e11f);projection[0]=projection[5]=1;
                view_projection=multiply(projection,camera.view);gpu_frame.view_projection=view_projection;
                frustum=model_frustum(camera,90.0L*std::bit_cast<float>(std::uint32_t{0x3c0efa35}),1);
                // Cold dynamic/AVI uploads above own their completed batches, never nest here.
                auto cmd=begin_commands();
                upload_material_tables(cmd);begin_target(cmd,cube_target,scene.clear_color);record_world_entities(cmd,view,CaptureMode::cube);end_target(cmd,cube_target);
                transition_image(cmd,cube_target.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={side,side,1};
                vkCmdCopyImageToBuffer(cmd,cube_target.color.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback_buffer.handle,1,&copy);
                VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=readback_buffer.handle;barrier.size=VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);finish_commands(cmd,true);invalidate(*this,readback_buffer);
                const auto* rgba=static_cast<const std::uint8_t*>(readback_buffer.mapped);
                // Cache/GL cube rows originate at the bottom. Reuse the RGB5 upload codec;
                // the protected cache records the original RGB8 capture before quantization.
                for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned c=0;c<3;++c)
                    rgb[(std::size_t(y)*side+x)*3+c]=rgba[(std::size_t(side-1-y)*side+x)*4+c];
                output.face(rgb);retire_frame(frames[frame_index]);upload_image(images.at(id),rgb,3,face);
            }
        }
        output.commit(original.reflection_divisor);reflections_pending=false;destroy_buffer(readback_buffer);
        extent=main_extent;capture_mode=mode;prepare_frame_geometry(scene);
    }catch(...){destroy_buffer(readback_buffer);extent=main_extent;capture_mode=mode;throw;}
}
} // namespace pusu
