#include "renderer_vulkan_private.hpp"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace pusu {
namespace {
using namespace vk_detail;
void checked(VkResult result,const char* operation) {
    if(result==VK_SUCCESS)return;
    const char* reason=result==VK_ERROR_DEVICE_LOST?"device lost":result==VK_ERROR_SURFACE_LOST_KHR?"surface lost":nullptr;
    throw std::runtime_error(std::string("Vulkan ")+operation+": "+(reason?reason:std::to_string(result)));
}
VkDeviceSize aligned(VkDeviceSize value,VkDeviceSize alignment) {
    alignment=std::max<VkDeviceSize>(alignment,1);
    const auto remainder=value%alignment;
    if(!remainder)return value;
    const auto extra=alignment-remainder;
    if(value>std::numeric_limits<VkDeviceSize>::max()-extra)throw std::runtime_error("Vulkan upload alignment overflow");
    return value+extra;
}
VkInstance make_instance(SDL_Window* window) {
    if(!window)throw std::runtime_error("Vulkan requires an SDL Vulkan window");
    unsigned count{};
    if(!SDL_Vulkan_GetInstanceExtensions(window,&count,nullptr))throw std::runtime_error(SDL_GetError());
    std::vector<const char*> extensions(count);
    if(!SDL_Vulkan_GetInstanceExtensions(window,&count,extensions.data()))throw std::runtime_error(SDL_GetError());
    std::uint32_t version=VK_API_VERSION_1_0;
    const auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr,"vkEnumerateInstanceVersion"));
    if(enumerate)checked(enumerate(&version),"query loader version");
    if(version<VK_API_VERSION_1_2)throw std::runtime_error("Vulkan 1.2 loader required");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName="Pusu: Uyanış";app.applicationVersion=VK_MAKE_VERSION(1,0,0);app.pEngineName="Pusu native";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
    info.enabledExtensionCount=count;info.ppEnabledExtensionNames=extensions.data();
    VkInstance result{};checked(vkCreateInstance(&info,nullptr,&result),"create instance");return result;
}
struct Candidate {
    VkPhysicalDevice physical{};VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{};std::uint32_t graphics{},present{};
    VkFormat depth{VK_FORMAT_UNDEFINED};bool rays{};
    VulkanCapabilities caps;
};
std::vector<VkPresentModeKHR> surface_modes(VkPhysicalDevice physical,VkSurfaceKHR surface) {
    std::uint32_t count{};checked(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&count,nullptr),"query present modes");
    std::vector<VkPresentModeKHR> modes(count);checked(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&count,modes.data()),"query present modes");modes.resize(count);return modes;
}
std::vector<VkSurfaceFormatKHR> surface_formats(VkPhysicalDevice physical,VkSurfaceKHR surface) {
    std::uint32_t count{};checked(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"query surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count);checked(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"query surface formats");formats.resize(count);return formats;
}
VkSurfaceFormatKHR choose_format(VkPhysicalDevice physical,VkSurfaceKHR surface) {
    const auto formats=surface_formats(physical,surface);
    constexpr VkFormat choices[]{VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_SRGB};
    for(const auto wanted:choices)for(const auto& format:formats)
        if(format.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR&&(format.format==wanted||format.format==VK_FORMAT_UNDEFINED))return {wanted,format.colorSpace};
    throw std::runtime_error("Vulkan surface has no supported SDR RGBA8 SRGB_NONLINEAR format");
}
bool format_support(VkPhysicalDevice physical,VkFormat format,VkFormatFeatureFlags flags) {
    VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
    return (properties.optimalTilingFeatures&flags)==flags;
}
Candidate select_device(VkInstance instance,VkSurfaceKHR surface) {
    std::uint32_t count{};checked(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate physical devices");
    std::vector<VkPhysicalDevice> devices(count);checked(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate physical devices");
    Candidate best;int best_score=-1;
    for(const auto physical:devices) {
        Candidate current;current.physical=physical;vkGetPhysicalDeviceProperties(physical,&current.properties);
        if(current.properties.apiVersion<VK_API_VERSION_1_2)continue;
        std::uint32_t extension_count{};checked(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extension_count,nullptr),"query device extensions");
        std::vector<VkExtensionProperties> extensions(extension_count);
        checked(vkEnumerateDeviceExtensionProperties(physical,nullptr,&extension_count,extensions.data()),"query device extensions");
        const auto has=[&](const char* name){return std::any_of(extensions.begin(),extensions.end(),[&](const auto& ext){return std::strcmp(ext.extensionName,name)==0;});};
        if(!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME))continue;
        const bool ray_extensions=has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)&&has(VK_KHR_RAY_QUERY_EXTENSION_NAME)&&has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        VkPhysicalDeviceRayQueryFeaturesKHR ray{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&features12;
        if(ray_extensions){features12.pNext=&acceleration;acceleration.pNext=&ray;}
        vkGetPhysicalDeviceFeatures2(physical,&features);current.features=features.features;
        const auto& limits=current.properties.limits;
        if(!features12.descriptorIndexing||!features12.bufferDeviceAddress||!features12.runtimeDescriptorArray||!features12.shaderSampledImageArrayNonUniformIndexing||!features12.descriptorBindingPartiallyBound||!features.features.shaderInt64||!features.features.shaderSampledImageArrayDynamicIndexing)continue;
        if(limits.maxPerStageDescriptorSamplers<19||limits.maxDescriptorSetSamplers<19||limits.maxPerStageDescriptorSampledImages<4||limits.maxDescriptorSetSampledImages<4||limits.maxPerStageDescriptorStorageBuffers<6||limits.maxDescriptorSetStorageBuffers<6||limits.maxPerStageDescriptorUniformBuffers<1||limits.maxPerStageResources<30)continue;
        std::uint32_t family_count{};vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&family_count,families.data());
        current.graphics=invalid_id;current.present=invalid_id;
        for(std::uint32_t i=0;i<family_count;++i) {
            if(!families[i].queueCount)continue;
            VkBool32 present{};checked(vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&present),"query queue presentation");
            const bool graphics=(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)!=0;
            if(graphics&&present){current.graphics=i;current.present=i;break;}
            if(graphics&&current.graphics==invalid_id)current.graphics=i;
            if(present&&current.present==invalid_id)current.present=i;
        }
        if(current.graphics==invalid_id||current.present==invalid_id)continue;
        VkSurfaceCapabilitiesKHR surface_caps{};checked(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&surface_caps),"query surface capabilities");
        if(!(surface_caps.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))continue;
        const auto formats=surface_formats(physical,surface);
        if(std::none_of(formats.begin(),formats.end(),[](const auto& f){return f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR&&(f.format==VK_FORMAT_UNDEFINED||f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM||f.format==VK_FORMAT_B8G8R8A8_SRGB||f.format==VK_FORMAT_R8G8B8A8_SRGB);}))continue;
        const auto modes=surface_modes(physical,surface);if(modes.empty())continue;
        constexpr auto color_flags=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if(!format_support(physical,VK_FORMAT_R16G16B16A16_SFLOAT,color_flags)||!format_support(physical,VK_FORMAT_R8G8B8A8_UNORM,color_flags))continue;
        for(const auto depth:{VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D32_SFLOAT_S8_UINT})if(format_support(physical,depth,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)){current.depth=depth;break;}
        if(current.depth==VK_FORMAT_UNDEFINED)continue;
        VkImageFormatProperties color_properties{},depth_properties{};
        if(vkGetPhysicalDeviceImageFormatProperties(physical,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,0,&color_properties)!=VK_SUCCESS||vkGetPhysicalDeviceImageFormatProperties(physical,current.depth,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,0,&depth_properties)!=VK_SUCCESS)continue;
        const auto sample_counts=color_properties.sampleCounts&depth_properties.sampleCounts&limits.framebufferColorSampleCounts&limits.framebufferDepthSampleCounts&limits.framebufferStencilSampleCounts;
        if(!(sample_counts&VK_SAMPLE_COUNT_1_BIT))continue;
        current.rays=ray_extensions&&acceleration.accelerationStructure&&ray.rayQuery&&(families[current.graphics].queueFlags&VK_QUEUE_COMPUTE_BIT)&&limits.maxPerStageDescriptorStorageBuffers>=8&&limits.maxDescriptorSetStorageBuffers>=8&&limits.maxPerStageResources>=33;
        if(current.rays) {
            VkPhysicalDeviceAccelerationStructurePropertiesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&as;vkGetPhysicalDeviceProperties2(physical,&properties);
            current.rays=as.maxPerStageDescriptorAccelerationStructures>=1&&as.maxDescriptorSetAccelerationStructures>=1&&as.maxGeometryCount&&as.maxPrimitiveCount&&as.maxInstanceCount;
        }
        auto& caps=current.caps;caps.available=true;caps.hardware_ray_tracing=current.rays;caps.ray_shadows=current.rays;caps.ray_reflections=current.rays;
        caps.bloom=true;caps.gamma=true;caps.hdr_tonemapping=true;caps.vsync=std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_FIFO_KHR)!=modes.end();
        caps.msaa_values.push_back(0);
        for(const int samples:{2,4,8,16})if(sample_counts&static_cast<VkSampleCountFlagBits>(samples))caps.msaa_values.push_back(samples);
        caps.max_msaa=caps.msaa_values.back();caps.msaa=caps.max_msaa>0;caps.anisotropy_values.push_back(1);
        if(features.features.samplerAnisotropy)for(const int value:{2,4,8,16})if(float(value)<=limits.maxSamplerAnisotropy)caps.anisotropy_values.push_back(value);
        caps.max_anisotropy=caps.anisotropy_values.back();caps.anisotropy=caps.anisotropy_values.size()>1;
        for(const auto mode:modes)switch(mode){case VK_PRESENT_MODE_FIFO_KHR:caps.present_modes.emplace_back("fifo");break;case VK_PRESENT_MODE_IMMEDIATE_KHR:caps.present_modes.emplace_back("immediate");break;case VK_PRESENT_MODE_MAILBOX_KHR:caps.present_modes.emplace_back("mailbox");break;case VK_PRESENT_MODE_FIFO_RELAXED_KHR:caps.present_modes.emplace_back("fifo_relaxed");break;default:break;}
        switch(current.properties.vendorID){case 0x1002:caps.vendor="AMD";break;case 0x10de:caps.vendor="NVIDIA";break;case 0x8086:caps.vendor="Intel";break;default:caps.vendor="PCI vendor "+std::to_string(current.properties.vendorID);break;}
        caps.renderer=current.properties.deviceName;const auto version=current.properties.apiVersion;
        caps.version="Vulkan "+std::to_string(VK_VERSION_MAJOR(version))+"."+std::to_string(VK_VERSION_MINOR(version))+"."+std::to_string(VK_VERSION_PATCH(version))+"; driver "+std::to_string(current.properties.driverVersion);
        // Query and construction deliberately use this one preference, independent of saved settings.
        const int score=(current.rays?1000:0)+(current.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?100:current.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU?50:0);
        if(score>best_score){best=std::move(current);best_score=score;}
    }
    if(!best.physical)throw std::runtime_error("No presentable Vulkan 1.2 device supports the native descriptor/BDA/float-target baseline (including 19 samplers)");
    return best;
}
}

