#include "renderer_vulkan_private.hpp"
#include "scene_math.hpp"
#include "resources.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace pusu {
namespace {
using namespace vk_detail;
void ray_check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" failed (VkResult "+std::to_string(result)+")");
}
bool finite(Vec3 p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
bool finite_attributes(const RenderVertex& v) {
    return finite(v.normal)&&std::isfinite(v.uv.x)&&std::isfinite(v.uv.y)&&
           std::isfinite(v.light_uv.x)&&std::isfinite(v.light_uv.y)&&
           std::all_of(v.color.begin(),v.color.end(),[](float f){return std::isfinite(f);});
}
double determinant(const Matrix& m) {
    return double(m[0])*(double(m[5])*m[10]-double(m[9])*m[6])-
           double(m[4])*(double(m[1])*m[10]-double(m[9])*m[2])+
           double(m[8])*(double(m[1])*m[6]-double(m[5])*m[2]);
}
bool valid_transform(const Matrix& m) {
    if(!std::all_of(m.begin(),m.end(),[](float f){return std::isfinite(f);})||
       m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)return false;
    const double d=determinant(m);
    if(!std::isfinite(d)||d==0)return false;
    // Reject transforms whose inverse cannot be represented by the AS float ABI.
    for(unsigned row=0;row<3;++row) {
        double translation=0;
        for(unsigned col=0;col<3;++col) {
            const unsigned r0=(col+1)%3,r1=(col+2)%3,c0=(row+1)%3,c1=(row+2)%3;
            const double value=(double(m[c0*4+r0])*m[c1*4+r1]-double(m[c1*4+r0])*m[c0*4+r1])/d;
            if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())return false;
            translation+=value*m[12+col];
        }
        if(!std::isfinite(translation)||std::abs(translation)>std::numeric_limits<float>::max())return false;
    }
    return true;
}
bool degenerate(Vec3 a,Vec3 b,Vec3 c) {
    const double x=double(b.x)-a.x,y=double(b.y)-a.y,z=double(b.z)-a.z;
    const double u=double(c.x)-a.x,v=double(c.y)-a.y,w=double(c.z)-a.z;
    const double nx=y*w-z*v,ny=z*u-x*w,nz=x*v-y*u;
    return nx==0&&ny==0&&nz==0;
}
void hash_word(std::uint64_t& h,std::uint32_t word) { h=(h^word)*1099511628211ull; }
VkDeviceAddress scratch_address(const Buffer& buffer,VkDeviceSize alignment) {
    return (buffer.address+alignment-1)&~VkDeviceAddress(alignment-1);
}
VkAccelerationStructureGeometryKHR triangle_geometry(const Acceleration& acceleration) {
    VkAccelerationStructureGeometryKHR out{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    out.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    // Deliberately non-opaque: every candidate runs current original alpha/cull rules.
    auto& triangle=out.geometry.triangles;
    triangle.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangle.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;
    triangle.vertexData.deviceAddress=acceleration.vertex_range.address;
    triangle.vertexStride=sizeof(RenderVertex);
    triangle.maxVertex=acceleration.vertex_count-1;
    triangle.indexType=VK_INDEX_TYPE_UINT32;
    triangle.indexData.deviceAddress=acceleration.indices.address;
    return out;
}
VkAccelerationStructureBuildGeometryInfoKHR build_info(VkAccelerationStructureTypeKHR type,
                                                       const Acceleration& acceleration,
                                                       const VkAccelerationStructureGeometryKHR& geometry) {
    VkAccelerationStructureBuildGeometryInfoKHR out{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    out.type=type;
    out.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR|VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    out.mode=acceleration.update_pending?VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR:VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    out.srcAccelerationStructure=acceleration.update_pending?acceleration.handle:VK_NULL_HANDLE;
    out.dstAccelerationStructure=acceleration.handle;
    out.geometryCount=1;out.pGeometries=&geometry;
    return out;
}
}

void VulkanRenderer::State::initialize_ray_tracing() {
    if(!settings.ray_tracing)return;
    if(!rt_supported||!rt_enabled)throw std::runtime_error("Vulkan hardware accelerationStructure/rayQuery/bufferDeviceAddress unavailable or not enabled");
    create_acceleration=reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device,"vkCreateAccelerationStructureKHR"));
    destroy_acceleration=reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device,"vkDestroyAccelerationStructureKHR"));
    acceleration_build_sizes=reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device,"vkGetAccelerationStructureBuildSizesKHR"));
    cmd_build_acceleration=reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device,"vkCmdBuildAccelerationStructuresKHR"));
    acceleration_address=reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device,"vkGetAccelerationStructureDeviceAddressKHR"));
    if(!create_acceleration||!destroy_acceleration||!acceleration_build_sizes||!cmd_build_acceleration||!acceleration_address)
        throw std::runtime_error("Vulkan enabled RT device lacks required acceleration-structure entrypoints");
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32_SFLOAT,&format);
    if(!(format.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))
        throw std::runtime_error("Vulkan device cannot build AS from actual RGB32F raster positions");
}

