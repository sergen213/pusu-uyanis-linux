#pragma once

#include "renderer.hpp"
#include <string>
#include <vector>

struct SDL_Window;
namespace pusu {
struct VulkanCapabilities {
    bool available{},hardware_ray_tracing{},msaa{},anisotropy{},bloom{},gamma{},vsync{};
    bool ray_shadows{},ray_reflections{},hdr_tonemapping{};
    int max_msaa{},max_anisotropy{1};
    std::vector<int> msaa_values,anisotropy_values;
    std::vector<std::string> present_modes;
    std::string vendor,renderer,version,error;
};
VulkanCapabilities query_vulkan_capabilities(SDL_Window* window);
// Concrete Vulkan 1.2 backend; never creates a GL context or falls back to GL.
// The SDL Vulkan window must outlive this renderer. Scene/UI inputs are borrowed
// only for each synchronous call; submitted buffers/descriptors are backend-owned.
class VulkanRenderer {
public:
    using LevelProgress=Renderer::LevelProgress;
    VulkanRenderer(SDL_Window*,AssetStore&,MaterialLibrary&,const Settings&);
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&)=delete;
    VulkanRenderer& operator=(const VulkanRenderer&)=delete;
    void resize(int drawable_width,int drawable_height);
    void apply_settings(const Settings&);
    void apply_original_options(const OriginalGraphicsOptions&);
    void prepare(const RenderScene&,std::span<const InterfaceQuad>);
    // Progress may synchronously render/present loading and advance the frame slot.
    // Following preparation uploads reacquire that slot's fence before mapped writes.
    void prepare_level(const Level&,std::uint64_t generation,LevelProgress,void*);
    void prepare_reflections(const RenderScene&,std::string_view bsp_path);
    void render(const RenderScene&,std::span<const InterfaceQuad> = {});
    void render_loading(std::span<const InterfaceQuad>);
    // An acquire skipped by resize/minimize has no screenshot/presentation to consume.
    bool frame_ready() const noexcept;
    // Copy the final SDR output before present, RGBA8/top-left; explicit transfer wait.
    void readback(std::span<std::uint8_t> rgba,std::size_t row_pitch);
    // False on a skipped/out-of-date frame; only success permits after_present.
    bool present();
    void after_present(const RenderScene&,std::span<const InterfaceQuad> = {});
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace pusu