VulkanCapabilities query_vulkan_capabilities(SDL_Window* window) {
    VkInstance instance{};VkSurfaceKHR surface{};VulkanCapabilities caps;
    try {
        instance=make_instance(window);
        if(!SDL_Vulkan_CreateSurface(window,instance,&surface))throw std::runtime_error(SDL_GetError());
        caps=select_device(instance,surface).caps;
    }catch(const std::exception& error){caps={};caps.error=error.what();}
    if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);
    if(instance)vkDestroyInstance(instance,nullptr);
    return caps;
}

VulkanRenderer::State::State(SDL_Window* win,AssetStore& store,MaterialLibrary& library,const Settings& config)
    :window(win),assets(store),materials(library),settings(config) {
    try {
        initialize_device();validate_settings(settings);
        samples=settings.msaa?static_cast<VkSampleCountFlagBits>(settings.msaa):VK_SAMPLE_COUNT_1_BIT;
        initialize_raster();initialize_quality();initialize_ray_tracing();
        int width{},height{};SDL_Vulkan_GetDrawableSize(window,&width,&height);resize(width,height);recreate_swapchain();resources_dirty=true;
    }catch(...) {
        if(device)vkDeviceWaitIdle(device);
        destroy_ray_tracing();destroy_raster();destroy_quality();destroy_device();throw;
    }
}
VulkanRenderer::State::~State() {
    // Device loss cannot prevent releasing host-side ownership.
    if(device)vkDeviceWaitIdle(device);
    destroy_ray_tracing();destroy_raster();destroy_quality();destroy_device();
}
void VulkanRenderer::State::initialize_device() {
    instance=make_instance(window);
    if(!SDL_Vulkan_CreateSurface(window,instance,&surface))throw std::runtime_error(SDL_GetError());
    auto selected=select_device(instance,surface);physical=selected.physical;properties=selected.properties;
    graphics_family=selected.graphics;present_family=selected.present;depth_format=selected.depth;
    capabilities=std::move(selected.caps);rt_supported=selected.rays;rt_enabled=settings.ray_tracing;
    validate_settings(settings);vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);
    const float priority=1;
    std::array<VkDeviceQueueCreateInfo,2> queues{};
    queues[0].sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;queues[0].queueFamilyIndex=graphics_family;queues[0].queueCount=1;queues[0].pQueuePriorities=&priority;
    queues[1]=queues[0];queues[1].queueFamilyIndex=present_family;
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceRayQueryFeaturesKHR rays{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.descriptorIndexing=VK_TRUE;features12.bufferDeviceAddress=VK_TRUE;features12.runtimeDescriptorArray=VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing=VK_TRUE;features12.descriptorBindingPartiallyBound=VK_TRUE;
    if(rt_enabled) {
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        features12.pNext=&acceleration;acceleration.accelerationStructure=VK_TRUE;acceleration.pNext=&rays;rays.rayQuery=VK_TRUE;
        VkPhysicalDeviceProperties2 queried{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};queried.pNext=&acceleration_properties;vkGetPhysicalDeviceProperties2(physical,&queried);
    }
    VkPhysicalDeviceFeatures enabled{};enabled.samplerAnisotropy=selected.features.samplerAnisotropy;enabled.shaderInt64=VK_TRUE;enabled.shaderSampledImageArrayDynamicIndexing=VK_TRUE;enabled.textureCompressionBC=selected.features.textureCompressionBC;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};info.pNext=&features12;info.pEnabledFeatures=&enabled;
    info.queueCreateInfoCount=graphics_family==present_family?1:2;info.pQueueCreateInfos=queues.data();info.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());info.ppEnabledExtensionNames=extensions.data();
    checked(vkCreateDevice(physical,&info,nullptr,&device),"create logical device");
    vkGetDeviceQueue(device,graphics_family,0,&graphics_queue);vkGetDeviceQueue(device,present_family,0,&present_queue);
    if(rt_enabled) {
        create_acceleration=reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device,"vkCreateAccelerationStructureKHR"));
        destroy_acceleration=reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device,"vkDestroyAccelerationStructureKHR"));
        acceleration_build_sizes=reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device,"vkGetAccelerationStructureBuildSizesKHR"));
        cmd_build_acceleration=reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device,"vkCmdBuildAccelerationStructuresKHR"));
        acceleration_address=reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device,"vkGetAccelerationStructureDeviceAddressKHR"));
        if(!create_acceleration||!destroy_acceleration||!acceleration_build_sizes||!cmd_build_acceleration||!acceleration_address)throw std::runtime_error("Vulkan ray-query device lacks required AS entrypoints");
    }
    for(auto& frame:frames) {
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;checked(vkCreateFence(device,&fence,nullptr,&frame.fence),"create frame fence");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};checked(vkCreateSemaphore(device,&semaphore,nullptr,&frame.acquired),"create acquire semaphore");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=graphics_family;
        checked(vkCreateCommandPool(device,&pool,nullptr,&frame.pool),"create frame command pool");
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commands.commandPool=frame.pool;commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commands.commandBufferCount=1;
        checked(vkAllocateCommandBuffers(device,&commands,&frame.command),"allocate frame command");
        // Pools are slot-owned and reset only after that slot's submission fence.
        const auto image_limit=std::min(properties.limits.maxDescriptorSetSampledImages,properties.limits.maxPerStageDescriptorSampledImages);
        const std::array<VkDescriptorPoolSize,6> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,256},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2048},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,std::max(4096u,image_limit*2)},{VK_DESCRIPTOR_TYPE_SAMPLER,4864},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,768},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,256}}};
        VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descriptors.maxSets=256;
        descriptors.poolSizeCount=rt_enabled?6:5;descriptors.pPoolSizes=sizes.data();checked(vkCreateDescriptorPool(device,&descriptors,nullptr,&frame.descriptors),"create frame descriptor pool");
    }
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=graphics_family;
    checked(vkCreateCommandPool(device,&pool,nullptr,&immediate_pool),"create upload command pool");
    VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commands.commandPool=immediate_pool;commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commands.commandBufferCount=1;
    checked(vkAllocateCommandBuffers(device,&commands,&immediate_command),"allocate upload command");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;checked(vkCreateFence(device,&fence,nullptr,&immediate_fence),"create upload fence");
}
void VulkanRenderer::State::destroy_device() noexcept {
    if(device) {
        destroy_swapchain();
        for(auto& frame:frames) {
            for(auto& block:frame.uploads)destroy_buffer(block.buffer);
            for(auto& buffer:frame.retired_buffers)destroy_buffer(buffer);
            for(auto& image:frame.retired_images)destroy_image(image);
            for(auto as:frame.retired_acceleration)if(as&&destroy_acceleration)destroy_acceleration(device,as,nullptr);
            for(auto pipeline:frame.retired_pipelines)if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
            if(frame.descriptors)vkDestroyDescriptorPool(device,frame.descriptors,nullptr);
            if(frame.pool)vkDestroyCommandPool(device,frame.pool,nullptr);
            if(frame.acquired)vkDestroySemaphore(device,frame.acquired,nullptr);
            if(frame.fence)vkDestroyFence(device,frame.fence,nullptr);
            frame={};
        }
        if(immediate_pool)vkDestroyCommandPool(device,immediate_pool,nullptr);
        if(immediate_fence)vkDestroyFence(device,immediate_fence,nullptr);
        vkDestroyDevice(device,nullptr);device={};
    }
    if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);
    if(instance)vkDestroyInstance(instance,nullptr);
    surface={};instance={};
}
void VulkanRenderer::State::validate_settings(const Settings& config) const {
    const auto contains=[](const auto& values,int value){return std::find(values.begin(),values.end(),value)!=values.end();};
    if(!contains(capabilities.msaa_values,config.msaa))throw std::runtime_error("Requested Vulkan MSAA count is not supported by this device's float color/depth formats");
    if(!contains(capabilities.anisotropy_values,config.anisotropy))throw std::runtime_error("Requested Vulkan anisotropy is not supported by this device");
    if(config.ray_tracing&&!rt_supported)throw std::runtime_error("Requested Vulkan hardware ray queries are unavailable on the selected presentable device");
    if(!config.vsync&&std::none_of(capabilities.present_modes.begin(),capabilities.present_modes.end(),[](const auto& mode){return mode=="immediate"||mode=="mailbox"||mode=="fifo_relaxed";}))throw std::runtime_error("Vulkan VSync-off requested but no non-FIFO present mode is available");
    const auto finite_range=[](float value,float low,float high){return std::isfinite(value)&&value>=low&&value<=high;};
    if(!finite_range(config.gamma,0.25f,4.0f)||!finite_range(config.exposure_ev,-8,8)||!finite_range(config.virtual_light_azimuth,0,360)||!finite_range(config.virtual_light_elevation,-90,90)||!finite_range(config.virtual_light_red,0,1)||!finite_range(config.virtual_light_green,0,1)||!finite_range(config.virtual_light_blue,0,1)||!finite_range(config.virtual_light_strength,0,16))throw std::runtime_error("Vulkan color/light settings are nonfinite or out of range");
}
void VulkanRenderer::State::wait_idle() {
    if(!device)return;
    checked(vkDeviceWaitIdle(device),"wait idle");
    for(auto& frame:frames)if(frame.submitted){completed_serial=std::max(completed_serial,frame.serial);frame.submitted=false;}
}
void VulkanRenderer::State::destroy_swapchain() noexcept {
    if(!device)return;
    for(auto& image:swap_images) {
        if(image.framebuffer)vkDestroyFramebuffer(device,image.framebuffer,nullptr);
        if(image.view)vkDestroyImageView(device,image.view,nullptr);
        if(image.complete)vkDestroySemaphore(device,image.complete,nullptr);
    }
    swap_images.clear();if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain={};extent={};acquired=false;frame_rendered=false;
}
void VulkanRenderer::State::recreate_swapchain() {
    if(acquired)throw std::runtime_error("Vulkan cannot recreate an acquired frame before presentation");
    int width{},height{};SDL_Vulkan_GetDrawableSize(window,&width,&height);
    if(width<=0||height<=0){requested_extent={};recreate_pending=true;return;}
    requested_extent={static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height)};
    VkSurfaceCapabilitiesKHR caps{};checked(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"query resized surface");
    auto target=caps.currentExtent;
    if(target.width==std::numeric_limits<std::uint32_t>::max())target={std::clamp(requested_extent.width,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(requested_extent.height,caps.minImageExtent.height,caps.maxImageExtent.height)};
    if(!target.width||!target.height){recreate_pending=true;return;}
    const auto format=choose_format(physical,surface);const auto modes=surface_modes(physical,surface);
    VkPresentModeKHR mode=VK_PRESENT_MODE_FIFO_KHR;
    if(!settings.vsync) {
        bool found=false;
        for(const auto wanted:{VK_PRESENT_MODE_MAILBOX_KHR,VK_PRESENT_MODE_IMMEDIATE_KHR,VK_PRESENT_MODE_FIFO_RELAXED_KHR})if(std::find(modes.begin(),modes.end(),wanted)!=modes.end()){mode=wanted;found=true;break;}
        if(!found)throw std::runtime_error("Vulkan VSync-off has no available presentation mode");
    }else if(std::find(modes.begin(),modes.end(),mode)==modes.end())throw std::runtime_error("Vulkan FIFO presentation is unavailable");
    wait_idle();destroy_targets();destroy_swapchain();
    std::uint32_t image_count=caps.minImageCount;
    if(image_count<std::numeric_limits<std::uint32_t>::max())++image_count;
    if(caps.maxImageCount)image_count=std::min(image_count,caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR composite=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for(const auto flag:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})if(caps.supportedCompositeAlpha&flag){composite=flag;break;}
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};info.surface=surface;info.minImageCount=image_count;info.imageFormat=format.format;info.imageColorSpace=format.colorSpace;info.imageExtent=target;
    info.imageArrayLayers=1;info.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;info.preTransform=caps.currentTransform;info.compositeAlpha=composite;info.presentMode=mode;info.clipped=VK_TRUE;
    checked(vkCreateSwapchainKHR(device,&info,nullptr,&swapchain),"create swapchain");swap_format=format.format;color_space=format.colorSpace;present_mode=mode;extent=target;
    std::uint32_t count{};checked(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"query swapchain images");std::vector<VkImage> handles(count);
    checked(vkGetSwapchainImagesKHR(device,swapchain,&count,handles.data()),"query swapchain images");swap_images.resize(count);
    for(std::size_t i=0;i<count;++i) {
        auto& image=swap_images[i];image.handle=handles[i];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image.handle;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=swap_format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        checked(vkCreateImageView(device,&view,nullptr,&image.view),"create swapchain view");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};checked(vkCreateSemaphore(device,&semaphore,nullptr,&image.complete),"create image presentation semaphore");
    }
    create_targets(extent,samples);recreate_pending=false;
}
void VulkanRenderer::State::resize(int width,int height) {
    const VkExtent2D next{static_cast<std::uint32_t>(std::max(width,0)),static_cast<std::uint32_t>(std::max(height,0))};
    if(next.width!=requested_extent.width||next.height!=requested_extent.height){requested_extent=next;recreate_pending=true;}
}
void VulkanRenderer::State::apply_settings(const Settings& config) {
    validate_settings(config);
    if(config.ray_tracing!=rt_enabled)throw std::runtime_error("Changing Vulkan hardware ray-query device enablement requires a new launch");
    const bool targets=config.msaa!=settings.msaa||config.vsync!=settings.vsync;
    const bool resources=config.anisotropy!=settings.anisotropy;
    const bool history_changed=config.hdr_tonemapping!=settings.hdr_tonemapping;
    if(acquired)throw std::runtime_error("Vulkan settings must be applied before frame acquisition");
    if(resources)wait_idle();
    settings=config;samples=config.msaa?static_cast<VkSampleCountFlagBits>(config.msaa):VK_SAMPLE_COUNT_1_BIT;
    if(resources)refresh_samplers();
    if(targets)recreate_pending=true;
    if(history_changed&&!targets)reset_history();
}
void VulkanRenderer::State::apply_original_options(const OriginalGraphicsOptions& options) {
    if(options==original)return;
    if(acquired)throw std::runtime_error("Original graphics options must be applied before Vulkan frame acquisition");
    // Divisors/filter interpretation remains the original shared policy, not a new Vk-only enum.
    if(options.texture_divisor<1||options.lightmap_divisor<1||options.reflection_divisor<1||options.motion_blur_size<1||options.motion_blur_frames<2||options.motion_blur_frames>10)
        throw std::runtime_error("Invalid original graphics options");
    const bool resources=original.texture_divisor!=options.texture_divisor||original.lightmap_divisor!=options.lightmap_divisor||
        original.reflection_divisor!=options.reflection_divisor||original.texture_compression!=options.texture_compression||
        original.lightmap_compression!=options.lightmap_compression||original.reflection_compression!=options.reflection_compression||
        (effective_texture_filter(original.reflection_filter,true)>=2)!=(effective_texture_filter(options.reflection_filter,true)>=2);
    const bool sampler_change=original.texture_filter!=options.texture_filter||original.lightmap_filter!=options.lightmap_filter||original.reflection_filter!=options.reflection_filter;
    const bool history_change=original.motion_blur!=options.motion_blur||original.motion_blur_size!=options.motion_blur_size||original.motion_blur_frames!=options.motion_blur_frames;
    if(resources||sampler_change||history_change)wait_idle();
    if(resources){
        // Resource replacement is not a scene transition: retain the committed BSP identity/path.
        const auto* committed_level=level;const auto committed_generation=generation;
        destroy_raster();level=committed_level;generation=committed_generation;
    }
    original=options;
    if(resources){initialize_raster();resources_dirty=true;reflections_pending=true;}
    else if(sampler_change)refresh_samplers();
    if(history_change)reset_history();
}
void VulkanRenderer::State::service_window() {
    const auto now=SDL_GetTicks();
    if(now-window_service_tick<50)return;
    window_service_tick=now;
    // Pump native window/WM protocol only. The main loop retains Quit/input ownership;
    // never dispatch game actions or loading presents from an upload/recording batch.
    SDL_PumpEvents();
}
void VulkanRenderer::State::retire_frame(FrameSlot& frame) {
    if(frame.recording)throw std::runtime_error("Vulkan cannot retire a recording frame");
    if(frame.submitted){checked(vkWaitForFences(device,1,&frame.fence,VK_TRUE,UINT64_MAX),"wait reusable frame");completed_serial=std::max(completed_serial,frame.serial);frame.submitted=false;}
    for(auto as:frame.retired_acceleration)if(as&&destroy_acceleration)destroy_acceleration(device,as,nullptr);
    for(auto& buffer:frame.retired_buffers)destroy_buffer(buffer);
    for(auto& image:frame.retired_images)destroy_image(image);
    for(auto pipeline:frame.retired_pipelines)if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
    frame.retired_acceleration.clear();frame.retired_buffers.clear();frame.retired_images.clear();frame.retired_pipelines.clear();
    for(auto& upload:frame.uploads)upload.used=0;
    checked(vkResetCommandPool(device,frame.pool,0),"reset frame command pool");
    checked(vkResetDescriptorPool(device,frame.descriptors,0),"reset frame descriptor pool");
}
bool VulkanRenderer::State::begin_frame() {
    if(acquired)throw std::runtime_error("Vulkan previous frame has not been presented");
    if(immediate_recording)throw std::runtime_error("Vulkan loading callback crossed an unfinished upload batch");
    presented=false;frame_rendered=false;
    int width{},height{};SDL_Vulkan_GetDrawableSize(window,&width,&height);resize(width,height);
    if(width<=0||height<=0)return false;
    if(recreate_pending||!swapchain)recreate_swapchain();
    if(!swapchain||!extent.width||!extent.height)return false;
    auto& frame=frames[frame_index];retire_frame(frame);
    const auto result=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,frame.acquired,VK_NULL_HANDLE,&image_index);
    if(result==VK_ERROR_OUT_OF_DATE_KHR){recreate_pending=true;return false;}
    if(result!=VK_SUBOPTIMAL_KHR)checked(result,"acquire image");else recreate_pending=true;
    auto& image=swap_images.at(image_index);
    if(image.in_flight&&image.in_flight!=frame.fence)checked(vkWaitForFences(device,1,&image.in_flight,VK_TRUE,UINT64_MAX),"wait swapchain image");
    acquired=true;return true;
}
void VulkanRenderer::State::submit_frame() {
    auto& frame=frames[frame_index];
    if(!acquired||!frame.recording)throw std::runtime_error("Vulkan frame submission has no acquired recording");
    checked(vkEndCommandBuffer(frame.command),"end frame command");frame.recording=false;
    const VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
    const auto complete=swap_images.at(image_index).complete;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&frame.acquired;submit.pWaitDstStageMask=&wait_stage;submit.commandBufferCount=1;submit.pCommandBuffers=&frame.command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&complete;
    // Never reset on rejected/out-of-date acquisition: only a real submit consumes the fence.
    checked(vkResetFences(device,1,&frame.fence),"reset submitting frame fence");
    checked(vkQueueSubmit(graphics_queue,1,&submit,frame.fence),"submit frame");
    frame.submitted=true;frame.serial=++submission_serial;swap_images[image_index].in_flight=frame.fence;frame_rendered=true;
}
bool VulkanRenderer::State::present() {
    if(!acquired||!frame_rendered)return false;
    const auto complete=swap_images[image_index].complete;
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};info.waitSemaphoreCount=1;info.pWaitSemaphores=&complete;info.swapchainCount=1;info.pSwapchains=&swapchain;info.pImageIndices=&image_index;
    const auto result=vkQueuePresentKHR(present_queue,&info);
    acquired=false;frame_rendered=false;frame_index=(frame_index+1)%static_cast<std::uint32_t>(frames.size());
    if(result==VK_ERROR_OUT_OF_DATE_KHR){recreate_pending=true;presented=false;return false;}
    if(result==VK_SUBOPTIMAL_KHR)recreate_pending=true;else checked(result,"present image");
    presented=true;return true;
}
VkCommandBuffer VulkanRenderer::State::begin_commands() {
    if(immediate_recording||frames[frame_index].recording)throw std::runtime_error("Vulkan nested command/upload batch is forbidden");
    service_window();
    auto& frame=frames[frame_index];
    // Captures/uploads share owned ranges, not a recording render pass or borrowed decoder storage.
    if(frame.submitted){checked(vkWaitForFences(device,1,&frame.fence,VK_TRUE,UINT64_MAX),"wait upload slot");completed_serial=std::max(completed_serial,frame.serial);frame.submitted=false;}
    checked(vkWaitForFences(device,1,&immediate_fence,VK_TRUE,UINT64_MAX),"wait upload command");
    checked(vkResetCommandPool(device,immediate_pool,0),"reset upload command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(vkBeginCommandBuffer(immediate_command,&begin),"begin upload command");immediate_recording=true;return immediate_command;
}
void VulkanRenderer::State::finish_commands(VkCommandBuffer command,bool wait) {
    if(!immediate_recording||command!=immediate_command)throw std::runtime_error("Vulkan command batch ownership mismatch");
    checked(vkEndCommandBuffer(command),"end upload command");immediate_recording=false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    checked(vkResetFences(device,1,&immediate_fence),"reset upload fence");checked(vkQueueSubmit(graphics_queue,1,&submit,immediate_fence),"submit upload command");
    // All non-frame batches are synchronous: callbacks may mutate/free their source immediately.
    (void)wait;checked(vkWaitForFences(device,1,&immediate_fence,VK_TRUE,UINT64_MAX),"finish upload command");
}
std::uint32_t VulkanRenderer::State::memory_type(std::uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred) const {
    std::uint32_t fallback=invalid_id;
    for(std::uint32_t i=0;i<memory_properties.memoryTypeCount;++i)if((bits&(1u<<i))&&(memory_properties.memoryTypes[i].propertyFlags&required)==required) {
        if((memory_properties.memoryTypes[i].propertyFlags&preferred)==preferred)return i;
        if(fallback==invalid_id)fallback=i;
    }
    if(fallback!=invalid_id)return fallback;
    throw std::runtime_error("Vulkan device has no compatible memory type");
}
Buffer VulkanRenderer::State::create_buffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags flags,bool address) {
    if(!size)throw std::runtime_error("Vulkan zero-size buffer allocation");
    Buffer result;result.size=size;
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=size;info.usage=usage|(address?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0);info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateBuffer(device,&info,nullptr,&result.handle),"create buffer");VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,result.handle,&requirements);result.allocation_size=requirements.size;
        const auto type=memory_type(requirements.memoryTypeBits,flags,(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)?VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:0);
        result.coherent=(memory_properties.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        VkMemoryAllocateFlagsInfo allocation_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};allocation_flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;if(address)allocation.pNext=&allocation_flags;
        checked(vkAllocateMemory(device,&allocation,nullptr,&result.memory),"allocate buffer memory");checked(vkBindBufferMemory(device,result.handle,result.memory,0),"bind buffer memory");
        if(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)checked(vkMapMemory(device,result.memory,0,VK_WHOLE_SIZE,0,&result.mapped),"map buffer memory");
        if(address){VkBufferDeviceAddressInfo query{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};query.buffer=result.handle;result.address=vkGetBufferDeviceAddress(device,&query);if(!result.address)throw std::runtime_error("Vulkan buffer device address is zero");}
        return result;
    }catch(...){destroy_buffer(result);throw;}
}
void VulkanRenderer::State::destroy_buffer(Buffer& buffer) noexcept {
    if(!device)return;
    if(buffer.mapped)vkUnmapMemory(device,buffer.memory);
    if(buffer.handle)vkDestroyBuffer(device,buffer.handle,nullptr);
    if(buffer.memory)vkFreeMemory(device,buffer.memory,nullptr);
    buffer={};
}
BufferRange VulkanRenderer::State::upload_bytes(std::span<const std::byte> bytes,VkDeviceSize alignment,VkBufferUsageFlags usage) {
    if(bytes.empty())throw std::runtime_error("Vulkan empty upload range");
    auto& frame=frames[frame_index];
    // A synchronous loading present may advance the slot during preparation.
    // Image staging writes precede begin_commands(), so acquire ownership here.
    // Never reset ranges/descriptors belonging to an acquired or recording batch.
    if(frame.submitted&&!acquired&&!immediate_recording)retire_frame(frame);
    if(frame.submitted)throw std::runtime_error("Vulkan upload attempts to overwrite an in-flight frame slot");
    if(usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)alignment=std::max(alignment,properties.limits.minUniformBufferOffsetAlignment);
    if(usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)alignment=std::max(alignment,properties.limits.minStorageBufferOffsetAlignment);
    alignment=std::max<VkDeviceSize>(alignment,16);
    const auto size=static_cast<VkDeviceSize>(bytes.size());
    for(auto& block:frame.uploads) {
        const auto offset=aligned(block.used,alignment);
        if(offset>block.buffer.size||size>block.buffer.size-offset)continue;
        std::memcpy(static_cast<std::byte*>(block.buffer.mapped)+offset,bytes.data(),bytes.size());flush_buffer(block.buffer,offset,size);block.used=offset+size;
        return {block.buffer.handle,offset,size,block.buffer.address+offset};
    }
    constexpr VkDeviceSize minimum=4*1024*1024;
    VkDeviceSize capacity=std::max(minimum,aligned(size,alignment));
    if(!frame.uploads.empty()&&frame.uploads.back().buffer.size<=std::numeric_limits<VkDeviceSize>::max()/2)capacity=std::max(capacity,frame.uploads.back().buffer.size*2);
    VkBufferUsageFlags all=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if(rt_enabled)all|=VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    UploadBlock block;block.buffer=create_buffer(capacity,all|usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,true);
    try{frame.uploads.push_back(block);}catch(...){destroy_buffer(block.buffer);throw;}
    auto& owned=frame.uploads.back();std::memcpy(owned.buffer.mapped,bytes.data(),bytes.size());flush_buffer(owned.buffer,0,size);owned.used=size;
    return {owned.buffer.handle,0,size,owned.buffer.address};
}
void VulkanRenderer::State::flush_buffer(const Buffer& buffer,VkDeviceSize offset,VkDeviceSize size) {
    if(offset>buffer.size||size>buffer.size-offset)throw std::runtime_error("Vulkan mapped flush exceeds buffer range");
    if(buffer.coherent||!size)return;
    const auto atom=properties.limits.nonCoherentAtomSize;const auto first=offset-offset%atom;
    const auto end=std::min(aligned(offset+size,atom),buffer.allocation_size);
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=buffer.memory;range.offset=first;range.size=end-first;
    checked(vkFlushMappedMemoryRanges(device,1,&range),"flush noncoherent upload");
}
Image VulkanRenderer::State::create_image(VkExtent2D size,VkFormat format,VkImageUsageFlags usage,VkSampleCountFlagBits count,std::uint32_t layers,std::uint32_t mips,bool cube) {
    if(!size.width||!size.height||!layers||!mips||(cube&&(layers!=6||size.width!=size.height)))throw std::runtime_error("Invalid Vulkan image extent/layers/mips");
    Image result;result.format=format;result.extent=size;result.layers=layers;result.mip_levels=mips;result.samples=count;
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;info.imageType=VK_IMAGE_TYPE_2D;info.format=format;info.extent={size.width,size.height,1};info.mipLevels=mips;info.arrayLayers=layers;info.samples=count;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateImage(device,&info,nullptr,&result.handle),"create image");VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,result.handle,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device,&allocation,nullptr,&result.memory),"allocate image memory");checked(vkBindImageMemory(device,result.handle,result.memory,0),"bind image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=result.handle;view.format=format;view.viewType=cube?VK_IMAGE_VIEW_TYPE_CUBE:layers>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
        VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT;
        if(format==VK_FORMAT_D24_UNORM_S8_UINT||format==VK_FORMAT_D32_SFLOAT_S8_UINT)aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
        else if(format==VK_FORMAT_D32_SFLOAT||format==VK_FORMAT_D16_UNORM)aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
        view.subresourceRange={aspect,0,mips,0,layers};checked(vkCreateImageView(device,&view,nullptr,&result.view),"create image view");return result;
    }catch(...){destroy_image(result);throw;}
}
void VulkanRenderer::State::destroy_image(Image& image) noexcept {
    if(!device)return;
    if(image.view)vkDestroyImageView(device,image.view,nullptr);
    if(image.handle)vkDestroyImage(device,image.handle,nullptr);
    if(image.memory)vkFreeMemory(device,image.memory,nullptr);
    image={};
}
void VulkanRenderer::State::transition_image(VkCommandBuffer command,Image& image,VkImageLayout layout,VkPipelineStageFlags source_stage,VkAccessFlags source_access,VkPipelineStageFlags destination_stage,VkAccessFlags destination_access) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=image.layout;barrier.newLayout=layout;barrier.srcAccessMask=source_access;barrier.dstAccessMask=destination_access;barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image.handle;
    VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    if(image.format==VK_FORMAT_D24_UNORM_S8_UINT||image.format==VK_FORMAT_D32_SFLOAT_S8_UINT)aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    else if(image.format==VK_FORMAT_D32_SFLOAT||image.format==VK_FORMAT_D16_UNORM)aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange={aspect,0,image.mip_levels,0,image.layers};
    if(image.layout==VK_IMAGE_LAYOUT_UNDEFINED){barrier.srcAccessMask=0;source_stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;}
    vkCmdPipelineBarrier(command,source_stage,destination_stage,0,0,nullptr,0,nullptr,1,&barrier);image.layout=layout;
}
void VulkanRenderer::State::readback(std::span<std::uint8_t> rgba,std::size_t row_pitch) {
    if(!acquired||!frame_rendered)throw std::runtime_error("Vulkan SDR readback requires a rendered frame before present");
    const auto width=static_cast<std::size_t>(extent.width),height=static_cast<std::size_t>(extent.height);
    if(width>std::numeric_limits<std::size_t>::max()/4)throw std::runtime_error("Vulkan readback width overflow");
    const auto row=width*4;
    if(!height||row_pitch<row||(height-1&&row_pitch>(std::numeric_limits<std::size_t>::max()-row)/(height-1)))throw std::runtime_error("Vulkan readback row pitch overflow or undersized pitch");
    const auto required=(height-1)*row_pitch+row;
    if(rgba.size()<required||height>std::numeric_limits<std::size_t>::max()/row)throw std::runtime_error("Vulkan readback output span is too small");
    auto& image=output_target.color;
    if(image.format!=VK_FORMAT_R8G8B8A8_UNORM||image.extent.width!=extent.width||image.extent.height!=extent.height)throw std::runtime_error("Vulkan final SDR target is not RGBA8 drawable size");
    Buffer staging=create_buffer(row*height,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    try {
        const auto command=begin_commands();const auto previous=image.layout;
        transition_image(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};vkCmdCopyImageToBuffer(command,image.handle,image.layout,staging.handle,1,&copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=staging.handle;host.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        transition_image(command,image,previous,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);finish_commands(command,true);
        if(!staging.coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=staging.memory;range.size=VK_WHOLE_SIZE;checked(vkInvalidateMappedMemoryRanges(device,1,&range),"invalidate SDR readback");}
        for(std::size_t y=0;y<height;++y)std::memcpy(rgba.data()+y*row_pitch,static_cast<const std::uint8_t*>(staging.mapped)+y*row,row);
    }catch(...){destroy_buffer(staging);throw;}
    destroy_buffer(staging);
}
void VulkanRenderer::State::render(const RenderScene& scene,std::span<const InterfaceQuad> quads) {
    if(acquired||immediate_recording)throw std::runtime_error("Vulkan render called across an unfinished frame/upload");
    capture_mode=CaptureMode::ordinary;
    // Internal preparation owns the same retired upload/descriptor slot as explicit prepare().
    retire_frame(frames[frame_index]);
    prepare_resources(scene,quads);resources_dirty=false;
    // Republish replacement cube images synchronously, before acquiring/recording ordinary sampling.
    if(scene.level&&original.reflections&&reflections_pending&&!reflection_path.empty())prepare_cube_resources(scene,reflection_path);
    if(!begin_frame())return;
    prepare_frame_geometry(scene);
    if(rt_enabled){prepare_acceleration(scene);update_ray_parameters(scene);}
    auto& frame=frames[frame_index];VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(vkBeginCommandBuffer(frame.command,&begin),"begin frame command");frame.recording=true;
    if(rt_enabled)record_acceleration(frame.command);
    upload_material_tables(frame.command);
    begin_target(frame.command,scene_target,scene.clear_color);
    record_world_entities(frame.command,scene,CaptureMode::ordinary);record_history_consume(frame.command,scene);
    if(!settings.hdr_tonemapping)record_interface(frame.command,quads);
    end_target(frame.command,scene_target);record_post(frame.command);
    if(settings.hdr_tonemapping)record_interface(frame.command,quads);
    end_target(frame.command,output_target);record_output_to_swapchain(frame.command);submit_frame();
}
void VulkanRenderer::State::render_loading(std::span<const InterfaceQuad> quads) {
    if(acquired||immediate_recording)throw std::runtime_error("Vulkan loading render called across an unfinished frame/upload");
    capture_mode=CaptureMode::loading;
    // Game has already destroyed the outgoing Level at precommit progress callbacks.
    // Retire the upload slot, admit only UI, and leave world identity/dirty state untouched.
    retire_frame(frames[frame_index]);prepare_interface_resources(quads);
    if(!begin_frame())return;
    prepare_frame_tables();
    auto& frame=frames[frame_index];VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(vkBeginCommandBuffer(frame.command,&begin),"begin loading command");frame.recording=true;upload_material_tables(frame.command);
    begin_target(frame.command,scene_target,{0,0,0,1});
    if(!settings.hdr_tonemapping)record_interface(frame.command,quads);
    end_target(frame.command,scene_target);record_post(frame.command);
    if(settings.hdr_tonemapping)record_interface(frame.command,quads);
    end_target(frame.command,output_target);record_output_to_swapchain(frame.command);submit_frame();
}

VulkanRenderer::VulkanRenderer(SDL_Window* window,AssetStore& assets,MaterialLibrary& materials,const Settings& settings)
    :state_(std::make_unique<State>(window,assets,materials,settings)){}
VulkanRenderer::~VulkanRenderer()=default;
void VulkanRenderer::resize(int width,int height){state_->resize(width,height);}
void VulkanRenderer::apply_settings(const Settings& settings){state_->apply_settings(settings);}
void VulkanRenderer::apply_original_options(const OriginalGraphicsOptions& options){state_->apply_original_options(options);}
void VulkanRenderer::prepare(const RenderScene& scene,std::span<const InterfaceQuad> quads) {
    auto& state=*state_;if(state.acquired||state.immediate_recording)throw std::runtime_error("Vulkan prepare called across an unfinished frame/upload");
    state.retire_frame(state.frames[state.frame_index]);state.prepare_resources(scene,quads);state.resources_dirty=false;
    if(scene.level&&state.original.reflections&&state.reflections_pending&&!state.reflection_path.empty())state.prepare_cube_resources(scene,state.reflection_path);
}
void VulkanRenderer::prepare_level(const Level& level,std::uint64_t generation,LevelProgress progress,void* context) {
    auto& state=*state_;if(state.acquired||state.immediate_recording)throw std::runtime_error("Vulkan level callback crossed an unfinished frame/upload");
    state.wait_idle();state.retire_frame(state.frames[state.frame_index]);state.prepare_level_resources(level,generation,progress,context);
}
void VulkanRenderer::prepare_reflections(const RenderScene& scene,std::string_view path) {
    auto& state=*state_;if(state.acquired||state.immediate_recording)throw std::runtime_error("Vulkan reflection preparation crossed an unfinished frame/upload");
    state.wait_idle();state.retire_frame(state.frames[state.frame_index]);state.prepare_cube_resources(scene,path);
}
void VulkanRenderer::render(const RenderScene& scene,std::span<const InterfaceQuad> quads){state_->render(scene,quads);}
void VulkanRenderer::render_loading(std::span<const InterfaceQuad> quads){state_->render_loading(quads);}
void VulkanRenderer::readback(std::span<std::uint8_t> rgba,std::size_t pitch){state_->readback(rgba,pitch);}
bool VulkanRenderer::present(){return state_->present();}
bool VulkanRenderer::frame_ready() const noexcept{return state_->acquired&&state_->frame_rendered;}
void VulkanRenderer::after_present(const RenderScene& scene,std::span<const InterfaceQuad> quads) {
    auto& state=*state_;if(!state.presented)return;
    if(state.capture_mode==CaptureMode::ordinary)state.record_history_after_present(scene,quads);
    state.presented=false;
}
} // namespace pusu