void VulkanRenderer::State::destroy_ray_tracing() noexcept {
    // Lifecycle drains the device first. Each AS and every writable input is slot-owned.
    for(unsigned slot=0;slot<frames.size();++slot) {
        auto release=[&](Acceleration& acceleration) {
            if(acceleration.handle&&destroy_acceleration)destroy_acceleration(device,acceleration.handle,nullptr);
            acceleration.handle=VK_NULL_HANDLE;
            destroy_buffer(acceleration.storage);destroy_buffer(acceleration.scratch);destroy_buffer(acceleration.indices);
        };
        for(auto& acceleration:blas[slot])release(acceleration);
        blas[slot].clear();release(tlas[slot]);destroy_buffer(tlas_instances[slot]);ray_build_instances[slot].clear();
    }
    gpu_ray_instances.clear();primitive_map.clear();
}

vk_detail::Coverage VulkanRenderer::State::classify_coverage(const vk_detail::PassMaterial& material) const {
    Coverage out;out.cull=material.cull;
    if(material.sky!=MaterialSky::none||material.polygon_offset!=0) {
        out.exclusion="sky or polygon-offset overlay";return out;
    }
    for(const auto& deform:material.deforms)if(deform.kind!=MaterialDeformKind::wave) {
        out.exclusion="camera-dependent autosprite";return out;
    }
    bool establishing=false,cutout=false;
    for(std::uint32_t index=0;index<material.passes.size();++index) {
        const auto& pass=material.passes[index];
        if(pass.blend||!pass.depth_test||!pass.depth_write)continue;
        if(pass.depth_function==MaterialDepthFunc::equal&&!establishing)continue;
        out.pass_ordinals.push_back(index);
        establishing|=pass.depth_function==MaterialDepthFunc::lequal;
        cutout|=pass.alpha_test.has_value();
    }
    if(!establishing) {out.pass_ordinals.clear();out.exclusion="no non-overlay unblended LEQUAL depth coverage";return out;}
    out.kind=out.pass_ordinals.size()>1?Coverage::Kind::stack:(cutout?Coverage::Kind::cutout:Coverage::Kind::opaque);
    return out;
}

