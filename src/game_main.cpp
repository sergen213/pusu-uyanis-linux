#include "game.hpp"
#include "interface.hpp"
#include "media.hpp"
#include "renderer.hpp"
#include "renderer_vulkan.hpp"
#include "resources.hpp"
#include "settings.hpp"
#include <GL/glew.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_vulkan.h>
#include <limits>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Options {
    std::filesystem::path data{pusu::user_data_directory() / "data"};
    std::filesystem::path settings{pusu::user_settings_file()};
    std::string renderer;
    bool list{}, capabilities{}, help{};
};
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        auto value = [&]() -> std::string_view {
            if (++i == argc || std::string_view(argv[i]).starts_with("--"))
                throw std::runtime_error("Missing value for " + std::string(argument));
            return argv[i];
        };
        if (argument == "--data") result.data = value();
        else if (argument == "--settings") result.settings = value();
        else if (argument == "--renderer") result.renderer = value();
        else if (argument == "--list-renderers") result.list = true;
        else if (argument == "--capabilities") result.capabilities = true;
        else if (argument == "--help" || argument == "-h") result.help = true;
        else throw std::runtime_error("Unknown argument: " + std::string(argument));
    }
    if (int(result.list) + int(result.capabilities) + int(result.help) > 1)
        throw std::runtime_error("Choose one of --list-renderers, --capabilities, or --help");
    return result;
}
std::string json_string(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() + 2);
    result.push_back('"');
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { result.push_back('\\'); result.push_back(char(ch)); }
        else if (ch < 32) {
            result += "\\u00";
            result.push_back(hex[ch >> 4]); result.push_back(hex[ch & 15]);
        } else result.push_back(char(ch));
    }
    result.push_back('"');
    return result;
}
class Window {
public:
    Window(const pusu::Settings& settings, bool hidden):vulkan_(settings.renderer=="vulkan") {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
            throw std::runtime_error(std::string("SDL initialization: ") + SDL_GetError());
        initialized_ = true;
        try {
            if (!vulkan_) {
            attribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            attribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            attribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            attribute(SDL_GL_DOUBLEBUFFER, 1);
            attribute(SDL_GL_DEPTH_SIZE, 24);
            attribute(SDL_GL_STENCIL_SIZE, 8);
            // The renderer owns its MSAA/HDR framebuffer and explicit resolve.
            attribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            }
            Uint32 flags = (vulkan_?SDL_WINDOW_VULKAN:SDL_WINDOW_OPENGL) | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
            flags |= hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
            if (settings.fullscreen && !hidden) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            window_ = SDL_CreateWindow("Pusu: Uyanış", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      hidden ? 640 : settings.width, hidden ? 480 : settings.height, flags);
            if (!window_) throw std::runtime_error(std::string("SDL window: ") + SDL_GetError());
            if (!vulkan_) {
            context_ = SDL_GL_CreateContext(window_);
            if (!context_) throw std::runtime_error(std::string("OpenGL 3.3 context: ") + SDL_GetError());
            glewExperimental = GL_TRUE;
            const GLenum result = glewInit();
            if (result != GLEW_OK)
                throw std::runtime_error(std::string("GLEW: ") + reinterpret_cast<const char*>(glewGetErrorString(result)));
            // GLEW may probe legacy extension enumeration in a core context.
            while (glGetError() != GL_NO_ERROR) {}
            if (!GLEW_VERSION_3_3) throw std::runtime_error("OpenGL 3.3 is unavailable on this display");
            }
        } catch (...) { release(); throw; }
    }
    ~Window() { release(); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    SDL_Window* get() const noexcept { return window_; }
    bool vulkan() const noexcept { return vulkan_; }
    void drawable_size(int& width,int& height) const {
        if(vulkan_)SDL_Vulkan_GetDrawableSize(window_,&width,&height);
        else SDL_GL_GetDrawableSize(window_,&width,&height);
    }
    void apply(const pusu::Settings& settings) {
        if (!applied_ || fullscreen_ != settings.fullscreen) {
            if (SDL_SetWindowFullscreen(window_, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
                throw std::runtime_error(std::string("Fullscreen window: ") + SDL_GetError());
            fullscreen_ = settings.fullscreen;
        }
        if (!settings.fullscreen && (!applied_ || width_ != settings.width || height_ != settings.height))
            SDL_SetWindowSize(window_, settings.width, settings.height);
        if (!vulkan_ && (!applied_ || vsync_ != settings.vsync)) {
            if (SDL_GL_SetSwapInterval(settings.vsync ? 1 : 0) != 0 ||
                SDL_GL_GetSwapInterval() != (settings.vsync ? 1 : 0))
                throw std::runtime_error(std::string("OpenGL display does not support requested VSync: ") + SDL_GetError());
            vsync_ = settings.vsync;
        }
        width_ = settings.width; height_ = settings.height; applied_ = true;
    }
private:
    SDL_Window* window_{};
    SDL_GLContext context_{};
    bool initialized_{}, applied_{}, fullscreen_{}, vsync_{},vulkan_{};
    int width_{}, height_{};
    static void attribute(SDL_GLattr name, int value) {
        if (SDL_GL_SetAttribute(name, value) != 0)
            throw std::runtime_error(std::string("SDL OpenGL attribute: ") + SDL_GetError());
    }
    void release() noexcept {
        if (context_) { SDL_GL_DeleteContext(context_); context_ = nullptr; }
        if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
        if (initialized_) { SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER); initialized_ = false; }
    }
};
pusu::VulkanCapabilities opengl_capabilities(SDL_Window* window) {
    pusu::VulkanCapabilities result;
    GLint maximum{};glGetIntegerv(GL_MAX_SAMPLES,&maximum);result.max_msaa=std::max(0,maximum);
    result.msaa_values.push_back(0);
    if(GLEW_VERSION_4_2||GLEW_ARB_internalformat_query) {
        auto values=[](GLenum format){
            GLint count{};glGetInternalformativ(GL_RENDERBUFFER,format,GL_NUM_SAMPLE_COUNTS,1,&count);
            if(count<0||count>64)throw std::runtime_error("Invalid OpenGL format sample-count report");
            std::vector<GLint> values(std::size_t(count),0);
            if(count)glGetInternalformativ(GL_RENDERBUFFER,format,GL_SAMPLES,count,values.data());
            return values;
        };
        auto color=values(GL_RGBA16F),depth=values(GL_DEPTH24_STENCIL8);
        for(int sample:{2,4,8,16})
            if(std::find(color.begin(),color.end(),sample)!=color.end()&&std::find(depth.begin(),depth.end(),sample)!=depth.end())
                result.msaa_values.push_back(sample);
    } else {
        // GL3.3 without internalformat-query: verify actual storage, including driver round-up.
        GLint previous{};glGetIntegerv(GL_RENDERBUFFER_BINDING,&previous);
        GLuint probe{};glGenRenderbuffers(1,&probe);glBindRenderbuffer(GL_RENDERBUFFER,probe);
        for(int sample:{2,4,8,16})if(sample<=maximum) {
            bool supported=true;
            for(GLenum format:{GLenum(GL_RGBA16F),GLenum(GL_DEPTH24_STENCIL8)}) {
                glRenderbufferStorageMultisample(GL_RENDERBUFFER,sample,format,1,1);
                if(glGetError()!=GL_NO_ERROR){supported=false;continue;}
                GLint actual{};glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_SAMPLES,&actual);
                if(actual!=sample)supported=false;
            }
            if(supported)result.msaa_values.push_back(sample);
        }
        glBindRenderbuffer(GL_RENDERBUFFER,GLuint(previous));glDeleteRenderbuffers(1,&probe);
    }
    if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("Cannot query actual OpenGL color/depth sample formats");
    GLfloat anisotropy=1;
    if(GLEW_EXT_texture_filter_anisotropic)glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&anisotropy);
    result.max_anisotropy=std::max(1,int(anisotropy));
    for(int value:{1,2,4,8,16})if(value<=result.max_anisotropy)result.anisotropy_values.push_back(value);
    const int previous_interval=SDL_GL_GetSwapInterval();
    if(SDL_GL_SetSwapInterval(1)==0&&SDL_GL_GetSwapInterval()==1)result.present_modes.emplace_back("fifo");
    if(SDL_GL_SetSwapInterval(0)==0&&SDL_GL_GetSwapInterval()==0)result.present_modes.emplace_back("immediate");
    if(SDL_GL_SetSwapInterval(previous_interval)!=0)throw std::runtime_error("Cannot restore OpenGL presentation interval after capability query");
    if(result.present_modes.empty())throw std::runtime_error("Display exposes no selectable OpenGL presentation interval");
    if(SDL_GL_GetCurrentWindow()!=window)throw std::runtime_error("OpenGL capability query requires its current SDL window");
    auto text=[](GLenum name){auto* value=glGetString(name);return value?std::string(reinterpret_cast<const char*>(value)):std::string{};};
    result.vendor=text(GL_VENDOR);result.renderer=text(GL_RENDERER);result.version=text(GL_VERSION);
    result.msaa=result.msaa_values.size()>1;result.anisotropy=result.anisotropy_values.size()>1;
    result.vsync=std::find(result.present_modes.begin(),result.present_modes.end(),"fifo")!=result.present_modes.end();
    result.bloom=result.gamma=result.available=true;
    return result;
}
// Concrete two-case dispatch only; window/context/backend lifetime stays explicit.
class RendererDispatch {
public:
    RendererDispatch(Window& window,pusu::AssetStore& assets,pusu::MaterialLibrary& materials,const pusu::Settings& settings):window_(window) {
        if(window.vulkan())vulkan_.emplace(window.get(),assets,materials,settings);
        else {capabilities_=opengl_capabilities(window.get());validate_gl(settings);gl_.emplace(assets,materials,settings);}
    }
    void resize(int width,int height){if(vulkan_)vulkan_->resize(width,height);else gl_->resize(width,height);}
    void apply_settings(const pusu::Settings& settings){
        if((settings.renderer=="vulkan")!=window_.vulkan())throw std::runtime_error("Renderer changes require a new launch");
        if(vulkan_)vulkan_->apply_settings(settings);else{validate_gl(settings);gl_->apply_settings(settings);}
    }
    void apply_original_options(const pusu::OriginalGraphicsOptions& options){if(vulkan_)vulkan_->apply_original_options(options);else gl_->apply_original_options(options);}
    void prepare(const pusu::RenderScene& scene,std::span<const pusu::InterfaceQuad> quads){if(vulkan_)vulkan_->prepare(scene,quads);else gl_->prepare(scene,quads);}
    void prepare_level(const pusu::Level& level,std::uint64_t generation,pusu::Renderer::LevelProgress progress,void* context){if(vulkan_)vulkan_->prepare_level(level,generation,progress,context);else gl_->prepare_level(level,generation,progress,context);}
    void prepare_reflections(const pusu::RenderScene& scene,std::string_view bsp){if(vulkan_)vulkan_->prepare_reflections(scene,bsp);else gl_->prepare_reflections(scene,bsp);}
    void render(const pusu::RenderScene& scene,std::span<const pusu::InterfaceQuad> quads){if(vulkan_)vulkan_->render(scene,quads);else gl_->render(scene,quads);}
    void render_loading(std::span<const pusu::InterfaceQuad> quads){if(vulkan_)vulkan_->render_loading(quads);else gl_->render_loading(quads);}
    bool present(){if(vulkan_)return vulkan_->present();SDL_GL_SwapWindow(window_.get());return true;}
    void after_present(const pusu::RenderScene& scene,std::span<const pusu::InterfaceQuad> quads){if(vulkan_)vulkan_->after_present(scene,quads);else gl_->after_present(scene,quads);}
    pusu::VulkanRenderer* vulkan() noexcept{return vulkan_?&*vulkan_:nullptr;}
private:
    Window& window_;std::optional<pusu::Renderer> gl_;std::optional<pusu::VulkanRenderer> vulkan_;pusu::VulkanCapabilities capabilities_;
    void validate_gl(const pusu::Settings& settings)const{
        if(std::find(capabilities_.msaa_values.begin(),capabilities_.msaa_values.end(),settings.msaa)==capabilities_.msaa_values.end())
            throw std::runtime_error("Requested OpenGL MSAA is unsupported for the actual color/depth formats; select a supported value or Off");
        if(std::find(capabilities_.anisotropy_values.begin(),capabilities_.anisotropy_values.end(),settings.anisotropy)==capabilities_.anisotropy_values.end())
            throw std::runtime_error("Requested OpenGL anisotropy is unsupported; select a supported value or 1");
        const std::string_view mode=settings.vsync?"fifo":"immediate";
        if(std::find(capabilities_.present_modes.begin(),capabilities_.present_modes.end(),mode)==capabilities_.present_modes.end())
            throw std::runtime_error("Requested OpenGL VSync selection is unsupported on this display");
    }
};
struct MousePoint { pusu::Vec2 point; bool outside{true}; };
MousePoint menu_mouse(const Window& window, int x, int y) {
    int width{}, height{}, drawable_width{}, drawable_height{};
    SDL_GetWindowSize(window.get(), &width, &height);
    window.drawable_size(drawable_width,drawable_height);
    if (width <= 0 || height <= 0 || drawable_width <= 0 || drawable_height <= 0) return {};
    const float scale = std::min(float(drawable_width) / 1024, float(drawable_height) / 768);
    const float left = (drawable_width - 1024 * scale) * 0.5f;
    const float top = (drawable_height - 768 * scale) * 0.5f;
    const float px = float(x) * drawable_width / width;
    const float py = float(y) * drawable_height / height;
    const pusu::Vec2 point{(px - left) / scale, (py - top) / scale};
    return {point, point.x < 0 || point.x >= 1024 || point.y < 0 || point.y >= 768};
}
void screenshot(const Window& window,pusu::VulkanRenderer* vulkan) {
    int width{}, height{};
    window.drawable_size(width,height);
    if (width <= 0 || height <= 0) return;
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
    Surface image(SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32), SDL_FreeSurface);
    if (!image) throw std::runtime_error(std::string("Screenshot surface: ") + SDL_GetError());
    if(vulkan) {
        vulkan->readback(std::span(static_cast<std::uint8_t*>(image->pixels),std::size_t(image->pitch)*height),std::size_t(image->pitch));
    } else {
    GLint alignment{}, row_length{}, read_buffer{}, read_framebuffer{}, pack_buffer{};
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, image->pitch / 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image->pixels);
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(pack_buffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_framebuffer));
    glReadBuffer(GLenum(read_buffer));
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) throw std::runtime_error("OpenGL screenshot readback failed: " + std::to_string(error));
    std::vector<std::uint8_t> row(std::size_t(image->pitch));
    auto* pixels = static_cast<std::uint8_t*>(image->pixels);
    for (int y = 0; y < height / 2; ++y) {
        auto* a = pixels + std::size_t(y) * image->pitch;
        auto* b = pixels + std::size_t(height - y - 1) * image->pitch;
        std::copy_n(a, image->pitch, row.data());
        std::copy_n(b, image->pitch, a);
        std::copy_n(row.data(), image->pitch, b);
    }
    }
    const auto directory = pusu::user_data_directory() / "screenshots";
    std::filesystem::create_directories(directory);
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = directory / ("pusu-" + std::to_string(timestamp) + ".png");
    if (IMG_SavePNG(image.get(), path.c_str()) != 0)
        throw std::runtime_error(std::string("Saving screenshot: ") + IMG_GetError());
    std::cerr << "Screenshot: " << path << '\n';
}
int game_loop(Window& window, pusu::AssetStore& assets, pusu::MaterialLibrary& materials,
              pusu::Settings& settings, const std::filesystem::path& settings_file) {
    pusu::Interface interface(assets, settings, materials);
    interface.set_settings_file(settings_file);
    pusu::Media media(assets, settings);
    interface.set_media(media);
    pusu::Game game(assets, materials, settings, interface, media);
    interface.set_action_handler([&](const pusu::MenuAction& action) { game.action(action); });
    RendererDispatch renderer(window, assets, materials, settings);
    int drawable_width{}, drawable_height{};
    const auto refresh_game_size = [&] {
        window.drawable_size(drawable_width,drawable_height);
        if (drawable_width > 0 && drawable_height > 0) {
            game.resize(unsigned(drawable_width), unsigned(drawable_height));
            interface.set_drawable_size(unsigned(drawable_width), unsigned(drawable_height));
        }
    };
    bool prepared{};
    struct LoadingContext {
        Window& window; RendererDispatch& renderer; pusu::Interface& interface;
        pusu::Settings& settings; pusu::MaterialLibrary& materials; bool& prepared; pusu::Game& game;
        pusu::Game::LoadingPresenter present{};
        ~LoadingContext() { game.set_loading_presenter(nullptr, nullptr); }
    } loading{window, renderer, interface, settings, materials, prepared, game};
    loading.present = +[](void* context, std::string_view shader, float progress) {
        auto& state = *static_cast<LoadingContext*>(context);
        int width{}, height{};
        state.window.drawable_size(width,height);
        if (width <= 0 || height <= 0) return;
        std::array<pusu::InterfaceQuad,3> quads;
        quads[0].rect = {0,0,1024,768}; quads[0].shader = shader;
        quads[1].rect = {102.4f,721.92f,819.2f,7.68f}; quads[1].shader = "progress_bar_transparent";
        quads[2].rect = {102.4f,721.92f,819.2f*progress,7.68f}; quads[2].uv.width = progress;
        quads[2].shader = "progress_bar";
        for (auto& quad : quads) quad.full_viewport = true;
        const auto draws = shader.empty() ? std::span<const pusu::InterfaceQuad>{} : std::span<const pusu::InterfaceQuad>(quads);
        // Actual 004297f0 presentation constructs these shaders; renderer preload does not.
        for(const auto& quad:draws)state.materials.construct(quad.shader,0,0,false);
        state.renderer.apply_settings(state.settings);
        state.renderer.apply_original_options(state.interface.graphics_options());
        state.renderer.resize(width, height);
        state.renderer.render_loading(draws);
        state.renderer.present();
        state.prepared = false;
    };
    game.set_loading_presenter(&loading, loading.present,
        +[](void* context, const pusu::Level& level, std::uint64_t generation, std::string_view shader) {
            struct ProgressContext { LoadingContext& loading; std::string_view shader; };
            ProgressContext progress{*static_cast<LoadingContext*>(context), shader};
            progress.loading.renderer.prepare_level(level, generation,
                +[](void* context, float value) {
                    auto& progress = *static_cast<ProgressContext*>(context);
                    progress.loading.present(&progress.loading, progress.shader, value);
                }, &progress);
        },
        +[](void* context, const pusu::RenderScene& scene, std::string_view bsp_path) {
            auto& state = *static_cast<LoadingContext*>(context);
            // Load/on_start can invalidate previous UI shader views. The normal update
            // rebuilds and prepares current UI before rendering; reflections need only the scene.
            state.renderer.prepare(scene, {});
            state.renderer.prepare_reflections(scene, bsp_path);
        });
    refresh_game_size();
    game.boot();
    window.apply(settings);
    // The original interface renders its authored cursor; do not overlay an OS arrow.
    SDL_ShowCursor(SDL_DISABLE);
    bool screenshot_held{};
    bool relative{}, closing{}, focused{true}, minimized{};
    std::uint64_t interface_revision{};
    pusu::OriginalGraphicsOptions graphics_options;
    auto previous = SDL_GetPerformanceCounter();
    const auto frequency = SDL_GetPerformanceFrequency();
    if (!frequency) throw std::runtime_error("SDL performance timer has no frequency");
    while (!closing && !game.wants_quit()) {
        pusu::GameInput input;
        bool pause{};
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) closing = true;
            if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) closing = true;
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = false;
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
                if (event.window.event == SDL_WINDOWEVENT_MINIMIZED) minimized = true;
                if (event.window.event == SDL_WINDOWEVENT_RESTORED) minimized = false;
            }
            int x{}, y{};
            SDL_GetMouseState(&x, &y);
            if (event.type == SDL_MOUSEMOTION) { x = event.motion.x; y = event.motion.y; }
            else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                x = event.button.x; y = event.button.y;
            }
            const auto mouse = menu_mouse(window, x, y);
            const bool consumed = interface.input(event, mouse.point, mouse.outside);
            if (focused && !minimized && !consumed) {
                if (event.type == SDL_MOUSEMOTION && relative) {
                    input.mouse_x += float(event.motion.xrel);
                    input.mouse_y += float(event.motion.yrel);
                }
                // 004576a0 and 0045d760 dispatch Escape on release, in separate game/menu routes.
                if (event.type == SDL_KEYUP && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                    pause = true;
            }
        }
        if (closing || game.wants_quit()) break;
        const auto now = SDL_GetPerformanceCounter();
        const double elapsed = double(now - previous) / double(frequency);
        previous = now;
        const bool active = focused && !minimized;
        const bool wants_relative = active && interface.wants_relative_mouse();
        if (wants_relative != relative) {
            if (SDL_SetRelativeMouseMode(wants_relative ? SDL_TRUE : SDL_FALSE) != 0)
                throw std::runtime_error(std::string("Relative mouse input: ") + SDL_GetError());
            relative = wants_relative;
            input.mouse_x = input.mouse_y = 0;
            screenshot_held = false;
        }
        if (!active) {
            screenshot_held = false;
            media.update(float(elapsed));
            SDL_Delay(10);
            continue;
        }
        bool capture_screen{};
        if (!interface.captures_input()) {
            int count{};
            const Uint8* keys = SDL_GetKeyboardState(&count);
            const std::span<const Uint8> keyboard(keys, std::size_t(count));
            const Uint32 buttons = SDL_GetMouseState(nullptr, nullptr);
            const auto down = [&](std::string_view action) { return interface.action_down(action, keyboard, buttons); };
            input.forward_down = down("forward"); input.backward_down = down("back");
            input.right_down = down("right"); input.left_down = down("left");
            input.fire = down("fire"); input.crouch = down("crouch"); input.walk = down("walk");
            // Gameplay owns each original action's edge, repeat and toggle semantics.
            input.use = down("use"); input.jump = down("jump");
            input.reload = down("reload"); input.change_weapon = down("next_weapon");
            input.drop_weapon = down("drop_weapon"); input.aim = down("aim");
            input.pistol = down("weapon_1"); input.rifle = down("weapon_2");
            const bool screenshot_down = down("screenshot");
            capture_screen = screenshot_down && !screenshot_held;
            screenshot_held = screenshot_down;
        } else screenshot_held = false;
        input.pause = pause;
        refresh_game_size();
        // Completion callbacks may replace scene/UI storage; finish them before borrowing draw data.
        media.update(float(elapsed));
        game.update(float(elapsed), input);
        window.apply(settings);
        media.apply_settings();
        renderer.apply_settings(settings);
        const auto& original_options = interface.graphics_options();
        const bool graphics_changed = original_options != graphics_options;
        renderer.apply_original_options(original_options);
        refresh_game_size();
        if (drawable_width <= 0 || drawable_height <= 0) continue;
        renderer.resize(drawable_width, drawable_height);
        const auto scene = game.frame();
        const bool scene_changed = game.scene_changed();
        const auto revision = interface.resource_revision();
        if (!prepared || scene_changed || graphics_changed || revision != interface_revision) {
            renderer.prepare(scene, interface.draw_data());
            prepared = true;
            interface_revision = revision;
            graphics_options = original_options;
        }
        renderer.render(scene, interface.draw_data());
        if (capture_screen&&(!renderer.vulkan()||renderer.vulkan()->frame_ready())) screenshot(window,renderer.vulkan());
        if (renderer.present()) {
            interface.console_presented(scene.tick_milliseconds);
            renderer.after_present(scene, interface.draw_data());
        }
    }
    if (relative) SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);
    return 0;
}
void capabilities() {
    auto emit=[](std::string_view name,const pusu::VulkanCapabilities& c){
        auto boolean=[](bool value){return value?"true":"false";};
        std::cout<<"{\"name\":"<<json_string(name)<<",\"available\":"<<boolean(c.available);
        if(!c.available)std::cout<<",\"error\":"<<json_string(c.error);
        std::cout<<",\"features\":{\"hardware_ray_tracing\":"<<boolean(c.hardware_ray_tracing)
            <<",\"msaa\":"<<boolean(c.msaa)<<",\"anisotropy\":"<<boolean(c.anisotropy)
            <<",\"bloom\":"<<boolean(c.bloom)<<",\"gamma\":"<<boolean(c.gamma)<<",\"vsync\":"<<boolean(c.vsync)
            <<",\"ray_shadows\":"<<boolean(c.ray_shadows)<<",\"ray_reflections\":"<<boolean(c.ray_reflections)
            <<",\"hdr_tonemapping\":"<<boolean(c.hdr_tonemapping)<<"},\"hardware\":{\"vendor\":"<<json_string(c.vendor)
            <<",\"renderer\":"<<json_string(c.renderer)<<",\"version\":"<<json_string(c.version)
            <<",\"max_msaa\":"<<c.max_msaa<<",\"max_anisotropy\":"<<c.max_anisotropy;
        auto values=[](std::string_view name,const std::vector<int>& values){
            std::cout<<",\""<<name<<"\":[";
            for(std::size_t i=0;i<values.size();++i){if(i)std::cout<<',';std::cout<<values[i];}
            std::cout<<']';
        };
        values("msaa_values",c.msaa_values);values("anisotropy_values",c.anisotropy_values);
        std::cout<<",\"present_modes\":[";
        for(std::size_t i=0;i<c.present_modes.size();++i){if(i)std::cout<<',';std::cout<<json_string(c.present_modes[i]);}
        std::cout<<"]}}";
    };
    std::cout<<"{\"renderers\":[";
    bool first=true;
    for(std::string_view name:{"opengl","vulkan"}) {
        pusu::VulkanCapabilities result;
        try {
            pusu::Settings settings;settings.renderer=name;
            Window window(settings,true);
            result=window.vulkan()?pusu::query_vulkan_capabilities(window.get()):opengl_capabilities(window.get());
        } catch(const std::exception& error){result={};result.error=error.what();}
        if(!first)std::cout<<',';first=false;emit(name,result);
    }
    std::cout<<"]}\n";
}
class Images {
public:
    Images() {
        constexpr int formats = IMG_INIT_JPG | IMG_INIT_PNG;
        const int initialized = IMG_Init(formats);
        if ((initialized & formats) != formats) {
            const std::string error = IMG_GetError();
            IMG_Quit();
            throw std::runtime_error("SDL image decoding: " + error);
        }
    }
    ~Images() { IMG_Quit(); }
    Images(const Images&) = delete;
    Images& operator=(const Images&) = delete;
};
} // namespace

