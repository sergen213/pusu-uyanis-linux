#pragma once
#include "assets.hpp"
#include "settings.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace pusu {
class AssetStore;
struct VideoFrame { int width{},height{}; std::span<const std::uint8_t> rgba; double seconds{}; };
struct SoundState {
    std::uint64_t id{};std::string path;float volume{1};bool loop{},spatial{},stream{};Vec3 position{};
    int category{3};float min_distance{400};double seconds{};std::uint32_t owner{};
};
struct MediaSnapshot {
    std::vector<SoundState> sounds;int environment{};
};
// Authored avimap textures loop without claiming an audio channel.
class VideoTexture {
public:
    VideoTexture(const AssetStore&,std::string_view path,std::uint32_t load_clock);
    ~VideoTexture();
    VideoTexture(const VideoTexture&)=delete;
    VideoTexture& operator=(const VideoTexture&)=delete;
    void update_clock(std::uint32_t now);
    VideoFrame frame() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
class Media {
public:
    using SoundId = std::uint64_t;
    Media(const AssetStore&,const Settings&);
    ~Media();
    Media(const Media&)=delete; Media& operator=(const Media&)=delete;
    SoundId sound(std::string_view path,float volume=1,bool loop=false,Vec3 position={},bool spatial=false,int category=3,bool stream=false,std::uint32_t owner=0);
    void stop(SoundId);
    void clear_game_sounds();
    int channel_index(SoundId) const noexcept;
    void preload_sound(std::string_view path,bool stream=false);
    void pause_game(bool paused);
    void volume(SoundId,float);
    void position(SoundId,Vec3);
    void min_distance(SoundId,float);
    void environment(int type);
    void set_random_source(void* context,std::uint32_t (*word)(void*));
    SoundId environment_sound(std::string_view category,std::string_view name,std::string_view object={},float volume=1,Vec3 position={},bool spatial=false,bool apply_environment=true,std::uint32_t owner=0,std::uint32_t* previous_variant=nullptr);
    void music(std::string_view path,bool loop=true);
    void stop_music();
    void listener(Vec3 position,Vec3 forward,Vec3 up);
    void update(float seconds);
    void apply_settings();
    MediaSnapshot snapshot() const;
    void validate_state(const MediaSnapshot&) const;
    void restore_state(const MediaSnapshot&);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace pusu