void VulkanRenderer::State::prepare_acceleration(const RenderScene& scene) {
    gpu_ray_instances.clear();primitive_map.clear();ray_coverage={};
    if(!settings.ray_tracing||capture_mode!=CaptureMode::ordinary)return;
    if(!create_acceleration)throw std::runtime_error("Hardware ray query requested without an enabled RT device");
    auto& structures=blas[frame_index];auto& instances_to_build=ray_build_instances[frame_index];
    instances_to_build.clear();
    // This slot's fence was waited by begin_frame. No other slot owns these handles.
    auto release=[&](Acceleration& acceleration) {
        if(acceleration.handle)destroy_acceleration(device,acceleration.handle,nullptr);
        acceleration.handle=VK_NULL_HANDLE;destroy_buffer(acceleration.storage);
        destroy_buffer(acceleration.scratch);destroy_buffer(acceleration.indices);
    };
    while(structures.size()>prepared_instances.size()) {release(structures.back());structures.pop_back();}
    structures.resize(prepared_instances.size());
    auto grow=[&](Buffer& buffer,VkDeviceSize bytes,VkBufferUsageFlags usage,VkMemoryPropertyFlags memory) {
        bytes=std::max<VkDeviceSize>(bytes,16);
        if(buffer.size>=bytes)return;
        VkDeviceSize capacity=std::max<VkDeviceSize>(buffer.size,4096);
        while(capacity<bytes) {
            if(capacity>std::numeric_limits<VkDeviceSize>::max()/2) {capacity=bytes;break;}
            capacity*=2;
        }
        destroy_buffer(buffer);buffer=create_buffer(capacity,usage,memory,true);
    };
    auto allocate=[&](Acceleration& acceleration,VkAccelerationStructureTypeKHR type,
                      VkAccelerationStructureGeometryKHR& geometry,std::uint32_t count,bool update) {
        auto info=build_info(type,acceleration,geometry);
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        acceleration_build_sizes(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&info,&count,&sizes);
        if(!acceleration.handle||acceleration.storage.size<sizes.accelerationStructureSize) {
            if(acceleration.handle)destroy_acceleration(device,acceleration.handle,nullptr);
            acceleration.handle=VK_NULL_HANDLE;
            grow(acceleration.storage,sizes.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            create.buffer=acceleration.storage.handle;create.size=acceleration.storage.size;create.type=type;
            ray_check(create_acceleration(device,&create,nullptr,&acceleration.handle),"vkCreateAccelerationStructureKHR");
            VkAccelerationStructureDeviceAddressInfoKHR address{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            address.accelerationStructure=acceleration.handle;acceleration.address=acceleration_address(device,&address);
            if(!acceleration.address)throw std::runtime_error("Vulkan AS returned zero device address");
            update=false;
        }
        const VkDeviceSize alignment=std::max<VkDeviceSize>(acceleration_properties.minAccelerationStructureScratchOffsetAlignment,1);
        const VkDeviceSize required=std::max(sizes.buildScratchSize,sizes.updateScratchSize);
        if(required>std::numeric_limits<VkDeviceSize>::max()-(alignment-1))throw std::runtime_error("Vulkan AS scratch size overflow");
        grow(acceleration.scratch,required+alignment-1,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        acceleration.update_pending=update;acceleration.needs_build=true;acceleration.updateable=true;
    };
    for(std::uint32_t instance_id=0;instance_id<prepared_instances.size();++instance_id) {
        auto& acceleration=structures[instance_id];acceleration.needs_build=false;
        const auto& instance=prepared_instances[instance_id];
        if(instance.geometry>=geometry.size()||instance_id>=gpu_instances.size())throw std::runtime_error("Vulkan RT instance/geometry table mismatch");
        const auto& draw=geometry[instance.geometry];
        // Shared immutable PM geometry cannot carry per-instance material policy.
        // Current coverage already classifies this instance's sky/overlay/deforms.
        if(!instance.visible||instance.interface||
           instance.coverage==invalid_id||instance.coverage>=coverage.size()||coverage[instance.coverage].kind==Coverage::Kind::excluded) {
            ++ray_coverage.excluded_geometry;continue;
        }
        if(!valid_transform(instance.model)) {++ray_coverage.invalid_instances;continue;}
        const auto vertex_bytes=VkDeviceSize(draw.vertex_count)*sizeof(RenderVertex);
        if(draw.vertex_count!=draw.final_pose.size()||!draw.final_vertices.address||draw.vertex_count==0||
           draw.final_vertices.size<vertex_bytes||draw.final_vertices.address>std::numeric_limits<VkDeviceAddress>::max()-vertex_bytes)
            throw std::runtime_error("Vulkan RT requires complete actual raster final pose and bounded device address");
        const auto triangle_base=gpu_instances[instance_id].ranges[3];
        const auto triangle_end=instance_id+1<gpu_instances.size()?gpu_instances[instance_id+1].ranges[3]:gpu_triangles.size();
        if(triangle_base>triangle_end||triangle_end>gpu_triangles.size())throw std::runtime_error("Vulkan RT original triangle range mismatch");
        const auto source_signature=draw.payload_signature^std::rotl(draw.topology_signature,17);
        const bool cached=acceleration.admission_valid&&acceleration.generation==scene.generation&&
                          acceleration.geometry==instance.geometry&&acceleration.triangle_base==triangle_base&&
                          acceleration.source_signature==source_signature&&acceleration.admission_model==instance.model;
        std::uint64_t topology=acceleration.topology_signature,positions=acceleration.vertex_signature;
        if(!cached) {
            acceleration.candidate_map.clear();acceleration.candidate_indices.clear();
            acceleration.rejected_nonfinite=acceleration.rejected_degenerate=acceleration.unsupported_attributes=0;
            const float limit=std::numeric_limits<float>::max();
            acceleration.admitted_bounds={{limit,limit,limit},{-limit,-limit,-limit}};
            topology=positions=1469598103934665603ull;
            hash_word(topology,draw.vertex_count);hash_word(topology,instance.geometry);
            for(std::size_t ordinal=triangle_base;ordinal<triangle_end;++ordinal) {
                const auto& triangle=gpu_triangles[ordinal];
                if(triangle.vertices[3]!=instance_id)throw std::runtime_error("Vulkan RT original triangle owner mismatch");
                const auto a=triangle.vertices[0],b=triangle.vertices[1],c=triangle.vertices[2];
                if(a>=draw.vertex_count||b>=draw.vertex_count||c>=draw.vertex_count)throw std::runtime_error("Vulkan RT triangle outside admitted raster vertices");
                const Vec3 pa=draw.final_pose[a].position,pb=draw.final_pose[b].position,pc=draw.final_pose[c].position;
                const Vec3 wa=transform_point(instance.model,pa),wb=transform_point(instance.model,pb),wc=transform_point(instance.model,pc);
                if(!finite(pa)||!finite(pb)||!finite(pc)||!finite(wa)||!finite(wb)||!finite(wc)) {
                    ++acceleration.rejected_nonfinite;continue;
                }
                if(a==b||a==c||b==c||degenerate(pa,pb,pc)||degenerate(wa,wb,wc)) {++acceleration.rejected_degenerate;continue;}
                if(!finite_attributes(draw.final_pose[a])||!finite_attributes(draw.final_pose[b])||!finite_attributes(draw.final_pose[c]))
                    ++acceleration.unsupported_attributes;
                if(ordinal>std::numeric_limits<std::uint32_t>::max())throw std::runtime_error("Vulkan RT original triangle map exceeds uint32");
                acceleration.candidate_map.push_back(static_cast<std::uint32_t>(ordinal));
                for(const auto p:{wa,wb,wc}) {
                    auto& bounds=acceleration.admitted_bounds;
                    bounds.minimum.x=std::min(bounds.minimum.x,p.x);bounds.minimum.y=std::min(bounds.minimum.y,p.y);bounds.minimum.z=std::min(bounds.minimum.z,p.z);
                    bounds.maximum.x=std::max(bounds.maximum.x,p.x);bounds.maximum.y=std::max(bounds.maximum.y,p.y);bounds.maximum.z=std::max(bounds.maximum.z,p.z);
                }
                for(const auto vertex:{a,b,c}) {
                    acceleration.candidate_indices.push_back(vertex);hash_word(topology,vertex);
                    const Vec3 p=draw.final_pose[vertex].position;
                    hash_word(positions,std::bit_cast<std::uint32_t>(p.x));hash_word(positions,std::bit_cast<std::uint32_t>(p.y));hash_word(positions,std::bit_cast<std::uint32_t>(p.z));
                }
            }
            acceleration.admitted_to_original.swap(acceleration.candidate_map);
            acceleration.source_signature=source_signature;acceleration.generation=scene.generation;
            acceleration.admission_model=instance.model;acceleration.triangle_base=triangle_base;acceleration.admission_valid=true;
        }
        ray_coverage.nonfinite_triangles+=acceleration.rejected_nonfinite;
        ray_coverage.degenerate_triangles+=acceleration.rejected_degenerate;
        ray_coverage.unsupported_hit_attributes+=acceleration.unsupported_attributes;
        const auto count=acceleration.admitted_to_original.size();
        if(!count) {
            acceleration.geometry=instance.geometry;acceleration.primitive_count=0;
            acceleration.topology_signature=topology;acceleration.vertex_signature=positions;
            continue;
        }
        if(count>acceleration_properties.maxPrimitiveCount||count>std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Vulkan actual raster BLAS exceeds device maxPrimitiveCount");
        const bool same_topology=acceleration.handle&&acceleration.topology_signature==topology&&
                                 acceleration.vertex_count==draw.vertex_count&&acceleration.primitive_count==count;
        acceleration.vertex_range=draw.final_vertices;
        acceleration.vertex_count=draw.vertex_count;acceleration.geometry=instance.geometry;
        if(!same_topology||acceleration.vertex_signature!=positions) {
            if(!same_topology) {
                const auto bytes=std::as_bytes(std::span(acceleration.candidate_indices));
                grow(acceleration.indices,bytes.size(),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
                std::memcpy(acceleration.indices.mapped,bytes.data(),bytes.size());flush_buffer(acceleration.indices,0,bytes.size());
            }
            auto triangles_geometry=triangle_geometry(acceleration);
            allocate(acceleration,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,triangles_geometry,static_cast<std::uint32_t>(count),same_topology&&acceleration.updateable);
        }
        acceleration.topology_signature=topology;acceleration.vertex_signature=positions;
        acceleration.primitive_count=static_cast<std::uint32_t>(count);acceleration.pose_revision=draw.identity.pose_revision;
        if(gpu_ray_instances.size()>=0x1000000||gpu_ray_instances.size()>=acceleration_properties.maxInstanceCount)
            throw std::runtime_error("Vulkan TLAS instance count exceeds device or customIndex limits");
        if(primitive_map.size()>std::numeric_limits<std::uint32_t>::max()-count)
            throw std::runtime_error("Vulkan RT primitive map exceeds uint32");
        GpuRayInstance record;record.mapping={instance_id,static_cast<std::uint32_t>(primitive_map.size()),static_cast<std::uint32_t>(count),0};
        VkAccelerationStructureInstanceKHR build{};
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col)build.transform.matrix[row][col]=instance.model[col*4+row];
        build.instanceCustomIndex=static_cast<std::uint32_t>(gpu_ray_instances.size());build.mask=0xff;
        // Vulkan AS facing is object-space and ignores instance determinant. Original
        // CW raster faces are world-space: flip positive determinants, not mirrored ones.
        build.flags=VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        if(determinant(instance.model)>0)build.flags|=VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
        build.accelerationStructureReference=acceleration.address;
        gpu_ray_instances.push_back(record);instances_to_build.push_back(build);
        primitive_map.insert(primitive_map.end(),acceleration.admitted_to_original.begin(),acceleration.admitted_to_original.end());
        ray_coverage.admitted_triangles+=count;
    }
    const auto bytes=std::as_bytes(std::span(instances_to_build));
    grow(tlas_instances[frame_index],bytes.size(),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if(!bytes.empty()) {std::memcpy(tlas_instances[frame_index].mapped,bytes.data(),bytes.size());flush_buffer(tlas_instances[frame_index],0,bytes.size());}
    auto& top=tlas[frame_index];
    VkAccelerationStructureGeometryKHR instance_geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    instance_geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    instance_geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instance_geometry.geometry.instances.data.deviceAddress=tlas_instances[frame_index].address;
    const auto count=static_cast<std::uint32_t>(instances_to_build.size());
    allocate(top,VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,instance_geometry,count,top.handle&&top.updateable&&top.primitive_count==count);
    top.primitive_count=count;
}

void VulkanRenderer::State::record_acceleration(VkCommandBuffer command) {
    if(!settings.ray_tracing||capture_mode!=CaptureMode::ordinary)return;
    VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    inputs.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|
                         VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR|VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    inputs.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT|
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,0,1,&inputs,0,nullptr,0,nullptr);
    const auto alignment=std::max<VkDeviceSize>(acceleration_properties.minAccelerationStructureScratchOffsetAlignment,1);
    for(auto& acceleration:blas[frame_index])if(acceleration.needs_build) {
        auto geometry_info=triangle_geometry(acceleration);
        auto info=build_info(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,acceleration,geometry_info);
        info.scratchData.deviceAddress=scratch_address(acceleration.scratch,alignment);
        VkAccelerationStructureBuildRangeInfoKHR range{};range.primitiveCount=acceleration.primitive_count;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges=&range;
        cmd_build_acceleration(command,1,&info,&ranges);acceleration.needs_build=false;
    }
    VkMemoryBarrier built{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    built.srcAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;built.dstAccessMask=VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,0,1,&built,0,nullptr,0,nullptr);
    auto& top=tlas[frame_index];
    if(!top.needs_build)throw std::runtime_error("Vulkan RT TLAS was not prepared for the current frame");
    VkAccelerationStructureGeometryKHR instance_geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    instance_geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    instance_geometry.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instance_geometry.geometry.instances.data.deviceAddress=tlas_instances[frame_index].address;
    auto info=build_info(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,top,instance_geometry);
    info.scratchData.deviceAddress=scratch_address(top.scratch,alignment);
    VkAccelerationStructureBuildRangeInfoKHR range{};range.primitiveCount=top.primitive_count;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges=&range;
    cmd_build_acceleration(command,1,&info,&ranges);top.needs_build=false;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&built,0,nullptr,0,nullptr);
}

void VulkanRenderer::State::update_ray_parameters(const RenderScene& scene) {
    const bool ordinary=capture_mode==CaptureMode::ordinary;
    gpu_frame.flags[1]=ordinary&&settings.ray_tracing&&settings.ray_shadows;
    gpu_frame.flags[2]=ordinary&&settings.ray_tracing&&settings.ray_reflections&&original.reflections;
    gpu_frame.flags[3]=ordinary&&scene.level&&settings.virtual_light_enabled&&settings.ray_tracing;
    const float azimuth=settings.virtual_light_azimuth*std::numbers::pi_v<float>/180;
    const float elevation=settings.virtual_light_elevation*std::numbers::pi_v<float>/180;
    gpu_frame.light_direction={std::cos(azimuth)*std::cos(elevation),std::sin(azimuth)*std::cos(elevation),std::sin(elevation),1};
    gpu_frame.light_color={settings.virtual_light_red,settings.virtual_light_green,settings.virtual_light_blue,settings.virtual_light_strength};
    // An invalid borrowed camera is not allowed to poison otherwise finite AS
    // bounds; its reflection incident is rejected in the shader, raster unchanged.
    Vec3 minimum=finite(scene.camera.position)?scene.camera.position:Vec3{},maximum=minimum;
    for(const auto& ray_instance:gpu_ray_instances) {
        const auto& bounds=blas[frame_index][ray_instance.mapping[0]].admitted_bounds;
        minimum.x=std::min(minimum.x,bounds.minimum.x);minimum.y=std::min(minimum.y,bounds.minimum.y);minimum.z=std::min(minimum.z,bounds.minimum.z);
        maximum.x=std::max(maximum.x,bounds.maximum.x);maximum.y=std::max(maximum.y,bounds.maximum.y);maximum.z=std::max(maximum.z,bounds.maximum.z);
    }
    const double dx=double(maximum.x)-minimum.x,dy=double(maximum.y)-minimum.y,dz=double(maximum.z)-minimum.z;
    const double extent=std::max(1.0,std::sqrt(dx*dx+dy*dy+dz*dz)*1.01);
    if(!std::isfinite(extent)||extent>std::numeric_limits<float>::max())throw std::runtime_error("Vulkan RT admitted world exceeds finite ray extent");
    gpu_frame.light_direction[3]=static_cast<float>(extent);
    // Local float ULP offset is added in the shader; this is the absolute floor only.
    gpu_frame.quality[3]=0.0001f;
}
} // namespace pusu