int main(int argc, char** argv) {
    try {
        const auto request = options(argc, argv);
        if (!request.renderer.empty() && request.renderer != "opengl" && request.renderer != "vulkan")
            throw std::runtime_error("Unsupported requested renderer '" + request.renderer +
                                     "'; implemented backends are opengl and vulkan");
        if (request.help) {
            std::cout <<
                "Usage: pusu-game [--data DIR] [--settings FILE] [--renderer opengl|vulkan]\n"
                "       pusu-game --list-renderers\n"
                "       pusu-game --capabilities\n\n"
                "--data DIR        Original game data extracted by the native installer.\n"
                "--settings FILE   Native key=value settings (missing file uses defaults).\n"
                "--list-renderers  Implemented backend names without opening a window.\n"
                "--capabilities    JSON with actual OpenGL/Vulkan display/device capabilities.\n\n"
                "Menu input follows the authored controls. Escape pauses/resumes play.\n"
                "Controls and save/load are available in the original menus.\n"
                "Windows are resizable/high-DPI; fullscreen never switches desktop modes.\n"
                "Vulkan requires a presentable Vulkan 1.2 descriptor-indexing/BDA device.\n"
                "Unsupported saved MSAA, anisotropy, VSync or ray-query requests fail explicitly.\n"
                "Disable ray_tracing explicitly on devices without hardware ray queries.\n"
                "HDR tone mapping outputs SDR, not HDR10; virtual lighting is opt-in.\n"
                "Ray shadows affect only the enabled virtual light, not original baked lighting.\n";
            return 0;
        }
        if (request.list) { std::cout << "opengl\nvulkan\n"; return 0; }
        if (request.capabilities) { capabilities(); return 0; }
        auto settings = pusu::read_settings(request.settings);
        if (!request.renderer.empty()) settings.renderer = request.renderer;
        if (settings.renderer != "opengl" && settings.renderer != "vulkan")
            throw std::runtime_error("Unsupported requested renderer '" + settings.renderer +
                                     "'; implemented backends are opengl and vulkan");
        pusu::AssetStore assets(request.data);
        pusu::MaterialLibrary materials(assets);
        Window window(settings, false);
        Images images;
        return game_loop(window, assets, materials, settings, request.settings);
    } catch (const std::exception& error) {
        std::cerr << "pusu-game: " << error.what() << '\n';
        return 1;
    }
}
