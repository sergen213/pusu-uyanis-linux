#pragma once

#include "renderer_vulkan.hpp"
#include "renderer_shared.hpp"
#include "renderer_cube_cache.hpp"
#include <vulkan/vulkan.h>
#include <cstddef>
#include <limits>
#include <unordered_map>

namespace pusu::vk_detail {
inline constexpr std::uint32_t invalid_id=std::numeric_limits<std::uint32_t>::max();
enum class CaptureMode : std::uint32_t { ordinary, cube, history, loading };
struct Buffer {
    VkBuffer handle{};VkDeviceMemory memory{};VkDeviceSize size{},allocation_size{};
    void* mapped{};VkDeviceAddress address{};bool coherent{};
};
struct BufferRange { VkBuffer buffer{};VkDeviceSize offset{},size{};VkDeviceAddress address{}; };
struct Image {
    VkImage handle{};VkDeviceMemory memory{};VkImageView view{};
    VkFormat format{VK_FORMAT_UNDEFINED};VkExtent2D extent{};
    std::uint32_t mip_levels{1},layers{1};VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};
struct Target {
    Image color,resolve,depth;VkRenderPass render_pass{};VkFramebuffer framebuffer{};
    VkExtent2D extent{};VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
};
struct UploadBlock { Buffer buffer;VkDeviceSize used{}; };
struct FrameSlot {
    VkFence fence{};VkSemaphore acquired{};
    VkCommandPool pool{};VkCommandBuffer command{};
    VkDescriptorPool descriptors{};
    std::vector<UploadBlock> uploads;
    std::vector<Buffer> retired_buffers;
    std::vector<Image> retired_images;
    std::vector<VkAccelerationStructureKHR> retired_acceleration;
    std::vector<VkPipeline> retired_pipelines;
    std::uint64_t serial{};bool submitted{},recording{};
};
struct SwapImage {
    VkImage handle{};VkImageView view{};VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkFence in_flight{};VkFramebuffer framebuffer{};
    VkSemaphore complete{}; // presentation semaphore reuse follows acquisition of this image, not only frame fence
};
// std430 raw-word vertex storage avoids vec3 padding: fourteen float DWORDs.
static_assert(sizeof(RenderVertex)==56);
static_assert(offsetof(RenderVertex,position)==0&&offsetof(RenderVertex,uv)==12&&
              offsetof(RenderVertex,light_uv)==20&&offsetof(RenderVertex,normal)==28&&offsetof(RenderVertex,color)==40);
// Every shader table member is a mat4/vec4/uvec4, never a compiler enum/bool.
struct alignas(16) GpuPass {
    Matrix tc_matrix{render_identity};
    std::array<float,4> constant_color{1,1,1,1};
    std::array<std::uint32_t,4> binding{}; // image ID, sampler ID, cube flag, tcGen (0 base/1 LM/2 env/3 cube)
    std::array<std::uint32_t,4> modes{}; // RGB (0 constant/1 vertex/3 entity), alpha (0 none/1 GT0/2 LT128/3 GE128/4 EQ0), flags, source mode
    std::array<std::uint32_t,4> blend{}; // MaterialBlendFactor source/dest, depth (test bit0/write bit1/equal bit2), MaterialCull
    std::array<float,4> parameters{}; // polygon offset/ray LOD, or font cell sample bounds when flag64
};
// Pass flags: UI=1, legacy UV clamp=2, fog=4, eligible RT cube=8,
// new-light base contribution=16. Original cube-option/capture exclusion omits the draw entirely.
// UI font-cell sample bounds=64; no world/RT pass sets this flag.
struct alignas(16) GpuInstance {
    Matrix model{render_identity};
    std::array<float,12> normal_matrix{}; // three padded columns (inverse transpose), .w=0
    std::array<float,4> entity_color{1,1,1,1};
    std::array<std::uint32_t,4> ranges{}; // first pass/count, reserved zero, expanded original triangle base
    std::array<std::uint32_t,4> metadata{}; // geometry ID, coverage ID, lightmap image ID, flags
};
struct alignas(16) GpuTriangle {
    std::array<std::uint32_t,4> vertices{}; // local vertex IDs a,b,c; instance ID
    std::array<std::uint32_t,4> source{}; // original primitive ordinal, geometry ID, coverage ID, reserved
};
struct alignas(16) GpuGeometry {
    std::array<std::uint32_t,4> addresses{}; // low/high vertex BDA, low/high index BDA
    std::array<std::uint32_t,4> counts{}; // vertex count, index count, index16 flag, vertex stride (14 DWORDs)
};
struct alignas(16) GpuRayInstance {
    std::array<std::uint32_t,4> mapping{}; // prepared instance ID, admitted-to-original map base/count, reserved
};
struct alignas(16) GpuCoverage {
    std::array<std::uint32_t,4> passes{}; // first coverage-pass table index/count, MaterialCull, classification
};
struct alignas(16) GpuFrame {
    Matrix view{render_identity},view_projection{render_identity};
    std::array<float,4> camera_time{},fog_color{},fog_parameters{}; // fog start/end/density/mode
    std::array<float,4> light_direction{},light_color{}; // direction.xyz/ray extent; RGB/strength
    std::array<float,4> quality{}; // gamma, exposure EV, bloom strength, scale-aware ray epsilon
    std::array<std::uint32_t,4> flags{}; // HDR, ray shadows, ray reflections, enhanced-light enabled
};
struct DrawPush { std::uint32_t instance{},pass{},flags{},history_layer{}; };
static_assert(sizeof(GpuPass)==144&&sizeof(GpuInstance)==160&&sizeof(GpuTriangle)==32&&
              sizeof(GpuGeometry)==32&&sizeof(GpuRayInstance)==16&&sizeof(GpuCoverage)==16&&sizeof(GpuFrame)==240&&sizeof(DrawPush)==16);
struct GeometryIdentity {
    std::uint64_t generation{},object{},geometry_generation{},pose_revision{};
    std::uint32_t part{},surface{invalid_id};bool transient{};
    bool operator==(const GeometryIdentity&) const=default;
};
struct DrawGeometry {
    GeometryIdentity identity;
    Primitive primitive{Primitive::triangles};VkIndexType index_type{VK_INDEX_TYPE_UINT16};
    Buffer vertices,indices;BufferRange final_vertices,index_range;
    // Backend-owned CPU final pose/deform output; AS sees the same bytes as raster.
    std::vector<RenderVertex> rest_pose,final_pose,scratch;
    std::vector<std::uint16_t> indices16;
    std::vector<std::uint32_t> indices32;
    std::uint32_t vertex_count{},index_count{},material{},instance{},coverage{invalid_id};
    std::uint32_t last_seen{};
    Bounds bounds{};bool dynamic{},camera_dependent{},world{},sky{},overlay{};
    std::uint64_t topology_signature{},payload_signature{},last_submission{};
};
struct PassMaterial {
    std::vector<MaterialPass> passes; // owned finalized original stack, not reparsed/depth-refinalized
    std::vector<std::uint32_t> image_ids;
    std::vector<std::vector<std::uint32_t>> animation_ids;
    std::vector<MaterialDeform> deforms;
    MaterialCull cull{MaterialCull::back};MaterialLightGrid lightgrid{MaterialLightGrid::off};
    MaterialSort sort{MaterialSort::opaque};MaterialSky sky{MaterialSky::none};
    float polygon_offset{};bool mipmaps{},picmip{},compression{},fog{};
    std::string name;std::uint64_t signature{},prepared_revision{};
};
struct PreparedInstance {
    std::uint32_t geometry{},material{},first_pass{},pass_count{},coverage{invalid_id};
    Matrix model{render_identity};std::array<float,4> color{1,1,1,1};
    std::int32_t lightmap_selector{-1};std::uint32_t lightmap{invalid_id};
    std::uint64_t lighting_id{};std::uint32_t frame_ordinal{};
    Bounds root_bounds{},part_bounds{};Matrix culling_transform{render_identity};
    bool has_root{},has_culling{},culling_enabled{},frustum_cull{},visible{},world{},particle{},interface{};
};
struct ObjectLighting {
    const Mesh* mesh{};std::uint64_t geometry_generation{},prepared{},pose_revision{};
    std::vector<Vec3> positions,normals;
    std::vector<std::array<std::uint8_t,4>> colors;
    Vec3 origin{};Matrix transform{};MaterialLightGrid mode{MaterialLightGrid::off};bool valid{};
};
struct ImageResource {
    Image image;std::string name;MaterialTextureKind kind{MaterialTextureKind::none};
    bool mipmaps{},picmip{},compression{},capture{},center_set{},font{};
    Vec3 center{};std::uint64_t signature{},last_submission{};
    std::unique_ptr<VideoTexture> video;std::vector<std::uint8_t> pixels;
    double timestamp{-std::numeric_limits<double>::infinity()};
};
struct Coverage {
    enum class Kind : std::uint32_t { excluded,opaque,cutout,stack };
    Kind kind{Kind::excluded};std::vector<std::uint32_t> pass_ordinals;
    MaterialCull cull{MaterialCull::back};std::string exclusion;
};
struct PipelineKey {
    VkRenderPass render_pass{};VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    Primitive primitive{Primitive::triangles};MaterialCull cull{MaterialCull::back};
    MaterialBlendFactor source{MaterialBlendFactor::one},destination{MaterialBlendFactor::zero};
    bool blend{},depth_test{},depth_write{},equal{},interface{},hdr{},rays{};
    float polygon_offset{};
    bool operator==(const PipelineKey&) const=default;
};
struct PipelineRecord { PipelineKey key;VkPipeline pipeline{}; };
struct Acceleration {
    VkAccelerationStructureKHR handle{};Buffer storage,indices;
    BufferRange vertex_range;VkDeviceAddress address{};std::vector<std::uint32_t> admitted_to_original,candidate_map,candidate_indices;
    std::uint64_t topology_signature{},pose_revision{},last_submission{},vertex_signature{};
    Bounds admitted_bounds{};Matrix admission_model{render_identity};
    std::uint64_t source_signature{},generation{},rejected_nonfinite{},rejected_degenerate{},unsupported_attributes{};
    std::uint32_t triangle_base{};bool admission_valid{};
    std::uint32_t geometry{},primitive_count{},vertex_count{};bool updateable{},needs_build{},update_pending{};
};
struct RayCoverageStats {
    std::uint64_t admitted_triangles{},nonfinite_triangles{},degenerate_triangles{},invalid_instances{},
                  excluded_geometry{},unsupported_hit_attributes{};
};
struct History {
    Image image;Target target,display_target;VkImageView layer_view{};
    std::uint32_t side{},write{},warmup{},order{},tick{};std::uint64_t effect_serial{};
};
struct ShaderModules {
    VkShaderModule material_vertex{},material_fragment{},material_ray_fragment{},fullscreen_vertex{},post_fragment{},history_fragment{};
};
} // namespace pusu::vk_detail

namespace pusu {
struct VulkanRenderer::State {
    using LevelProgress=Renderer::LevelProgress;
    SDL_Window* window{};AssetStore& assets;MaterialLibrary& materials;Settings settings;
    OriginalGraphicsOptions original;VulkanCapabilities capabilities;
    VkInstance instance{};VkSurfaceKHR surface{};VkPhysicalDevice physical{};VkDevice device{};
    VkPhysicalDeviceProperties properties{};VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    std::uint32_t graphics_family{},present_family{};VkQueue graphics_queue{},present_queue{};
    VkSwapchainKHR swapchain{};VkFormat swap_format{VK_FORMAT_UNDEFINED},depth_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D extent{},requested_extent{};VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    std::vector<vk_detail::SwapImage> swap_images;
    std::array<vk_detail::FrameSlot,2> frames;
    VkCommandPool immediate_pool{};VkCommandBuffer immediate_command{};VkFence immediate_fence{};
    bool immediate_recording{};
    std::uint32_t frame_index{},image_index{};std::uint64_t submission_serial{},completed_serial{};
    bool recreate_pending{},acquired{},frame_rendered{},presented{},resources_dirty{},rt_supported{},rt_enabled{},preparing_level{};
    std::uint32_t window_service_tick{};
    vk_detail::CaptureMode capture_mode{vk_detail::CaptureMode::ordinary};
    vk_detail::Target scene_target,output_target,blur_targets[2],cube_target;
    vk_detail::Target* active_target{};
    vk_detail::History history;
    VkRenderPass output_render_pass{};VkSampler post_sampler{},history_sampler{};
    VkPipelineLayout material_layout{},post_layout{};
    VkDescriptorSetLayout material_descriptors{},post_descriptors{};
    vk_detail::ShaderModules shaders;
    std::vector<vk_detail::PipelineRecord> pipelines;
    VkPipeline post_pipeline{},history_pipeline{},blur_pipeline{},swap_pipeline{};
    std::array<VkSampler,18> samplers{};VkSampler font_sampler{};
    std::uint32_t image_descriptor_capacity{},raster_frame_ordinal{};
    std::vector<vk_detail::ImageResource> images;
    std::vector<VkDescriptorImageInfo> descriptor_images_2d,descriptor_images_cube;
    std::vector<std::uint8_t> image_upload_pixels,image_upload_encoded;
    std::vector<VkBufferImageCopy> image_upload_regions;
    std::unordered_map<std::string,std::uint32_t,RenderNameHash,RenderNameEqual> image_names,font_atlases;
    std::unordered_map<std::string,std::array<std::uint32_t,2>,ShaderNameHash,ShaderNameEqual> named_material_ids;
    std::unordered_map<const Material*,std::uint32_t> material_ids;
    std::unordered_map<const Mesh*,std::uint32_t> mesh_ids;
    std::unordered_map<std::uint64_t,std::vector<vk_detail::ObjectLighting>> object_lighting;
    std::vector<vk_detail::ObjectLighting> transient_lighting;
    std::vector<vk_detail::PassMaterial> pass_materials;
    std::vector<vk_detail::Coverage> material_coverage;
    std::vector<vk_detail::DrawGeometry> geometry;
    std::vector<RenderVertex> world_vertices;vk_detail::Buffer world_vertex_buffer;
    vk_detail::Buffer world_index_buffer,patch_index_buffer;
    vk_detail::Buffer interface_index_buffer,particle_triangle_index_buffer;
    std::vector<std::uint32_t> interface_geometry;
    std::vector<vk_detail::PreparedInstance> prepared_instances;
    std::vector<vk_detail::Coverage> coverage;
    std::vector<std::uint32_t> capture_images,lightmap_images,world_order,world_geometry;
    std::vector<CullingBounds> world_bounds;
    std::vector<unsigned> leaf_culls,node_culls;
    std::vector<std::int32_t> node_parents,leaf_parents;
    std::vector<std::uint32_t> surface_stamps,node_stamps,leaf_stamps;
    std::array<Vec3,2> sky_centers{};std::array<float,2> sky_radii{};
    const Level* level{};std::uint64_t generation{std::numeric_limits<std::uint64_t>::max()},prepare_revision{},pose_revision{};
    std::string reflection_path;bool reflections_pending{};int capture_display_height{1};
    RenderCamera camera;RenderFog fog;double seconds{};std::uint32_t tick_milliseconds{},frame_stamp{};
    Matrix view_projection{render_identity};ModelFrustum frustum;
    std::vector<vk_detail::GpuPass> gpu_passes;
    std::vector<vk_detail::GpuInstance> gpu_instances;
    std::vector<vk_detail::GpuTriangle> gpu_triangles;
    std::vector<vk_detail::GpuGeometry> gpu_geometry;
    std::vector<vk_detail::GpuRayInstance> gpu_ray_instances;
    std::vector<std::uint32_t> primitive_map;
    std::vector<vk_detail::GpuCoverage> gpu_coverage;
    std::vector<std::uint32_t> coverage_passes;
    vk_detail::GpuFrame gpu_frame;
    vk_detail::BufferRange frame_range,passes_range,instances_range,geometry_range,triangles_range,coverage_range,coverage_passes_range,ray_instances_range,primitive_map_range;
    VkDescriptorSet active_material_set{};
    std::array<std::vector<vk_detail::Acceleration>,2> blas;
    std::array<vk_detail::Acceleration,2> tlas;
    std::array<vk_detail::Buffer,2> tlas_instances;
    std::array<vk_detail::Buffer,2> acceleration_scratch;
    std::array<std::vector<VkAccelerationStructureInstanceKHR>,2> ray_build_instances;
    vk_detail::RayCoverageStats ray_coverage;
    PFN_vkCreateAccelerationStructureKHR create_acceleration{};
    PFN_vkDestroyAccelerationStructureKHR destroy_acceleration{};
    PFN_vkGetAccelerationStructureBuildSizesKHR acceleration_build_sizes{};
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_acceleration{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR acceleration_address{};

    // Lifecycle owner: all queue submission, frame-slot reuse, memory ownership and public forwards.
    State(SDL_Window*,AssetStore&,MaterialLibrary&,const Settings&);
    ~State();
    void initialize_device();void destroy_device() noexcept;
    void recreate_swapchain();void destroy_swapchain() noexcept;
    void validate_settings(const Settings&) const;
    void resize(int,int);void apply_settings(const Settings&);void apply_original_options(const OriginalGraphicsOptions&);
    bool begin_frame();void submit_frame();bool present();void wait_idle();
    void retire_frame(vk_detail::FrameSlot&);
    void service_window();
    VkCommandBuffer begin_commands();void finish_commands(VkCommandBuffer,bool wait);
    std::uint32_t memory_type(std::uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred=0) const;
    vk_detail::Buffer create_buffer(VkDeviceSize,VkBufferUsageFlags,VkMemoryPropertyFlags,bool address=false);
    void destroy_buffer(vk_detail::Buffer&) noexcept;
    vk_detail::BufferRange upload_bytes(std::span<const std::byte>,VkDeviceSize alignment,VkBufferUsageFlags);
    void flush_buffer(const vk_detail::Buffer&,VkDeviceSize offset,VkDeviceSize size);
    vk_detail::Image create_image(VkExtent2D,VkFormat,VkImageUsageFlags,VkSampleCountFlagBits=VK_SAMPLE_COUNT_1_BIT,
                                 std::uint32_t layers=1,std::uint32_t mip_levels=1,bool cube=false);
    void destroy_image(vk_detail::Image&) noexcept;
    void transition_image(VkCommandBuffer,vk_detail::Image&,VkImageLayout,VkPipelineStageFlags,VkAccessFlags,VkPipelineStageFlags,VkAccessFlags);
    void readback(std::span<std::uint8_t>,std::size_t row_pitch);
    void render(const RenderScene&,std::span<const InterfaceQuad>);void render_loading(std::span<const InterfaceQuad>);

    // Raster owner: source admission and owned final geometry/material tables for every submitted view.
    void initialize_raster();void destroy_raster() noexcept;void refresh_samplers();
    void begin_generation(std::uint64_t,const Level*);
    void prepare_resources(const RenderScene&,std::span<const InterfaceQuad>);
    void prepare_interface_resources(std::span<const InterfaceQuad>);
    void prepare_level_resources(const Level&,std::uint64_t,LevelProgress,void*);
    void prepare_frame_geometry(const RenderScene&);
    void prepare_frame_tables();
    void record_world_entities(VkCommandBuffer,const RenderScene&,vk_detail::CaptureMode);
    void record_interface(VkCommandBuffer,std::span<const InterfaceQuad>,bool capture_only=false);
    void upload_material_tables(VkCommandBuffer);
    std::uint32_t admit_material(const Material&);
    std::uint32_t admit_image(const MaterialTexture&,const Material&);
    std::uint32_t admit_texture(std::string_view,bool picmip,bool compression,bool font=false);
    void upload_image(vk_detail::ImageResource&,std::span<const std::uint8_t>,std::uint32_t components,std::uint32_t layer=0);
    void update_video(std::uint32_t image_id);
    VkPipeline material_pipeline(const vk_detail::PipelineKey&);
    void draw_instance(VkCommandBuffer,std::uint32_t instance_id,bool interface=false);

    // Quality owner: target/render-pass contracts, captures and display composition, never submits borrowed state.
    void initialize_quality();void destroy_quality() noexcept;
    void create_targets(VkExtent2D,VkSampleCountFlagBits);void destroy_targets() noexcept;
    void begin_target(VkCommandBuffer,vk_detail::Target&,std::array<float,4> clear,bool clear_depth=true);
    void end_target(VkCommandBuffer,vk_detail::Target&);
    void record_post(VkCommandBuffer);void record_output_to_swapchain(VkCommandBuffer);
    void prepare_cube_resources(const RenderScene&,std::string_view bsp_path);
    void reset_history();void record_history_consume(VkCommandBuffer,const RenderScene&);
    void record_history_after_present(const RenderScene&,std::span<const InterfaceQuad>);

    // RT owner: AS-only input admission, owned primitive maps, candidate coverage and hardware queries.
    void initialize_ray_tracing();void destroy_ray_tracing() noexcept;
    void prepare_acceleration(const RenderScene&);void record_acceleration(VkCommandBuffer);
    void update_ray_parameters(const RenderScene&);
    vk_detail::Coverage classify_coverage(const vk_detail::PassMaterial&) const;
};
} // namespace pusu
