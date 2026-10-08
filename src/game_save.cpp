#include "game.hpp"
#include "interface.hpp"
#include "media.hpp"
#include "resources.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/sha.h>
#ifdef PUSU_GAME_SAVE_CHECK
#include <SDL_mixer.h>
#include <cstdlib>
#endif

namespace pusu {
namespace {
constexpr std::size_t save_limit = 128u * 1024u * 1024u;
constexpr std::size_t section_limit = 64u * 1024u * 1024u;
// V4 stores all six original controllers, including paired fades and masters.
// Earlier native layouts are deliberately rejected, not partially restored.
constexpr std::string_view save_magic = "PUSUNSV4";
[[noreturn]] void invalid_save(std::string_view reason) {
    throw std::runtime_error("Invalid native save: " + std::string(reason));
}
void logical_name(std::string_view name, bool empty = false) {
    if (name.empty()) { if (!empty) invalid_save("empty resource name"); return; }
    if (name.size() > 4096 || name.front() == '/' || name.front() == '\\' ||
        name.find(':') != name.npos || name.find('\0') != name.npos)
        invalid_save("unsafe resource name");
    for (std::size_t at = 0; at < name.size();) {
        const auto end = name.find_first_of("/\\", at);
        const auto component = name.substr(at, end == name.npos ? name.size() - at : end - at);
        if (component.empty() || component == "." || component == "..") invalid_save("unsafe resource component");
        if (end == name.npos) break;
        at = end + 1;
        if (at == name.size()) invalid_save("trailing resource separator");
    }
}
std::string identity(std::string_view name) {
    std::string result(name);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
struct SaveWriter {
    std::ostream& stream;
    std::size_t remaining{save_limit};
    void bytes(const char* data, std::size_t count) {
        if (count > remaining) invalid_save("size limit");
        remaining -= count; stream.write(data, static_cast<std::streamsize>(count));
        if (!stream) throw std::runtime_error("Cannot write native save");
    }
    void u8(std::uint8_t value) { const char c = static_cast<char>(value); bytes(&c, 1); }
    void u32(std::uint32_t value) { char b[4]; for (unsigned i=0;i<4;++i) b[i]=static_cast<char>(value>>(8*i));bytes(b,4); }
    void u64(std::uint64_t value) { char b[8]; for (unsigned i=0;i<8;++i) b[i]=static_cast<char>(value>>(8*i));bytes(b,8); }
    void integer(int value) { u32(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(value))); }
    void real(float value) { if (!std::isfinite(value)) invalid_save("nonfinite field");u32(std::bit_cast<std::uint32_t>(value)); }
    void real(double value) { if (!std::isfinite(value)) invalid_save("nonfinite field");u64(std::bit_cast<std::uint64_t>(value)); }
    void flag(bool value) { u8(value ? 1 : 0); }
    void count(std::size_t count, std::size_t maximum=65536) { if(count>maximum) invalid_save("count limit");u32(static_cast<std::uint32_t>(count)); }
    void text(std::string_view value) { count(value.size());bytes(value.data(),value.size()); }
    void vector(Vec3 value) { real(value.x);real(value.y);real(value.z); }
    void matrix(const Matrix& value) { for (const auto word:value) real(word); }
    // Original partial-fread PKA frames can carry authored NaN payloads. Only
    // geometry words use raw IEEE storage; all scalar metadata stays finite.
    void geometry_vector(Vec3 value) { u32(std::bit_cast<std::uint32_t>(value.x));u32(std::bit_cast<std::uint32_t>(value.y));u32(std::bit_cast<std::uint32_t>(value.z)); }
    void geometry_matrix(const Matrix& value) { for(const auto word:value)u32(std::bit_cast<std::uint32_t>(word)); }
    template<class F> void section(F&& write) {
        std::ostringstream output(std::ios::binary);write(output);const auto data=output.view();
        count(data.size(),section_limit);bytes(data.data(),data.size());
    }
};
struct SaveReader {
    std::istream& stream;
    std::size_t remaining;
    void bytes(char* data, std::size_t count) {
        if(count>remaining) invalid_save("truncated field");remaining-=count;
        stream.read(data,static_cast<std::streamsize>(count));if(!stream) invalid_save("truncated stream");
    }
    std::uint8_t u8() { char c{};bytes(&c,1);return static_cast<std::uint8_t>(c); }
    std::uint32_t u32() { char b[4];bytes(b,4);std::uint32_t v{};for(unsigned i=0;i<4;++i)v|=std::uint32_t(static_cast<unsigned char>(b[i]))<<(8*i);return v; }
    std::uint64_t u64() { char b[8];bytes(b,8);std::uint64_t v{};for(unsigned i=0;i<8;++i)v|=std::uint64_t(static_cast<unsigned char>(b[i]))<<(8*i);return v; }
    int integer() { return std::bit_cast<std::int32_t>(u32()); }
    float real() { const auto v=std::bit_cast<float>(u32());if(!std::isfinite(v))invalid_save("nonfinite field");return v; }
    double real64() { const auto v=std::bit_cast<double>(u64());if(!std::isfinite(v))invalid_save("nonfinite field");return v; }
    bool flag() { const auto v=u8();if(v>1)invalid_save("invalid boolean");return v!=0; }
    std::uint32_t count(std::size_t maximum=65536) { const auto v=u32();if(v>maximum)invalid_save("count limit");return v; }
    std::string text(std::size_t maximum=65536) { const auto size=count(maximum);if(size>remaining)invalid_save("string bounds");std::string v(size,'\0');bytes(v.data(),size);return v; }
    std::string name(bool empty=false) { auto result=text(4096);logical_name(result,empty);return result; }
    Vec3 vector() { return {real(),real(),real()}; }
    Matrix matrix() { Matrix value;for(auto& word:value)word=real();return value; }
    Vec3 geometry_vector() { return {std::bit_cast<float>(u32()),std::bit_cast<float>(u32()),std::bit_cast<float>(u32())}; }
    Matrix geometry_matrix() { Matrix value;for(auto& word:value)word=std::bit_cast<float>(u32());return value; }
    template<class F> void section(F&& load) {
        auto data=text(section_limit);std::istringstream input(std::move(data),std::ios::binary);load(input);
        if(input.peek()!=std::char_traits<char>::eof())invalid_save("trailing section bytes");
    }
};
float restored_keyframe_duration(float stored, const Keyframes& clip) {
    const auto duration=.001f*keyframe_playback_duration_ticks(clip);
    if(stored!=duration)invalid_save("keyframe duration");
    return duration;
}
struct MutableMaterialPassSnapshot {
    MaterialRgbGen rgb_gen;
    MaterialTextureKind texture_kind;
    std::string image;
    std::array<std::uint8_t,4> color;
};
using MutableMaterialSnapshot=std::optional<std::vector<MutableMaterialPassSnapshot>>;
void write_mutable_material(SaveWriter& w,const std::optional<Material>& material) {
    w.flag(material.has_value());
    if(!material)return;
    w.count(material->passes.size());
    for(const auto& pass:material->passes) {
        const auto rgb=static_cast<std::uint32_t>(pass.rgb_gen);
        const auto texture=static_cast<std::uint32_t>(pass.texture.kind);
        if(rgb>7 || texture>7)invalid_save("mutable material enum");
        logical_name(pass.texture.image,true);
        w.u32(rgb);w.u32(texture);w.text(pass.texture.image);
        for(const auto channel:pass.color)w.u8(channel);
    }
}
MutableMaterialSnapshot read_mutable_material(SaveReader& r,std::size_t& allocation_budget) {
    if(!r.flag())return std::nullopt;
    const auto count=r.count();
    // Two enum words, a counted empty image, and four channels need 16 bytes.
    if(count>r.remaining/16 || count>allocation_budget/sizeof(MutableMaterialPassSnapshot))
        invalid_save("mutable material allocation bounds");
    allocation_budget-=count*sizeof(MutableMaterialPassSnapshot);
    MutableMaterialSnapshot snapshot(std::in_place);snapshot->reserve(count);
    for(std::uint32_t i=0;i<count;++i) {
        const auto rgb=r.u32(),texture=r.u32();
        if(rgb>7 || texture>7)invalid_save("mutable material enum");
        const auto size=r.count(4096);
        if(size>r.remaining || r.remaining-size<4 || size+1>allocation_budget)invalid_save("mutable material image bounds");
        std::array<char,4096> image;r.bytes(image.data(),size);
        const std::string_view name(image.data(),size);logical_name(name,true);
        allocation_budget-=size+1;
        MutableMaterialPassSnapshot pass{static_cast<MaterialRgbGen>(rgb),static_cast<MaterialTextureKind>(texture),
                                         std::string(name),{}};
        for(auto& channel:pass.color)channel=r.u8();
        snapshot->push_back(std::move(pass));
    }
    return snapshot;
}
void apply_mutable_material(std::optional<Material>& material,MutableMaterialSnapshot&& snapshot) {
    if(!snapshot){material.reset();return;}
    if(!material || material->passes.size()!=snapshot->size())invalid_save("mutable material pass count mismatch");
    for(std::size_t i=0;i<snapshot->size();++i) {
        auto& pass=material->passes[i];auto& saved=(*snapshot)[i];
        pass.rgb_gen=saved.rgb_gen;pass.texture.kind=saved.texture_kind;
        pass.texture.image=std::move(saved.image);pass.color=saved.color;
    }
}
std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    if(!stream) throw std::runtime_error("Cannot open save: "+path.string());
    const auto size=stream.tellg();if(size<0 || static_cast<std::uint64_t>(size)>save_limit)invalid_save("file size limit");
    std::string data(static_cast<std::size_t>(size),'\0');stream.seekg(0);
    stream.read(data.data(),static_cast<std::streamsize>(data.size()));
    if(!stream)invalid_save("file read failure");return data;
}
void atomic_output(const std::filesystem::path& destination,std::string_view bytes,std::string_view trailing=std::string_view()) {
    if(destination.filename().empty())throw std::runtime_error("Save destination has no filename");
    const auto directory=destination.has_parent_path()?destination.parent_path():std::filesystem::path(".");
    std::filesystem::create_directories(directory);
    auto temporary=(directory/(destination.filename().string()+".tmp.XXXXXX")).string();
    const int fd=::mkstemp(temporary.data());
    if(fd<0)throw std::system_error(errno,std::generic_category(),"Create save temporary");
    bool closed=false;
    try {
        for(const auto chunk:{bytes,trailing}) {
            std::size_t at=0;
            while(at<chunk.size()) {
                const auto wrote=::write(fd,chunk.data()+at,chunk.size()-at);
                if(wrote<0 && errno==EINTR)continue;
                if(wrote<=0)throw std::system_error(wrote<0?errno:EIO,std::generic_category(),"Write save temporary");
                at+=static_cast<std::size_t>(wrote);
            }
        }
        if(::fsync(fd)!=0)throw std::system_error(errno,std::generic_category(),"Flush save temporary");
        const int result=::close(fd);closed=true;
        if(result!=0)throw std::system_error(errno,std::generic_category(),"Close save temporary");
        if(::rename(temporary.c_str(),destination.c_str())!=0)throw std::system_error(errno,std::generic_category(),"Publish save");
    } catch(...) { if(!closed)::close(fd);::unlink(temporary.c_str());throw; }
}
std::filesystem::path checkpoint_path(std::string_view name) {
    logical_name(name);
    if(name.find_first_of("/\\")!=name.npos)invalid_save("checkpoint name contains directory");
    return user_data_directory()/"save"/(std::string(name)+".psv");
}
void write_media(SaveWriter& w,const MediaSnapshot& media) {
    w.integer(media.environment);
    w.count(media.sounds.size(),4096);
    for(const auto& sound:media.sounds) {
        w.u64(sound.id);w.text(sound.path);w.real(sound.volume);w.flag(sound.loop);w.flag(sound.spatial);
        w.flag(sound.stream);w.u32(sound.owner);
        w.vector(sound.position);w.integer(sound.category);w.real(sound.min_distance);w.real(sound.seconds);
    }
}
MediaSnapshot read_media(SaveReader& r) {
    MediaSnapshot media;media.environment=r.integer();
    const auto sounds=r.count(4096);media.sounds.reserve(sounds);std::set<std::uint64_t> ids;
    for(std::uint32_t i=0;i<sounds;++i) {
        SoundState sound;sound.id=r.u64();sound.path=r.name();sound.volume=r.real();sound.loop=r.flag();sound.spatial=r.flag();
        sound.stream=r.flag();sound.owner=r.u32();
        sound.position=r.vector();sound.category=r.integer();sound.min_distance=r.real();sound.seconds=r.real64();
        if(!sound.id || !ids.insert(sound.id).second || sound.volume<0 || sound.volume>1 || sound.category<-1 ||
           sound.category>4 || (sound.category==-1 && sound.owner>=90) ||
           sound.min_distance<0 || sound.seconds<0)invalid_save("audio state ranges");
        media.sounds.push_back(std::move(sound));
    }
    if(media.environment<0 || media.environment>2)invalid_save("media state ranges");
    return media;
}
void write_interface(SaveWriter& w,const InterfaceSnapshot& ui) {
    w.text(ui.fullscreen_shader);w.text(ui.subtitle_shader);for(const auto& line:ui.subtitle_lines)w.text(line);
    w.real(ui.fade_duration);w.real(ui.fade_elapsed);w.real(ui.fade_alpha);w.real(ui.subtitle_delay);
    w.u32(ui.subtitle_row);w.u32(ui.subtitle_character);
    w.flag(ui.fullscreen_visible);w.flag(ui.subtitle_visible);w.flag(ui.fade_in);w.flag(ui.fading);
    w.flag(ui.subtitle_row_finished);w.flag(ui.hud_visible);
}
InterfaceSnapshot read_interface(SaveReader& r) {
    InterfaceSnapshot ui;ui.fullscreen_shader=r.name(true);ui.subtitle_shader=r.name(true);for(auto& line:ui.subtitle_lines)line=r.text();
    ui.fade_duration=r.real();ui.fade_elapsed=r.real();ui.fade_alpha=r.real();ui.subtitle_delay=r.real();
    ui.subtitle_row=r.u32();ui.subtitle_character=r.u32();
    ui.fullscreen_visible=r.flag();ui.subtitle_visible=r.flag();ui.fade_in=r.flag();ui.fading=r.flag();
    ui.subtitle_row_finished=r.flag();ui.hud_visible=r.flag();
    if(ui.fade_duration<0 || ui.fade_elapsed<0 || ui.fade_alpha<0 || ui.fade_alpha>1 ||
       ui.subtitle_row>4 || ui.subtitle_character>255 ||
       (ui.subtitle_row<4 && ui.subtitle_character>ui.subtitle_lines[ui.subtitle_row].size()+1))
        invalid_save("interface progress ranges");
    return ui;
}
void validate_envelope(std::istringstream& input) {
    SaveReader r{input,input.view().size()};
    std::array<char,8> magic{};r.bytes(magic.data(),magic.size());
    if(std::string_view(magic.data(),magic.size())!=save_magic)invalid_save("signature/version");
    const auto size=r.count(save_limit-44);std::array<unsigned char,SHA256_DIGEST_LENGTH> expected{},actual{};
    r.bytes(reinterpret_cast<char*>(expected.data()),expected.size());
    if(size!=r.remaining)invalid_save("envelope length");
    const auto payload=input.view().substr(44);
    if(!SHA256(reinterpret_cast<const unsigned char*>(payload.data()),payload.size(),actual.data()))
        throw std::runtime_error("Cannot hash native save payload");
    if(expected!=actual)invalid_save("checksum");
}
}

#ifdef PUSU_GAME_SAVE_CHECK
// Exercises the bounded save codec and its actual authored Game consumers.
void game_save_check(Game& game) {
    const auto require=[](bool condition){if(!condition)throw std::runtime_error("save codec check failed");};
    std::ostringstream output(std::ios::binary);SaveWriter w{output};
    w.u32(0x12345678);w.u64(0x0123456789abcdefULL);w.real(-12.5f);w.real(0.125);
    w.flag(true);w.text(std::string_view("owned\0bytes",11));w.matrix(identity_matrix());
    const auto data=output.str();std::istringstream input(data,std::ios::binary);SaveReader r{input,data.size()};
    require(r.u32()==0x12345678);require(r.u64()==0x0123456789abcdefULL);require(r.real()==-12.5f);
    require(r.real64()==0.125);require(r.flag());require(r.text()==std::string("owned\0bytes",11));
    require(r.matrix()==identity_matrix());require(r.remaining==0);
    const auto payload_nan=std::bit_cast<float>(std::uint32_t{0x7fc0002a});
    std::ostringstream geometry(std::ios::binary);SaveWriter geometry_writer{geometry};
    geometry_writer.geometry_vector({payload_nan,1,2});
    auto geometry_data=std::move(geometry).str();const auto geometry_size=geometry_data.size();
    std::istringstream geometry_input(std::move(geometry_data),std::ios::binary);SaveReader geometry_reader{geometry_input,geometry_size};
    require(std::bit_cast<std::uint32_t>(geometry_reader.geometry_vector().x)==0x7fc0002a);
    const auto rejects=[&](std::string bytes,auto operation){std::istringstream in(bytes,std::ios::binary);SaveReader reader{in,bytes.size()};bool rejected=false;try{operation(reader);}catch(const std::runtime_error&){rejected=true;}require(rejected);};
    for(const auto& clip:{Keyframes{50000,2001,{}},Keyframes{18000,721,{}}}) {
        const auto duration=.001f*keyframe_duration_ticks(clip);
        require(duration==.001f*clip.header_word);
        std::ostringstream saved_duration(std::ios::binary);SaveWriter duration_writer{saved_duration};
        duration_writer.real(duration);
        const auto duration_bytes=saved_duration.str();
        std::istringstream duration_input(duration_bytes,std::ios::binary);SaveReader duration_reader{duration_input,duration_bytes.size()};
        require(restored_keyframe_duration(duration_reader.real(),clip)==duration);
        require(duration_reader.remaining==0);
        std::ostringstream wrong_duration(std::ios::binary);SaveWriter wrong_writer{wrong_duration};
        wrong_writer.real(.001f*clip.frame_count);
        rejects(wrong_duration.str(),[&](auto& reader){restored_keyframe_duration(reader.real(),clip);});
    }
    {
        const Keyframes clip{0,2,{}};
        std::ostringstream saved_duration(std::ios::binary);SaveWriter duration_writer{saved_duration};
        duration_writer.real(0.0f);
        const auto duration_bytes=saved_duration.str();
        std::istringstream duration_input(duration_bytes,std::ios::binary);SaveReader duration_reader{duration_input,duration_bytes.size()};
        bool rejected=false;
        try { (void)restored_keyframe_duration(duration_reader.real(),clip); }
        catch(const std::runtime_error& error) {
            require(std::string_view(error.what())=="Zero-duration PKA cannot be played");
            rejected=true;
        }
        require(rejected);
        require(duration_reader.remaining==0 && clip.header_word==0 && clip.frame_count==2);
    }
    rejects(std::string("\xff\xff\xff\xff",4),[](auto& reader){reader.text();});
    rejects(std::string("\x02",1),[](auto& reader){reader.flag();});
    rejects(std::string("\0\0\x80\x7f",4),[](auto& reader){reader.real();});
    rejects(std::string("\x05\0\0\0ab",6),[](auto& reader){reader.text();});
    for(const auto name:{"../bad","/absolute","x/../../bad","C:\\bad","x//bad","x/"}) {
        bool rejected=false;try{logical_name(name);}catch(const std::runtime_error&){rejected=true;}require(rejected);
    }
    MediaSnapshot media;media.environment=2;
    media.sounds.emplace_back();auto& source=media.sounds.back();
    source.id=7;source.path="sound/shot.ogg";source.volume=0.75f;source.spatial=true;source.position={1,2,3};source.seconds=0.125;
    InterfaceSnapshot ui;ui.subtitle_lines[0]="original bytes";ui.subtitle_delay=-0.025f;
    ui.fade_duration=2;ui.fade_elapsed=0.5f;ui.fade_alpha=0.25f;ui.fading=true;ui.subtitle_character=3;
    std::ostringstream snapshots(std::ios::binary);SaveWriter state_writer{snapshots};
    write_media(state_writer,media);write_interface(state_writer,ui);
    const auto encoded=snapshots.str();std::istringstream states(encoded,std::ios::binary);SaveReader state_reader{states,encoded.size()};
    const auto loaded_media=read_media(state_reader);const auto loaded_ui=read_interface(state_reader);
    require(state_reader.remaining==0);
    std::ostringstream roundtrip(std::ios::binary);SaveWriter roundtrip_writer{roundtrip};
    write_media(roundtrip_writer,loaded_media);write_interface(roundtrip_writer,loaded_ui);
    require(roundtrip.str()==encoded);
    for(std::size_t size=0;size<encoded.size();++size)
        rejects(encoded.substr(0,size),[](auto& reader){read_media(reader);read_interface(reader);});
    std::optional<Material> mutable_material(std::in_place);
    mutable_material->passes.resize(3);
    mutable_material->passes[0].texture.kind=MaterialTextureKind::image;
    mutable_material->passes[0].texture.image="texture/actor.tga";
    mutable_material->passes[1].color={255,255,255,0};
    mutable_material->passes[2].texture.kind=MaterialTextureKind::image;
    mutable_material->passes[2].texture.image="whiteimage";
    mutable_material->passes[2].rgb_gen=MaterialRgbGen::vertex;
    mutable_material->passes[2].color={255,255,255,73};
    const auto material_roundtrip=[&](const std::optional<Material>& source) {
        std::ostringstream encoded_material(std::ios::binary);SaveWriter writer{encoded_material};
        write_mutable_material(writer,source);
        const auto bytes=encoded_material.str();std::istringstream stream(bytes,std::ios::binary);
        SaveReader reader{stream,bytes.size()};std::size_t budget=save_limit;
        auto snapshot=read_mutable_material(reader,budget);require(reader.remaining==0);
        std::optional<Material> restored(std::in_place);
        restored->name="canonical";restored->fog=false;restored->passes.resize(3);
        restored->passes[2].blend=true;restored->passes[2].texture.fallback_image="texture/fallback.tga";
        restored->passes[2].tc_gen=MaterialTcGen::environment;
        apply_mutable_material(restored,std::move(snapshot));
        require(restored.has_value()==source.has_value());
        if(restored) {
            require(restored->name=="canonical" && !restored->fog && restored->passes[2].blend);
            require(restored->passes[2].texture.fallback_image=="texture/fallback.tga");
            require(restored->passes[2].tc_gen==MaterialTcGen::environment);
            for(std::size_t i=0;i<3;++i) {
                require(restored->passes[i].color==source->passes[i].color);
                require(restored->passes[i].rgb_gen==source->passes[i].rgb_gen);
                require(restored->passes[i].texture.kind==source->passes[i].texture.kind);
                require(restored->passes[i].texture.image==source->passes[i].texture.image);
            }
        }
        std::ostringstream repeated(std::ios::binary);SaveWriter repeated_writer{repeated};
        write_mutable_material(repeated_writer,restored);require(repeated.str()==bytes);
        for(std::size_t size=0;size<bytes.size();++size)
            rejects(bytes.substr(0,size),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
        if(source) {
            std::istringstream mismatch_stream(bytes,std::ios::binary);SaveReader mismatch_reader{mismatch_stream,bytes.size()};
            std::size_t mismatch_budget=save_limit;auto mismatch=read_mutable_material(mismatch_reader,mismatch_budget);
            restored->passes.pop_back();bool rejected=false;
            try{apply_mutable_material(restored,std::move(mismatch));}catch(const std::runtime_error&){rejected=true;}
            require(rejected);
        }
        return bytes;
    };
    // Ordinary damage does not feed this codec: alpha zero is independent of HP.
    const auto material_bytes=material_roundtrip(mutable_material);
    mutable_material->passes[1].color[3]=160;
    mutable_material->passes[2].texture.image="yellowimage";
    mutable_material->passes[2].rgb_gen=MaterialRgbGen::constant;
    mutable_material->passes[2].color={200,200,200,73};
    material_roundtrip(mutable_material);
    mutable_material->passes[2].texture.image="whiteimage";
    mutable_material->passes[2].rgb_gen=MaterialRgbGen::vertex;
    mutable_material->passes[2].color={255,255,255,73};
    material_roundtrip(mutable_material);
    mutable_material->passes[0].rgb_gen=MaterialRgbGen::lighting_diffuse;
    mutable_material->passes[0].texture.kind=MaterialTextureKind::avi;
    material_roundtrip(mutable_material);
    material_roundtrip(std::nullopt);
    for(const auto offset:{5u,9u}) {
        auto invalid_enum=material_bytes;invalid_enum[offset]=8;
        rejects(std::move(invalid_enum),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
    }
    for(const auto image:{"../bad","/absolute","x/../../bad","C:\\bad","x//bad","x/","x\\..\\bad"}) {
        std::ostringstream unsafe(std::ios::binary);SaveWriter writer{unsafe};
        writer.flag(true);writer.count(1);writer.u32(0);writer.u32(1);writer.text(image);
        for(unsigned i=0;i<4;++i)writer.u8(255);
        rejects(unsafe.str(),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
    }
    auto oversized_image=material_bytes;oversized_image[13]=1;oversized_image[14]=16;
    rejects(std::move(oversized_image),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
    auto embedded_null=material_bytes;embedded_null[17]='\0';
    rejects(std::move(embedded_null),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
    rejects(std::string("\x02",1),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
    rejects(std::string("\x01\xff\xff\xff\xff",5),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
    rejects(std::string("\x01\x01\0\0\0",5),[](auto& reader){std::size_t budget=save_limit;read_mutable_material(reader,budget);});
    rejects(material_bytes,[](auto& reader){std::size_t budget=0;read_mutable_material(reader,budget);});
    rejects(material_bytes,[](auto& reader){std::size_t budget=3*sizeof(MutableMaterialPassSnapshot);read_mutable_material(reader,budget);});
    std::array<unsigned char,SHA256_DIGEST_LENGTH> checksum{};
    if(!SHA256(reinterpret_cast<const unsigned char*>(encoded.data()),encoded.size(),checksum.data()))
        throw std::runtime_error("Cannot hash native save payload");
    std::ostringstream wrapped(std::ios::binary);SaveWriter envelope_writer{wrapped};
    envelope_writer.bytes(save_magic.data(),save_magic.size());envelope_writer.count(encoded.size(),save_limit-44);
    envelope_writer.bytes(reinterpret_cast<const char*>(checksum.data()),checksum.size());envelope_writer.bytes(encoded.data(),encoded.size());
    const auto complete=wrapped.str();std::istringstream valid(complete,std::ios::binary);validate_envelope(valid);
    auto previous_version=complete;
    previous_version.replace(0,8,"PUSUNSV3");
    std::istringstream previous_input(previous_version,std::ios::binary);
    bool previous_rejected=false;
    try { validate_envelope(previous_input); }
    catch(const std::runtime_error& error) {
        require(std::string_view(error.what())=="Invalid native save: signature/version");
        previous_rejected=true;
    }
    require(previous_rejected);
    const auto corrupt=[&](std::string value) {
        bool rejected=false;std::istringstream stream(std::move(value),std::ios::binary);
        try {validate_envelope(stream);}catch(const std::runtime_error&){rejected=true;}
        require(rejected);
    };
    for(std::size_t size=0;size<44;++size)corrupt(complete.substr(0,size));
    auto damaged=complete;damaged.back()^=1;corrupt(std::move(damaged));
    damaged=complete;damaged[8]^=1;corrupt(std::move(damaged));
    damaged=complete;damaged[0]^=1;corrupt(std::move(damaged));
    corrupt(complete+"extra");
    game.boot();
    game.new_game();
    game.update(0,{});
    require(game.level_ && game.collision_ && game.level_name_=="surlar_1");
    auto* player=game.actors_.find(game.player_);
    require(player && player==game.actors_.player());
    // Authored blocked startup binds the existing raw main-camera frame; it
    // must not run normal distance/ray/shake placement to manufacture a frame.
    require(game.camera_blocked_ && game.main_camera_ && !game.selected_camera_);
    // Only normal placement writes this ray; raw camera binding leaves it alone.
    require(player->player_camera.ray_origin.x==0 &&
            player->player_camera.ray_origin.y==0 && player->player_camera.ray_origin.z==0);
    require(game.camera_.frame0==game.entity(game.main_camera_).orientation);
    require(player->player_camera.world==game.camera_.frame0);
    const auto call=[&](std::string_view name) {
        require(game.runtime_.contains(name));
        game.runtime_.invoke(name);
    };
    call("exit_cinematic");
    call("enable_ingame_processing");
    call("enable_ingame_inputprocessing");
    player->combat_state=0;
    game.action({"oyuna_devam",{},-1});
    require(!game.menu_ && !game.interface_.captures_input());
    const auto gameplay_sound=game.media_.sound("sound/menu_music.ogg",1,true,{},false,3);
    const auto gameplay_channel=game.media_.channel_index(gameplay_sound);
    require(gameplay_channel>=0 && gameplay_channel!=1 && !Mix_Paused(gameplay_channel));
    const auto position_address=game.host_position_address(game.player_);
    const auto alias_word=game.runtime_.read_word(position_address);
    const auto actor_state=[&] {
        std::ostringstream state(std::ios::binary);
        game.actors_.save(state);
        return state.str();
    };
    const auto preserved_actors=actor_state();
    const auto stack_size=game.runtime_.stack_size();
    game.runtime_.push_word(0x1234abcd);
    GameInput release;release.pause=true;
    const auto clock=game.clock_;
    game.update(.05f,release);
    require(game.menu_ && game.interface_.captures_input() && !game.interface_.wants_relative_mouse());
    require(game.clock_==clock && actor_state()==preserved_actors);
    require(game.runtime_.stack_size()==stack_size+1 && game.runtime_.pop_word()==0x1234abcd);
    require(game.runtime_.read_word(position_address)==alias_word);
    require(Mix_Paused(gameplay_channel) && Mix_Playing(1) && !Mix_Paused(1));
    game.update(.05f,release); // An already-visible menu is exclusively Interface-owned.
    require(game.menu_ && game.interface_.captures_input() && game.clock_==clock);
    require(actor_state()==preserved_actors && game.runtime_.read_word(position_address)==alias_word);
    SDL_Event escape{};escape.type=SDL_KEYUP;escape.key.keysym.scancode=SDL_SCANCODE_ESCAPE;
    require(game.interface_.input(escape,{0,0},false)); // Real authored #RESUME action -> Game::action.
    require(!game.menu_ && !game.interface_.captures_input() && game.interface_.wants_relative_mouse());
    require(!Mix_Paused(gameplay_channel) && Mix_Paused(1));
    game.update(.001f,{});
    require(game.clock_>clock);
    // The unblocked public update uses the original 0041eed0 placement, not
    // an initialization-only bypass or a camera-axis correction.
    require(!game.camera_blocked_ && !game.selected_camera_);
    auto placed_camera=player->orientation;
    const auto& camera_state=player->player_camera;
    if(camera_state.reverse)rotate_local(placed_camera,{0,180,0});
    placed_camera[14]+=70;
    rotate_local(placed_camera,{camera_state.pitch,0,0});
    const auto camera_distance=camera_state.special?0.f:camera_state.distance;
    placed_camera[12]+=placed_camera[8]*camera_distance;
    placed_camera[13]+=placed_camera[9]*camera_distance;
    placed_camera[14]+=placed_camera[10]*camera_distance;
    require(player->player_camera.world==placed_camera);
    require(game.camera_.frame0==placed_camera &&
            game.entity(game.main_camera_).orientation==placed_camera);
    const auto pause=[&](bool expected) {
        game.update(0,release);
        require(game.menu_==expected && game.interface_.captures_input()==expected);
    };
    pause(true); // A second physical release needs no synthetic pause=false update.
    game.action({"oyuna_devam",{},-1});
    pause(true);
    game.action({"oyuna_devam",{},-1});
    call("disable_ingame_inputprocessing");
    player->combat_state=0;pause(false);
    player->combat_state=4;pause(false);
    call("disable_ingame_inputprocessing_except_use");pause(false);
    player->combat_state=5;pause(true);
    game.action({"oyuna_devam",{},-1});
    call("disable_ingame_processing");pause(false); // State5 never overrides processing-off.
    call("enable_ingame_inputprocessing");pause(false);
    call("enable_ingame_processing");
    player->combat_state=0;
    call("enter_cinematic");
    require(game.cinematic_ && game.camera_blocked_);
    pause(true); // Cinematic/camera ownership is not an Escape eligibility predicate.
    game.interface_.show_menu("ayarlar_ses");
    require(game.interface_.input(escape,{0,0},false));
    require(game.menu_ && game.interface_.captures_input());
    require(game.interface_.input(escape,{0,0},false));
    require(!game.menu_ && !game.interface_.captures_input());
    call("exit_cinematic");
    game.media_.stop(gameplay_sound);

    const auto close_matrix=[&](const Matrix& actual,const Matrix& expected) {
        for(std::size_t i=0;i<actual.size();++i)
            require(std::abs(actual[i]-expected[i])<.00001f);
    };
    // Cardinal matrices independently pin the registered hosts' yawY/rollX/pitchZ ABI.
    constexpr Matrix yaw90{0,0,-1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1};
    constexpr Matrix roll90{1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1};
    constexpr Matrix pitch90{0,1,0,0, -1,0,0,0, 0,0,1,0, 0,0,0,1};
    constexpr std::array<std::string_view,6> rotate_hosts{
        "entity_rotate_yaw","entity_rotate_roll","entity_rotate_pitch",
        "entity_rotate_yaw_world","entity_rotate_roll_world","entity_rotate_pitch_world"};
    const std::array<Matrix,6> rotations{
        yaw90,roll90,pitch90,yaw90,inverse_rigid(roll90),inverse_rigid(pitch90)};
    const auto position=game.host_position(game.player_);
    for(std::size_t i=0;i<rotate_hosts.size();++i) {
        player->orientation=original_actor_basis;
        player->orientation[12]=position.x;player->orientation[13]=position.y;player->orientation[14]=position.z;
        game.entity(game.player_).orientation=identity_matrix(); // Deliberately stale scene mirror.
        auto expected=multiply(original_actor_basis,rotations[i]);
        expected[12]=position.x;expected[13]=position.y;expected[14]=position.z;
        game.runtime_.push_float(90);game.runtime_.push_word(game.player_);
        call(rotate_hosts[i]);
        close_matrix(player->orientation,expected);
        close_matrix(game.entity(game.player_).orientation,expected);
        require(game.runtime_.stack_size()==stack_size);
    }
    player->orientation=original_actor_basis;
    player->orientation[12]=position.x;player->orientation[13]=position.y;player->orientation[14]=position.z;
    game.runtime_.push_float(270);game.runtime_.push_word(game.player_);
    call("entity_rotate_yaw_world");
    auto yaw270=multiply(original_actor_basis,inverse_rigid(yaw90));
    yaw270[12]=position.x;yaw270[13]=position.y;yaw270[14]=position.z;
    close_matrix(game.actors_.frame(game.player_,0),yaw270);
    require(std::abs(player->orientation[4])<.00001f &&
            std::abs(player->orientation[5])<.00001f &&
            std::abs(player->orientation[6]-1)<.00001f);
    // Use the real position setter so render interpolation has coherent cached endpoints.
    game.host_set_position(game.player_,position+Vec3{0,0,1});
    game.entity(game.player_).orientation=identity_matrix();
    const auto live_frame=game.actors_.frame(game.player_,0);
    const auto player_name=game.entity(game.player_).name;
    game.host_keyframe("anm_first_anim",0,1,player_name);
    const auto& bound=game.keyframe_playbacks_.back();
    require(bound.clip && !bound.targets.empty() && bound.targets.front()==game.player_);
    close_matrix(bound.target_base.front(),live_frame);
    require(bound.target_base.front()!=game.entity(game.player_).orientation);
    const auto expected_sample=evaluate_keyframe_track(bound.clip->tracks[bound.target_tracks.front()],
        0,keyframe_playback_duration_ticks(*bound.clip),false);
    const auto expected_frame=multiply(live_frame,expected_sample);
    game.update_animations(0);
    close_matrix(player->orientation,expected_frame);
    close_matrix(game.entity(game.player_).orientation,expected_frame);

    // The authored surlar_1 include scn_mo5_a_12 supplies this real prop; the
    // unmodified LPG clip repeats its target name with four distinct moving tracks.
    call("disable_ingame_processing");
    // Original 00451123..0045113a defaults PA lookup to the loaded object name;
    // actual fatih has no anim_set, while authored bots explicitly override it.
    const auto& player_model=game.entity(game.player_).model;
    require(player_model.definition && !player_model.definition->animation_set);
    require(identity(player_model.name)=="fatih");
    const auto override_bot=std::find_if(game.entities_.begin(),game.entities_.end(),[](const auto& e) {
        return e.alive && e.kind==1 && e.model.definition && e.model.definition->animation_set &&
               identity(*e.model.definition->animation_set)!=identity(e.model.name);
    });
    require(override_bot!=game.entities_.end());
    const std::array animation_targets{game.player_,override_bot->handle};
    // Script state7 deliberately retains explicit host layers; normal states
    // are exercised separately below through the render-time producer.
    for(const auto target:animation_targets)game.actors_.find(target)->combat_state=7;
    constexpr std::string_view relative_clip="common/motionless";
    for(const auto animation_handle:animation_targets) {
        const auto& model=game.entity(animation_handle).model;
        const auto& prefix=model.definition->animation_set?*model.definition->animation_set:model.name;
        const auto path="object/anim/pa/"+identity(prefix)+"/"+std::string(relative_clip)+".pa";
        const auto authored=read_animation(game.assets_.path(path));
        require(authored.frame_count>1 && authored.header_word && !authored.tracks.empty());
        const auto duration=static_cast<double>(authored.header_word)*(authored.frame_count-1);
        require(game.animation_duration(animation_handle,relative_clip)==duration && duration>0);
        game.host_character_layer(animation_handle,relative_clip,true,4,1,true);
        const auto playback=std::find_if(game.character_playbacks_.begin(),game.character_playbacks_.end(),
            [&](const auto& p){return p.entity==animation_handle && p.channel==4;});
        require(playback!=game.character_playbacks_.end() && playback->active && playback->clip==path);
        require(!model.parts.empty() && model.parts.front().layer_evaluators[4]);
        require(model.parts.front().layer_animations[4]==&game.animations_.at(path));
    }
    const auto evaluated_character_pose=[&](bool rendered=true) {
        for(const auto animation_handle:animation_targets) {
            const auto& part=game.entity(animation_handle).model.parts.front();
            require(part.mesh && !part.mesh->bones.empty());
            // Independently read every active authored PA so the real public
            // update's combined pose is checked without discarding other layers.
            std::array<std::optional<Animation>,6> clips;
            std::array<std::optional<PoseEvaluator>,6> bindings;
            std::array<AnimationLayer,6> layers{};
            for(const auto& p:game.character_playbacks_) {
                if(!p.active || p.entity!=animation_handle)continue;
                const auto channel=static_cast<std::size_t>(p.channel);
                require(channel<layers.size());
                clips[channel]=read_animation(game.assets_.path(p.clip));
                bindings[channel].emplace(*part.mesh,&*clips[channel]);
                require(p.elapsed>=0);
                layers[channel]={&*bindings[channel],p.elapsed,p.blend,false,p.animation_mode==4?4u:1u};
            }
            std::vector<Matrix> expected(part.mesh->bones.size());
            std::vector<BoneFrame> locals(expected.size());
            std::vector<std::uint8_t> touched(expected.size());
            evaluate_layered_pose(*part.mesh,layers,expected,locals,touched);
            require(std::any_of(touched.begin(),touched.end(),[](auto value){return value!=0;}));
            require(part.pose.size()==expected.size());
            for(std::size_t bone=0;bone<expected.size();++bone)
                for(std::size_t word=0;word<expected[bone].size();++word)
                    require(std::bit_cast<std::uint32_t>(part.pose[bone][word])==
                            std::bit_cast<std::uint32_t>(expected[bone][word]));
            if(rendered) {
                const auto scene=game.frame();
                const auto draw=std::find_if(scene.objects.begin(),scene.objects.end(),[&](const auto& object) {
                    return object.lighting_id==part.lighting_id;
                });
                require(draw!=scene.objects.end() && draw->bones.size()==expected.size());
                const auto world=game.actors_.frame(animation_handle,1);
                close_matrix(draw->transform,world);
                for(std::size_t bone=0;bone<expected.size();++bone) {
                    const auto actual_world=multiply(draw->transform,draw->bones[bone]);
                    const auto expected_world=multiply(world,expected[bone]);
                    for(std::size_t word=0;word<expected_world.size();++word)
                        require(std::bit_cast<std::uint32_t>(actual_world[word])==
                                std::bit_cast<std::uint32_t>(expected_world[word]));
                }
            }
        }
    };
    const auto older_index=game.keyframe_playbacks_.size();
    game.host_keyframe("anm_lpg_truck_explode",0,1,{});
    const auto& older=game.keyframe_playbacks_[older_index];
    const auto target=std::find_if(older.targets.begin(),older.targets.end(),[&](auto handle) {
        return game.entity(handle).name=="obj_propan_tube_03";
    });
    require(target!=older.targets.end());
    const auto handle=*target;
    const auto first=static_cast<std::size_t>(target-older.targets.begin());
    auto last=first;
    for(std::size_t i=first+1;i<older.targets.size();++i)
        if(older.targets[i]==handle)last=i;
    require(last!=first && older.target_tracks[first]<older.target_tracks[last]);
    const auto sampled_frame=[&](const Game::KeyframePlayback& playback,std::size_t binding) {
        const auto track=playback.target_tracks[binding];
        const auto sample=evaluate_keyframe_track(playback.clip->tracks[track],
            game.game_tick_-playback.start_tick,keyframe_playback_duration_ticks(*playback.clip),false);
        return multiply(playback.target_base[binding],sample);
    };
    const auto different=[&](const Matrix& a,const Matrix& b) {
        for(std::size_t i=0;i<a.size();++i)
            if(std::abs(a[i]-b[i])>.001f)return true;
        return false;
    };
    game.update(.025f,{});
    evaluated_character_pose(); // Real public update advances and evaluates both resolver branches.
    const auto inner_expected=sampled_frame(game.keyframe_playbacks_[older_index],first);
    require(different(inner_expected,sampled_frame(game.keyframe_playbacks_[older_index],last)));
    close_matrix(game.entity(handle).orientation,inner_expected); // Lower encounter wins: bindings prepend.
    const auto newer_index=game.keyframe_playbacks_.size();
    game.host_keyframe("anm_lpg_truck_explode",0,1,{});
    const auto& newer=game.keyframe_playbacks_[newer_index];
    const auto newer_target=std::find(newer.targets.begin(),newer.targets.end(),handle);
    require(newer_target!=newer.targets.end());
    const auto newer_first=static_cast<std::size_t>(newer_target-newer.targets.begin());
    game.update(.025f,{});
    const auto outer_expected=sampled_frame(game.keyframe_playbacks_[older_index],first);
    require(different(outer_expected,sampled_frame(game.keyframe_playbacks_[newer_index],newer_first)));
    close_matrix(game.entity(handle).orientation,outer_expected); // Newest playback runs first; older wins.

    // Round-trip the real post-prerender scene after time and authored LPG
    // playbacks have advanced, not merely the individual component codecs.
    require(!game.prerender_pending_ && game.time_>0 && !game.entities_.empty());
    auto pattern=(std::filesystem::temp_directory_path()/"pusu-game-save-XXXXXX").string();
    const auto directory=::mkdtemp(pattern.data());
    if(!directory)throw std::system_error(errno,std::generic_category(),"Create save check directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}
    } cleanup{directory};
    const auto save_path=cleanup.path/"scene.psv";
    // Original 00435ed0 -> 00414fa0 runs even for a stationary actor. Keep
    // simulation disabled, but exercise its actual public render/update path.
    for(auto& playback:game.keyframe_playbacks_)
        if(std::find(playback.targets.begin(),playback.targets.end(),game.player_)!=playback.targets.end())
            playback.active=false;
    player=game.actors_.find(game.player_);
    player->combat_state=0;player->previous_combat_state=0;
    player->weapon_slots[0]=WeaponKind::magnum;player->selected_slot=0;
    player->weapon=WeaponKind::magnum;player->no_weapon=false;
    player->movement_speed=0;player->input_gas=false;
    player->movement_command_flags={};player->movement_walk=false;
    player->posture_state=0;player->airborne=false;player->jumping=false;
    player->orientation=original_actor_basis;
    player->orientation[12]=player->position.x;
    player->orientation[13]=player->position.y;
    player->orientation[14]=player->position.z;
    const auto slot=[&](int channel)->const Game::CharacterPlayback& {
        const auto found=std::find_if(game.character_playbacks_.begin(),game.character_playbacks_.end(),[&](const auto& p) {
            return p.entity==game.player_ && p.channel==channel;
        });
        require(found!=game.character_playbacks_.end());
        return *found;
    };
    const auto active_clip=[&](int pair,std::string_view relative)->const Game::CharacterPlayback& {
        const auto expected="object/anim/pa/fatih/"+std::string(relative)+".pa";
        const auto found=std::find_if(game.character_playbacks_.begin(),game.character_playbacks_.end(),[&](const auto& p) {
            return p.entity==game.player_ && p.channel>=pair && p.channel<pair+2 &&
                   p.active && identity(p.clip)==expected;
        });
        require(found!=game.character_playbacks_.end());
        require(!read_animation(game.assets_.path(expected)).tracks.empty());
        return *found;
    };
    const auto same_controllers=[&](const auto& expected) {
        require(expected.size()==game.character_playbacks_.size());
        for(std::size_t index=0;index<expected.size();++index) {
            const auto& a=expected[index];const auto& b=game.character_playbacks_[index];
            require(a.entity==b.entity && a.channel==b.channel && a.clip==b.clip && a.master_clip==b.master_clip);
            require(a.elapsed==b.elapsed && a.loop==b.loop && a.blend==b.blend && a.active==b.active);
            require(a.start_tick==b.start_tick && a.animation_mode==b.animation_mode && a.flags==b.flags);
            require(a.pause_tick==b.pause_tick && a.sample_tick==b.sample_tick && a.fade_tick==b.fade_tick);
            require(a.master_channel==b.master_channel && a.coefficient==b.coefficient);
        }
    };
    const auto replace_character_mesh=[&](std::string_view resource) {
        const auto controllers=game.character_playbacks_;
        const auto tick=game.game_tick_;const auto clock=game.clock_;
        game.runtime_.push_string(resource);game.runtime_.push_word(game.player_);
        call("entity_set_mesh"); // Registered original VM host, not a private cache reset.
        require(game.runtime_.stack_size()==stack_size);
        require(game.game_tick_==tick && game.clock_==clock);
        same_controllers(controllers);
        evaluated_character_pose(false); // Rebuilt Part palette is correct synchronously.
        game.update(0,{});
        same_controllers(controllers);
        evaluated_character_pose(); // Newly borrowed renderer bones/world transforms are correct.
    };
    game.update(0,{});
    require(active_clip(0,"leg/idle").animation_mode==2);
    const auto torso_idle=game.assets_.contains("object/anim/pa/fatih/pistol/idle.pa")?
        "pistol/idle":"no_weapon/idle";
    require(active_clip(2,torso_idle).animation_mode==2);
    game.update(.2f,{});evaluated_character_pose();
    const auto idle_slot=active_clip(0,"leg/idle").channel;
    player->movement_speed=player->walk_speed;player->input_gas=true;player->movement_walk=true;
    game.update(0,{});game.update(.025f,{});
    const auto& walking=active_clip(0,"leg/walk");
    require(walking.channel!=idle_slot && (walking.flags&8u) && (slot(idle_slot).flags&4u));
    require(std::abs(walking.blend-.15f)<.00001f &&
            std::abs(slot(idle_slot).blend-.85f)<.00001f);
    const auto torso_walk=game.assets_.contains("object/anim/pa/fatih/pistol/walk.pa")?
        "pistol/walk":"no_weapon/walk";
    const auto& upper_walk=active_clip(2,torso_walk);
    require(upper_walk.flags&8u);
    const auto& upper_old=slot(upper_walk.channel==2?3:2);
    require(upper_old.active && (upper_old.flags&4u));
    require(std::abs(upper_walk.blend-.15f)<.00001f &&
            std::abs(upper_old.blend-.85f)<.00001f);
    const auto paired_path=cleanup.path/"paired.psv";
    const auto paired_controllers=game.character_playbacks_;
    game.save(paired_path);game.load(paired_path);
    same_controllers(paired_controllers);evaluated_character_pose();
    replace_character_mesh(game.entity(game.player_).object_name);
    player=game.actors_.find(game.player_);
    game.update(.2f,{});
    player->movement_speed=0;player->input_gas=false;player->movement_walk=false;
    player->posture_state=3;player->fully_crouched=true;
    game.update(0,{});game.update(.2f,{});
    require(active_clip(0,"leg/crouch_idle").animation_mode==2);
    const auto crouch_idle=game.assets_.contains("object/anim/pa/fatih/pistol/crouch_idle.pa")?
        "pistol/crouch_idle":"no_weapon/crouch_idle";
    require(active_clip(2,crouch_idle).animation_mode==2);
    evaluated_character_pose();

    // Original13830's nonnull final argument hard-switches the torso master;
    // do not invent a torso crossfade. The leg pair still fades while both
    // actual aim slots depend on the live mode4 master.
    player->combat_state=3;player->shoot_flag=0;player->combat_time=game.game_tick_;
    game.update(0,{});game.update(.025f,{});
    require(active_clip(0,"leg/crouch_still").flags&8u);
    const auto& master=active_clip(2,"common/motionless");
    require(master.animation_mode==4 && master.blend==1);
    require(slot(0).active && slot(1).active && slot(4).active && slot(5).active);
    require(!slot(master.channel==2?3:2).active);
    require(identity(slot(4).clip)=="object/anim/pa/fatih/pistol/crouch_aim_forward.pa");
    require(identity(slot(5).clip)=="object/anim/pa/fatih/pistol/crouch_aim_up.pa");
    require(slot(4).coefficient==1 && slot(5).coefficient==0);
    for(const auto channel:{4,5}) {
        const auto& aim=slot(channel);
        require((aim.flags&16u) && aim.master_channel==master.channel && aim.master_clip==master.clip);
        require(aim.blend==master.blend*aim.coefficient);
    }
    evaluated_character_pose();
    const auto transition_path=cleanup.path/"transition.psv";
    const auto midfade_controllers=game.character_playbacks_;
    game.save(transition_path);game.load(transition_path);
    same_controllers(midfade_controllers);
    evaluated_character_pose();
    const auto replacement=std::find_if(game.entities_.begin(),game.entities_.end(),[&](const auto& e) {
        return e.alive && e.kind==1 && e.model.definition && e.model.definition->animation_set &&
            identity(*e.model.definition->animation_set)=="fatih" && !e.model.parts.empty() &&
            e.model.parts.front().mesh!=game.entity(game.player_).model.parts.front().mesh;
    });
    require(replacement!=game.entities_.end());
    replace_character_mesh(replacement->object_name); // Authored shared PA set, different receiving PM.
    game.update(.025f,{});
    const auto continued_controllers=game.character_playbacks_;
    game.load(transition_path);game.update(.025f,{});
    same_controllers(continued_controllers);evaluated_character_pose();
    player=game.actors_.find(game.player_);
    for(const auto& horizontal:{original_actor_basis,multiply(pitch90,original_actor_basis),
                               multiply(inverse_rigid(pitch90),original_actor_basis)}) {
        player->orientation=horizontal;
        player->orientation[12]=player->position.x;
        player->orientation[13]=player->position.y;
        player->orientation[14]=player->position.z;
        game.update(0,{});
        require(player->orientation[10]==0 && slot(4).coefficient==1 && slot(5).coefficient==0);
        evaluated_character_pose();
    }
    player->combat_state=0;game.update(0,{});
    if(slot(4).active || slot(5).active) {
        const auto& outgoing_master=slot(slot(4).master_channel);
        require(outgoing_master.active && (outgoing_master.flags&4u));
    }
    // Leaving aim changed the master; the expired dependencies cannot revive.
    game.update(.2f,{});
    require(!slot(4).active && !slot(5).active);
    evaluated_character_pose();
    game.load(transition_path);
    game.actors_.find(game.player_)->combat_state=7; // Original script exemption preserves controller pause.
    ActorEvent pause_event;pause_event.kind=ActorEvent::Kind::animation;
    pause_event.entity=game.player_;pause_event.animation_channel=0;
    pause_event.animation_operation=ActorEvent::AnimationOperation::pause_controller;
    game.character_animation_event(pause_event);
    require((slot(0).flags&3u)==2 && slot(0).pause_tick==game.game_tick_);
    const auto paused_path=cleanup.path/"paused.psv";
    const auto paused_controllers=game.character_playbacks_;
    game.save(paused_path);game.load(paused_path);same_controllers(paused_controllers);
    const auto paused_phase=slot(0).sample_tick;
    game.update(.025f,{});require(slot(0).sample_tick==paused_phase);
    const auto paused_continuation=game.character_playbacks_;
    game.load(paused_path);game.update(.025f,{});
    same_controllers(paused_continuation);evaluated_character_pose();

    // A synchronous original host can change a master before advancement.
    // Saving that precise state is valid; restoration must not retie its aims.
    game.load(transition_path);
    game.actors_.find(game.player_)->combat_state=7;
    const auto stale_master_channel=slot(5).master_channel;
    game.host_character_layer(game.player_,torso_idle,true,stale_master_channel,1,true);
    const auto stale_path=cleanup.path/"stale-master.psv";
    const auto stale_controllers=game.character_playbacks_;
    game.save(stale_path);game.load(stale_path);same_controllers(stale_controllers);
    require(slot(4).active && slot(5).active);
    game.update(0,{});
    require(!slot(4).active && !slot(5).active && slot(4).flags==3 && slot(5).flags==3);
    game.load(transition_path);
    game.actors_.hurt(game.player_,100000);
    game.update(0,{});
    require(game.actors_.find(game.player_)->combat_state==4 ||
            game.actors_.find(game.player_)->combat_state==5);
    require(!slot(4).active && !slot(5).active && slot(4).flags==3 && slot(5).flags==3);
    for(const auto& p:game.character_playbacks_) {
        if(p.entity!=game.player_ || !p.active)continue;
        require(p.channel<4 && p.clip.find("/common/die/")!=std::string::npos &&
                p.clip.find("dying_")!=std::string::npos);
    }
    evaluated_character_pose();
    const auto corpse_path=cleanup.path/"corpse.psv";
    game.save(corpse_path);game.load(corpse_path);
    require(game.actors_.find(game.player_)->combat_state==4 ||
            game.actors_.find(game.player_)->combat_state==5);
    const auto& corpse=game.entity(game.player_);
    require(game.leaf_entities_.size()==game.level_->leaves.size()+1);
    require(!corpse.membership_leaves.empty());
    for(const auto leaf:corpse.membership_leaves) {
        require(leaf<game.leaf_entities_.size());
        const auto& members=game.leaf_entities_[leaf];
        require(std::count(members.begin(),members.end(),corpse.handle)==1);
    }
    game.load(transition_path); // Saves six bound slots, including the outgoing reset torso member.
    same_controllers(midfade_controllers);
    const auto saved_actors=actor_state();
    const auto saved_sounds=game.sounds_;
    const auto inactive_sound=std::find_if(saved_sounds.begin(),saved_sounds.end(),[&](const auto& sound) {
        return sound.channels.empty() && !game.assets_.contains(sound.path);
    });
    require(inactive_sound!=saved_sounds.end()); // Real authored surlar_1 music source, not a fixture alias.
    const auto preserved_sources=[&] {
        require(game.sounds_.size()==saved_sounds.size());
        for(std::size_t i=0;i<saved_sounds.size();++i) {
            require(game.sounds_[i].name==saved_sounds[i].name && game.sounds_[i].path==saved_sounds[i].path);
            require(game.sounds_[i].channels==saved_sounds[i].channels);
        }
    };
    const auto saved_time=game.time_,saved_clock=game.clock_;
    const auto saved_tick=game.game_tick_;
    const auto saved_entities=game.entities_.size();
    const auto saved_playbacks=game.keyframe_playbacks_.size();
    const auto saved_frame=game.entity(handle).orientation;
    const auto saved_generation=game.frame().generation;
    game.save(save_path);
    const auto saved_bytes=file_bytes(save_path);
    game.load(save_path);
    require(actor_state()==saved_actors && game.time_==saved_time && game.clock_==saved_clock);
    preserved_sources();
    require(game.game_tick_==saved_tick && game.entities_.size()==saved_entities);
    same_controllers(midfade_controllers);
    require(game.keyframe_playbacks_.size()==saved_playbacks);
    for(std::size_t word=0;word<saved_frame.size();++word)
        require(std::bit_cast<std::uint32_t>(game.entity(handle).orientation[word])==
                std::bit_cast<std::uint32_t>(saved_frame[word]));
    const auto restored=game.frame();
    require(restored.level==game.level_.get() && restored.generation==saved_generation+1);
    require(!game.entity(game.player_).model.parts.empty());
    const auto& restored_part=game.entity(game.player_).model.parts.front();
    const auto restored_draw=std::find_if(restored.objects.begin(),restored.objects.end(),[&](const auto& draw) {
        return draw.mesh==restored_part.mesh && draw.bones.data()==restored_part.pose.data();
    });
    require(restored_draw!=restored.objects.end() && restored_draw->material==restored_part.shader);
    require(restored_draw->material_override==
            (restored_part.runtime_material?&*restored_part.runtime_material:&game.materials_.construct(restored_part.shader,0,0,true)));
    for(const auto& playback:game.keyframe_playbacks_)
        require(playback.clip==&game.keyframes_.at(identity(playback.name)));
    for(const auto& playback:game.character_playbacks_) {
        const auto& part=game.entity(playback.entity).model.parts.front();
        require(playback.channel>=0 && playback.channel<6);
        require(part.layer_evaluators[playback.channel].has_value());
        require(part.layer_animations[playback.channel]==&game.animations_.at(identity(playback.clip)));
    }

    // Add a non-grammar byte inside the actual material subsection, then
    // rehash the envelope so rejection exercises section EOF, not checksum.
    std::ostringstream material_state(std::ios::binary);game.physical_materials_.save(material_state);
    const auto material_section=material_state.str();
    auto bad_payload=saved_bytes.substr(44);
    const auto material_offset=bad_payload.find(material_section);
    require(material_offset>=4 && material_offset!=std::string::npos &&
            bad_payload.rfind(material_section)==material_offset);
    std::istringstream size_input(bad_payload.substr(material_offset-4,4),std::ios::binary);
    SaveReader size_reader{size_input,4};require(size_reader.u32()==material_section.size());
    bad_payload.insert(material_offset+material_section.size(),1,'!');
    std::ostringstream new_size(std::ios::binary);SaveWriter size_writer{new_size};
    size_writer.count(material_section.size()+1,section_limit);
    bad_payload.replace(material_offset-4,4,new_size.view());
    const auto publish_payload=[&](std::string_view payload) {
        std::array<unsigned char,SHA256_DIGEST_LENGTH> digest{};
        if(!SHA256(reinterpret_cast<const unsigned char*>(payload.data()),payload.size(),digest.data()))
            throw std::runtime_error("Cannot hash native save check payload");
        std::ostringstream envelope(std::ios::binary);SaveWriter writer{envelope};
        writer.bytes(save_magic.data(),save_magic.size());writer.count(payload.size(),save_limit-44);
        writer.bytes(reinterpret_cast<const char*>(digest.data()),digest.size());
        atomic_output(save_path,envelope.view(),payload);
    };
    publish_payload(bad_payload);
    const auto restored_address=game.host_position_address(game.player_);
    const auto restored_word=game.runtime_.read_word(restored_address);
    bool section_rejected=false;
    try{game.load(save_path);}catch(const std::runtime_error& error) {
        require(std::string_view(error.what())=="Invalid native save: trailing section bytes");
        section_rejected=true;
    }
    const auto preserved_live_state=[&] {
        require(actor_state()==saved_actors);
        preserved_sources();
        require(game.time_==saved_time && game.clock_==saved_clock && game.game_tick_==saved_tick);
        require(game.frame().generation==restored.generation && game.level_.get()==restored.level);
        require(game.frame().objects.data()==restored.objects.data());
        require(game.runtime_.read_word(restored_address)==restored_word);
        same_controllers(midfade_controllers);
    };
    require(section_rejected);preserved_live_state();

    // Missing inactive assets remain valid metadata, not permission to bypass
    // AssetStore's Windows-name grammar. Mutate only this real inactive path.
    std::ostringstream source_prefix(std::ios::binary);SaveWriter source_writer{source_prefix};
    source_writer.text(inactive_sound->name);source_writer.text(inactive_sound->path);
    const auto prefix=source_prefix.str();
    auto unsafe_payload=saved_bytes.substr(44);
    const auto source_offset=unsafe_payload.find(prefix);
    require(source_offset!=std::string::npos && unsafe_payload.rfind(prefix)==source_offset);
    std::ostringstream unsafe_prefix(std::ios::binary);SaveWriter unsafe_writer{unsafe_prefix};
    unsafe_writer.text(inactive_sound->name);unsafe_writer.text(inactive_sound->path+".");
    unsafe_payload.replace(source_offset,prefix.size(),unsafe_prefix.view());
    publish_payload(unsafe_payload);
    bool unsafe_rejected=false;
    try{game.load(save_path);}catch(const std::runtime_error& error) {
        require(std::string_view(error.what())=="AssetStore: invalid asset path component");
        unsafe_rejected=true;
    }
    require(unsafe_rejected);preserved_live_state();
    // Rehash genuine six-controller saves after corrupting their wire fields.
    // Each rejected load must leave the live renderer storage/aliases intact.
    const auto controller_bytes=[&](const Game::CharacterPlayback& p) {
        std::ostringstream stream(std::ios::binary);SaveWriter writer{stream};
        writer.u32(p.entity);writer.text(p.clip);writer.real(p.elapsed);writer.flag(p.loop);
        writer.integer(p.channel);writer.real(p.blend);writer.flag(p.active);
        writer.u32(saved_tick-p.start_tick);writer.u32(p.animation_mode);
        writer.u32(p.flags);writer.u32(saved_tick-p.pause_tick);writer.u32(p.sample_tick);
        writer.u32(saved_tick-p.fade_tick);writer.integer(p.master_channel);
        writer.text(p.master_clip);writer.real(p.coefficient);
        return stream.str();
    };
    const auto aim_bytes=controller_bytes(slot(5));
    const auto clean_payload=saved_bytes.substr(44);
    const auto aim_offset=clean_payload.find(aim_bytes);
    require(aim_offset!=std::string::npos && clean_payload.rfind(aim_bytes)==aim_offset);
    const auto channel_offset=8+slot(5).clip.size()+5;
    const auto flags_offset=channel_offset+17;
    const auto master_channel_offset=flags_offset+16;
    const auto rejected_controller_payload=[&](const std::string& payload) {
        publish_payload(payload);bool rejected=false;
        try{game.load(save_path);}catch(const std::runtime_error&){rejected=true;}
        require(rejected);preserved_live_state();
    };
    const auto malformed_word=[&](std::size_t field,std::uint32_t value) {
        auto payload=clean_payload;
        std::ostringstream word(std::ios::binary);SaveWriter writer{word};writer.u32(value);
        payload.replace(aim_offset+field,4,word.view());
        rejected_controller_payload(payload);
    };
    malformed_word(channel_offset,6); // The removed five-slot layout also cannot restore slot5.
    malformed_word(channel_offset,4); // Duplicate actual slot of this same actor.
    for(const auto flags:{0u,32u,13u,3u})malformed_word(flags_offset,flags);
    malformed_word(master_channel_offset,0);
    malformed_word(master_channel_offset,0xffffffffu);
    malformed_word(aim_bytes.size()-4,0x7fc0002au);
    malformed_word(aim_bytes.size()-4,std::bit_cast<std::uint32_t>(-1.f));
    malformed_word(channel_offset+4,std::bit_cast<std::uint32_t>(1.1f));
    {
        auto payload=clean_payload;payload[aim_offset+channel_offset+8]=2;
        rejected_controller_payload(payload); // Invalid active boolean, independently of flags.
    }
    {
        const auto master_bytes=controller_bytes(slot(slot(5).master_channel));
        const auto master_offset=clean_payload.find(master_bytes);
        require(master_offset!=std::string::npos && clean_payload.rfind(master_bytes)==master_offset);
        auto payload=clean_payload;
        payload.erase(master_offset,master_bytes.size());
        std::ostringstream count(std::ios::binary);SaveWriter writer{count};
        writer.count(game.character_playbacks_.size()-1);
        payload.replace(material_offset+material_section.size(),4,count.view());
        rejected_controller_payload(payload); // Valid individual slots, dangling stable master slot.
    }

    // Keep menu discovery independent of every other fixture's/user's saves.
    const auto baseline_path=cleanup.path/"checkpoint-baseline.psv";
    game.save(baseline_path);
    struct ScopedDataHome {
        std::optional<std::string> previous;
        explicit ScopedDataHome(const std::filesystem::path& path) {
            if(const auto* value=std::getenv("XDG_DATA_HOME"))previous=value;
            if(::setenv("XDG_DATA_HOME",path.c_str(),1)!=0)
                throw std::system_error(errno,std::generic_category(),"Set checkpoint check data home");
        }
        ~ScopedDataHome() {
            if(previous)::setenv("XDG_DATA_HOME",previous->c_str(),1);
            else ::unsetenv("XDG_DATA_HOME");
        }
    } data_home{cleanup.path/"checkpoint-xdg"};
    game.load(transition_path);
    game.update(0,{}); // Settle due authored callbacks before recording preservation witnesses.
    for(const auto target:animation_targets)game.actors_.find(target)->combat_state=7;
    call("enter_cinematic");
    call("disable_ingame_processing");
    call("disable_ingame_inputprocessing_except_use");
    call("disable_ingame_rendering");
    call("interface_hide");
    game.runtime_.push_float(7);call("set_fade_time");call("fade_in");
    game.runtime_.push_float(3600);game.runtime_.push_string("__checkpoint_future");
    call("add_timed_event");
    require(std::all_of(game.events_.begin(),game.events_.end(),[&](const auto& event) {
        return event.deadline>game.time_;
    }));

    // Explicit generic saves retain locked processing and outgoing requests.
    const auto exact_path=cleanup.path/"checkpoint-exact.psv";
    game.runtime_.push_string("surlar_1_0");call("checkpoint_load");
    const auto exact_actors=actor_state();
    const auto exact_interface=game.interface_.snapshot();
    game.save(exact_path);
    call("exit_cinematic");call("enable_ingame_processing");call("enable_ingame_rendering");
    game.load(exact_path);
    require(actor_state()==exact_actors && game.cinematic_ && game.camera_blocked_);
    require(!game.processing_ && !game.input_processing_ && !game.rendering_ && game.use_only_);
    require(game.pending_level_=="surlar_1_0" && game.pending_image_ && game.pending_checkpoint_);
    require(game.interface_.snapshot().fade_duration==exact_interface.fade_duration &&
            game.interface_.snapshot().fade_in==exact_interface.fade_in);
    game.pending_level_.clear();game.pending_image_=false;game.pending_checkpoint_=false;

    const auto parts_state=[&](const Game& source) {
        std::ostringstream stream(std::ios::binary);SaveWriter writer{stream};
        const auto model=[&](auto&& self,const Game::Entity::Model& value)->void {
            writer.text(value.name);writer.count(value.parts.size());
            for(const auto& part:value.parts) {
                writer.text(part.mesh_name);writer.text(part.shader);
                writer.u32(part.trace_mask);writer.integer(part.material);writer.flag(part.visible);
                writer.count(part.pose.size());for(const auto& matrix:part.pose)writer.geometry_matrix(matrix);
                write_mutable_material(writer,part.runtime_material);
            }
            writer.count(value.linked.size());for(const auto& child:value.linked)self(self,child);
        };
        for(const auto& e:source.entities_)if(e.alive) {writer.u32(e.handle);model(model,e.model);}
        return stream.str();
    };
    const auto pickup=std::find_if(game.entities_.begin(),game.entities_.end(),[](const auto& e) {
        return e.alive && e.visible && e.pickup_kind==3 && e.pickup_health>0 &&
            e.model.visible && e.model.has_bounds &&
            std::any_of(e.model.parts.begin(),e.model.parts.end(),[](const auto& part) {
                return (part.trace_mask&0x1000u)!=0;
            });
    });
    require(pickup!=game.entities_.end());
    const auto pickup_handle=pickup->handle;
    const auto pickup_callback=pickup->name+"_on_taken";
    const auto pickup_local_center=(pickup->model.bounds.minimum+pickup->model.bounds.maximum)*.5f;
    const auto pickup_start=transform_point(pickup->orientation,
        {pickup->model.bounds.minimum.x-1,pickup_local_center.y,pickup_local_center.z});
    const auto pickup_center=transform_point(pickup->orientation,pickup_local_center);
    game.actors_.set_health(game.player_,.25f);
    const auto checkpoint_health=std::min(1.f,.25f+pickup->pickup_health);
    const auto checkpoint_actor=*game.actors_.find(game.player_);
    // Healing changes the owned blood pass before _on_taken saves the checkpoint.
    // Witness that boundary, then restore the genuine damaged pickup precondition.
    game.actors_.set_health(game.player_,checkpoint_health);
    const auto checkpoint_parts=parts_state(game);
    game.actors_.set_health(game.player_,checkpoint_actor.health);
    const auto checkpoint_controllers=game.character_playbacks_;
    const auto checkpoint_events=game.events_;
    const auto checkpoint_serial=game.next_event_serial_;
    const auto checkpoint_clock=game.clock_,checkpoint_time=game.time_;
    const auto checkpoint_tick=game.game_tick_;
    const auto checkpoint_pathname=checkpoint_path("surlar_1_0");
    const auto later_position=checkpoint_actor.position+Vec3{123,45,67};

    // Encode genuine PCS records, including inline owned strings, then use
    // production host registrations rather than replacing callback dispatch.
    const auto load_callback=[&](std::string_view name,const std::vector<std::uint32_t>& code) {
        std::vector<std::uint8_t> bytes;
        const auto word=[&](std::uint32_t value) {
            for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(value>>shift);
        };
        word(0);word(static_cast<std::uint32_t>(name.size()));
        bytes.insert(bytes.end(),name.begin(),name.end());
        word(0);word(static_cast<std::uint32_t>(code.size()));
        for(const auto value:code)word(value);
        game.runtime_.load(script::Program::decode(bytes));
    };
    std::vector<std::uint32_t> checkpoint_code;
    const auto text=[&](std::uint32_t opcode,std::string_view value) {
        const auto count=static_cast<std::uint32_t>((value.size()+4)/4);
        checkpoint_code.push_back(opcode);checkpoint_code.push_back(count);
        const auto start=checkpoint_code.size();checkpoint_code.resize(start+count,0);
        for(std::size_t index=0;index<value.size();++index)
            checkpoint_code[start+index/4]|=std::uint32_t(static_cast<unsigned char>(value[index]))<<((index%4)*8);
    };
    text(5,"surlar_1_0");text(35,"checkpoint_save");
    for(const auto coordinate:{later_position.z,later_position.y,later_position.x})
        checkpoint_code.insert(checkpoint_code.end(),{1,std::bit_cast<std::uint32_t>(coordinate)});
    text(35,"get_player");text(35,"entity_set_pos_manual");
    checkpoint_code.insert(checkpoint_code.end(),{1,std::bit_cast<std::uint32_t>(.1f)});
    text(35,"player_hurt");
    text(5,"beyaz_odaya_gecis");text(35,"load_level_without_image");
    load_callback(pickup_callback,checkpoint_code);
    require(game.runtime_.contains(pickup_callback) && game.actor_events_.empty());
    // Real pickup queues medikit_taken BEFORE _on_taken and deletes itself AFTER.
    game.trace_scene(pickup_start,pickup_center,game.player_,0,2,true);
    require(!game.entities_[pickup_handle-1].alive && std::filesystem::is_regular_file(checkpoint_pathname));
    require(game.runtime_.stack_size()==stack_size);
    close_matrix(game.actors_.find(game.player_)->orientation,game.entity(game.player_).orientation);
    require(game.host_position(game.player_).x==later_position.x &&
            game.host_position(game.player_).y==later_position.y &&
            game.host_position(game.player_).z==later_position.z);
    require(game.actors_.find(game.player_)->health<checkpoint_health);
    require(game.pending_level_=="beyaz_odaya_gecis" && !game.pending_checkpoint_ && !game.pending_image_);
    require(game.actor_events_.size()==1 &&
            game.actor_events_.front().kind==ActorEvent::Kind::sound &&
            game.actor_events_.front().name=="medikit_taken");
    bool callback_save_rejected=false;
    try { game.save(cleanup.path/"undelivered.psv"); }
    catch(const std::runtime_error& error) {
        require(std::string_view(error.what())=="Cannot save undelivered actor events");
        callback_save_rejected=true;
    }
    require(callback_save_rejected && game.actor_events_.size()==1);

    const auto checkpoint_restored=[&](const Game& restored_game) {
        const auto* actor=restored_game.actors_.find(restored_game.player_);
        require(actor && actor->health==checkpoint_health);
        close_matrix(actor->orientation,checkpoint_actor.orientation);
        require(actor->position.x==checkpoint_actor.position.x &&
                actor->position.y==checkpoint_actor.position.y && actor->position.z==checkpoint_actor.position.z);
        require(actor->weapon_slots==checkpoint_actor.weapon_slots &&
                actor->selected_slot==checkpoint_actor.selected_slot && actor->weapon==checkpoint_actor.weapon);
        require(actor->ammunition==checkpoint_actor.ammunition &&
                actor->ammunition_order==checkpoint_actor.ammunition_order && actor->inventory==checkpoint_actor.inventory);
        for(std::size_t i=0;i<actor->weapons.size();++i) {
            require(actor->weapons[i].kind==checkpoint_actor.weapons[i].kind &&
                    actor->weapons[i].magazine==checkpoint_actor.weapons[i].magazine &&
                    actor->weapons[i].owned==checkpoint_actor.weapons[i].owned);
        }
        require(restored_game.processing_ && restored_game.input_processing_ && restored_game.rendering_);
        require(!restored_game.menu_ && !restored_game.cinematic_ && !restored_game.camera_blocked_);
        require(restored_game.use_only_ && restored_game.hud_visible_ && !restored_game.fullscreen_);
        require(restored_game.pending_level_.empty() && !restored_game.pending_image_ &&
                !restored_game.pending_checkpoint_ && restored_game.actor_events_.empty());
        require(restored_game.entity(pickup_handle).alive); // Earlier callback boundary, not later deletion.
        require(restored_game.clock_==checkpoint_clock && restored_game.time_==checkpoint_time &&
                restored_game.game_tick_==checkpoint_tick);
        require(restored_game.next_event_serial_==checkpoint_serial &&
                restored_game.events_.size()==checkpoint_events.size());
        for(std::size_t i=0;i<checkpoint_events.size();++i) {
            require(restored_game.events_[i].name==checkpoint_events[i].name &&
                    restored_game.events_[i].serial==checkpoint_events[i].serial &&
                    std::abs(restored_game.events_[i].deadline-checkpoint_events[i].deadline)<1e-9);
        }
        const auto restored_parts=parts_state(restored_game);
        require(restored_parts==checkpoint_parts);
        require(restored_game.character_playbacks_.size()==checkpoint_controllers.size());
        for(std::size_t i=0;i<checkpoint_controllers.size();++i)
            require(controller_bytes(restored_game.character_playbacks_[i])==controller_bytes(checkpoint_controllers[i]));
        const auto fade=restored_game.interface_.snapshot();
        require(!restored_game.fade_in_ && restored_game.fade_seconds_==.5f);
        require(!fade.fade_in && fade.fading && fade.fade_duration==.5f &&
                fade.fade_elapsed==0 && fade.fade_alpha==1);
        require(fade.hud_visible && !fade.fullscreen_visible);
    };
    game.runtime_.push_string("surlar_1_0");call("checkpoint_load");
    game.update(0,{}); // Queued checkpoint_load uses the shared staged continuation.
    checkpoint_restored(game);
    require(game.camera_transition_tick_==checkpoint_tick); // Pre-load live camera was blocked.
    same_controllers(checkpoint_controllers);
    game.camera_transition_tick_=game.game_tick_-17; // Inactive transition age belongs to the live frame.
    game.action({"oyun_yukle_on_activate",{},-1});
    game.interface_.set_selected_item("oyun_yukle","list_oyun_yukle_checkpoint",0);
    game.action({"list_oyun_yukle_checkpoint",{},-1});
    game.action({"oyun_yukle_yukle",{},-1});
    checkpoint_restored(game);
    require(game.camera_transition_tick_==checkpoint_tick-17);
    {
        Game resumed(game.assets_,game.materials_,game.settings_,game.interface_,game.media_);
        require(!resumed.level_);
        const auto resume_transition_age=resumed.game_tick_-resumed.camera_transition_tick_;
        resumed.action({"oyuna_devam",{},-1}); // No-world resume must load the sandbox's sole save.
        checkpoint_restored(resumed);
        require(resumed.game_tick_-resumed.camera_transition_tick_==resume_transition_age);
    }
    game.load(baseline_path);
    game.load(paired_path); // Actual standing player snapshot, before the crouch checks.
    const auto graph_trace_consumers=[&](std::uint32_t handle,std::string_view mesh,std::string_view shader) {
        const auto& owner=game.entity(handle);
        const auto part=std::find_if(owner.model.parts.begin(),owner.model.parts.end(),[&](const auto& value) {
            return value.mesh_name==mesh;
        });
        require(part!=owner.model.parts.end());
        const auto* actor=game.actors_.find(game.player_);
        require(actor && actor->posture_state==0 && !actor->fully_crouched);
        const auto center=(part->collision_bounds.minimum+part->collision_bounds.maximum)*.5f;
        const auto half=(actor->hull.maximum-actor->hull.minimum)*.5f;
        const auto end=transform_point(owner.orientation,center);
        const auto motion_start=transform_point(owner.orientation,
            {part->collision_bounds.minimum.x-half.x-1,center.y,center.z});
        const auto segment=end-motion_start;
        const auto motion=game.trace_motion(motion_start,original_normalized(segment),original_length(segment),
            actor->hull,game.player_,0x200u,2u,false,game.player_,true);
        require(motion.hit && motion.entity==handle && motion.mesh==mesh && motion.shader==shader);
        const auto impact_start=transform_point(owner.orientation,
            {part->collision_bounds.minimum.x-1,center.y,center.z});
        const auto impact=game.trace_scene(impact_start,end,game.player_,0x400u,2u,false);
        require(impact.hit && impact.entity==handle && impact.mesh==mesh && impact.shader==shader);
    };
    require(!game.materials_.find_instance("3te_objects/gun_silencer_2013"));

    // Use a genuine authored PO attachment whose shader distinguishes original
    // object construction (0x10000) from a saved/host shader replacement (0).
    const auto graph_handle=game.host_create_entity("__save_graph_real",0,"chr_character_01_512");
    const auto graph_owner_name=game.entity(graph_handle).name;
    game.host_set_position(graph_handle,game.actors_.find(game.player_)->position+Vec3{0,0,150});
    const auto graph_baseline_path=cleanup.path/"authored-graph.psv";
    game.save(graph_baseline_path);game.load(graph_baseline_path); // Fresh constructor epoch, real full PO graph.
    const auto& graph=game.entity(graph_handle).model;
    const auto original_child=std::find_if(graph.linked.begin(),graph.linked.end(),[](const auto& child) {
        return identity(child.name)=="2013_cz_75";
    });
    require(original_child!=graph.linked.end() && original_child->attachment && !original_child->parts.empty());
    const auto child_part=std::find_if(original_child->parts.begin(),original_child->parts.end(),[](const auto& part) {
        return identity(part.shader)=="3te_objects/gun_silencer_2013";
    });
    require(child_part!=original_child->parts.end() && !graph.parts.empty());
    const auto child_shader=child_part->shader;
    const auto root_mesh=graph.parts.front().mesh_name;
    const auto child_mask=game.materials_.runtime_trace_mask(child_shader);
    require(game.materials_.find_instance(child_shader) && child_mask==0x108600u);
    require(std::none_of(graph.parts.begin(),graph.parts.end(),[&](const auto& part) {
        return identity(part.shader)==identity(child_shader);
    }));
    game.host_shader(graph_owner_name,root_mesh,child_shader,false);
    const auto graph_replacement_path=cleanup.path/"authored-graph-replacement.psv";
    game.save(graph_replacement_path);game.load(graph_replacement_path);
    const auto graph_replaced=[&] {
        const auto& root=game.entity(graph_handle).model;
        const auto part=std::find_if(root.parts.begin(),root.parts.end(),[&](const auto& value) {
            return value.mesh_name==root_mesh;
        });
        require(part!=root.parts.end() && part->shader==child_shader);
        // Saved Part.trace_mask can conceal a wrong shared first claim.
        require(game.materials_.runtime_trace_mask(child_shader)==child_mask);
        game.host_shader(graph_owner_name,root_mesh,child_shader,false); // Same-shader post-load reassignment.
        graph_trace_consumers(graph_handle,root_mesh,child_shader);
    };
    graph_replaced();
    require(std::any_of(game.entity(graph_handle).model.linked.begin(),
        game.entity(graph_handle).model.linked.end(),[](const auto& child) {
            return identity(child.name)=="2013_cz_75";
        }));
    auto& saved_children=game.entity(graph_handle).model.linked;
    require(std::erase_if(saved_children,[](const auto& child) {
        return identity(child.name)=="2013_cz_75";
    })==1);
    const auto missing_child_path=cleanup.path/"authored-graph-missing-child.psv";
    game.save(missing_child_path);game.load(missing_child_path);
    graph_replaced();
    require(std::none_of(game.entity(graph_handle).model.linked.begin(),
        game.entity(graph_handle).model.linked.end(),[](const auto& child) {
            return identity(child.name)=="2013_cz_75";
        }));
    game.load(baseline_path);

    // A's ORIGINAL siblings are B(phone), then C(pistol). B's saved shader
    // override must not claim C before the original sibling factory reaches it.
    game.load(paired_path);
    require(!game.materials_.find_instance(child_shader));
    const auto sibling_handle=game.host_create_entity("__save_sibling_real",0,"anm_char_zap_suikastci");
    const auto sibling_owner_name=game.entity(sibling_handle).name;
    game.host_set_position(sibling_handle,game.actors_.find(game.player_)->position+Vec3{0,0,150});
    const auto sibling_baseline_path=cleanup.path/"authored-siblings.psv";
    game.save(sibling_baseline_path);game.load(sibling_baseline_path);
    auto& sibling_graph=game.entity(sibling_handle).model;
    require(sibling_graph.linked.size()==2 &&
            identity(sibling_graph.linked[0].name)=="eqp_cell_phone_2013" &&
            identity(sibling_graph.linked[1].name)=="2013_cz_75");
    require(!sibling_graph.parts.empty() && sibling_graph.linked[0].parts.size()==1 &&
            !sibling_graph.linked[1].parts.empty());
    const auto sibling_shader=sibling_graph.linked[1].parts.front().shader;
    const auto sibling_root_mesh=sibling_graph.parts.front().mesh_name;
    const auto sibling_mask=game.materials_.runtime_trace_mask(sibling_shader);
    require(sibling_shader==child_shader && sibling_mask==0x108600u);
    require(sibling_graph.linked[0].parts.front().shader!=sibling_shader &&
            sibling_graph.parts.front().shader!=sibling_shader);
    auto& saved_phone=sibling_graph.linked[0].parts.front();
    saved_phone.shader=sibling_shader;saved_phone.runtime_material.reset();
    game.resolve_part_material(saved_phone,0); // Genuine linked-part saved override, no fabricated mask.
    const auto sibling_override_path=cleanup.path/"authored-sibling-override.psv";
    game.save(sibling_override_path);game.load(sibling_override_path);
    require(game.entity(sibling_handle).model.linked[0].parts.front().shader==sibling_shader);
    require(game.materials_.runtime_trace_mask(sibling_shader)==sibling_mask);
    game.host_shader(sibling_owner_name,sibling_root_mesh,sibling_shader,false);
    graph_trace_consumers(sibling_handle,sibling_root_mesh,sibling_shader);
    game.load(paired_path); // Keep the standing trace fixture, not the later crouched baseline.

    // Genuine retail absences: preserve authored identity, order and placement,
    // rather than substituting the similarly named papers PO or a dummy PM.
    const auto tower_scene=read_scene(game.assets_,"level/object/askeri_us_4.txt");
    const auto tower_entry=std::find_if(tower_scene.entries.begin(),tower_scene.entries.end(),[](const auto& entry) {
        return entry.name=="floresans" && entry.mesh=="fur_light_tower_01";
    });
    require(tower_entry!=tower_scene.entries.end() && tower_entry->align_positive[0] &&
            tower_entry->align_positive[2]);
    const auto papers_scene=read_scene(game.assets_,"level/object/askeri_us_1.txt");
    const auto papers_entry=std::find_if(papers_scene.entries.begin(),papers_scene.entries.end(),[](const auto& entry) {
        return entry.mesh=="obj?garbage_papers_02";
    });
    require(papers_entry!=papers_scene.entries.end());
    const auto authored_prop=[&](const auto& entry) {
        const auto handle=game.host_create_entity(entry.name,0,entry.mesh);
        game.host_set_position(handle,entry.position);
        auto& frame=game.entity(handle).orientation;
        rotate_local(frame,entry.local_rotation);rotate_world(frame,entry.world_rotation);
        const auto before_alignment=frame;
        for(int axis=0;axis<3;++axis) {
            if(entry.align_positive[axis])game.host_align(handle,axis,0);
            if(entry.align_negative[axis])game.host_align(handle,axis,1);
        }
        close_matrix(game.entity(handle).orientation,before_alignment);
        return handle;
    };
    const auto tower_handle=authored_prop(*tower_entry);
    const auto papers_handle=authored_prop(*papers_entry);
    const auto tower_frame=game.entity(tower_handle).orientation;
    require(game.entity(tower_handle).model.parts.size()==1);
    const auto papers_frame=game.entity(papers_handle).orientation;
    require(tower_frame[12]==1482 && tower_frame[13]==3179 && tower_frame[14]==275);
    require(!game.assets_.contains_optional_game_file("object/pm/fur_light_tower_01.pm") &&
            !game.assets_.contains_optional_game_file("object/po/obj?garbage_papers_02.po"));
    const auto tower_material=game.entity(tower_handle).model.parts.front().material;
    const auto absent_objects=[&] {
        const auto& tower=game.entity(tower_handle);
        require(tower.object_name=="fur_light_tower_01" && tower.model.definition &&
                tower.model.name=="fur_light_tower_01" && tower.model.parts.size()==1 &&
                !tower.model.has_bounds);
        const auto& part=tower.model.parts.front();
        require(part.mesh_name=="fur_light_tower_01" && part.shader=="3te_objects/fur_light_tower_01" &&
                !part.mesh && part.pose.empty() && !part.evaluator && part.lighting_id &&
                part.material==tower_material && part.trace_mask==0x108600u);
        require(game.materials_.find_instance(part.shader) &&
                game.materials_.runtime_trace_mask(part.shader)==0x108600u);
        close_matrix(tower.orientation,tower_frame);
        require(game.has_attachment(tower_handle,part.mesh_name) &&
                !game.has_attachment(tower_handle,"__unavailable_bone"));
        close_matrix(game.attachment_world(tower_handle,part.mesh_name),tower_frame);
        const auto& papers=game.entity(papers_handle);
        require(papers.name==papers_entry->name && papers.object_name=="obj?garbage_papers_02" &&
                papers.model.name=="obj?garbage_papers_02" && !papers.model.definition &&
                papers.model.parts.empty() && papers.model.linked.empty() && !papers.model.has_bounds);
        close_matrix(papers.orientation,papers_frame);
    };
    absent_objects();

    // eqp_monitor_01.PO really has absent eqp_monitor FIRST and the available
    // eqp_monitor_screen SECOND. Both shaders still get original PO construction.
    const auto mixed_handle=game.host_create_entity("__save_missing_monitor",0,"eqp_monitor_01");
    game.host_set_position(mixed_handle,game.actors_.find(game.player_)->position+Vec3{0,0,150});
    auto& mixed=game.entity(mixed_handle).model;
    require(mixed.parts.size()==2 && !mixed.parts[0].mesh && mixed.parts[1].mesh);
    require(mixed.parts[0].trace_mask==0x108600u && mixed.parts[1].trace_mask==0x108600u);
    mixed.parts[0].trace_mask=0x80001000u; // Independent saved mask; missing-only pickup permission.
    const auto mixed_state=[&] {
        const auto& model=game.entity(mixed_handle).model;
        require(model.parts.size()==2 && model.has_bounds);
        const auto& missing=model.parts[0];const auto& loaded=model.parts[1];
        require(missing.mesh_name=="eqp_monitor" && missing.shader=="3te_objects/eqp_monitor" &&
                !missing.mesh && missing.pose.empty() && !missing.evaluator && missing.trace_mask==0x80001000u);
        require(loaded.mesh_name=="eqp_monitor_screen" &&
                loaded.shader=="3te_objects/eqp_monitor_screen_01" && loaded.mesh &&
                loaded.trace_mask==0x108600u && missing.lighting_id && loaded.lighting_id>missing.lighting_id);
        require(game.materials_.find_instance(missing.shader) &&
                game.materials_.runtime_trace_mask(missing.shader)==0x108600u);
        require(game.has_attachment(mixed_handle,missing.mesh_name));
        close_matrix(game.attachment_world(mixed_handle,missing.mesh_name),
            game.entity(mixed_handle).orientation);
        require(!game.assets_.contains_optional_game_file("object/pm/eqp_monitor.pm") &&
                game.assets_.contains_optional_game_file("object/pm/eqp_monitor_screen.pm"));
        close_matrix(game.entity(mixed_handle).orientation,model.world);
        require(model.bounds.minimum.x==loaded.collision_bounds.minimum.x &&
                model.bounds.minimum.y==loaded.collision_bounds.minimum.y &&
                model.bounds.minimum.z==loaded.collision_bounds.minimum.z &&
                model.bounds.maximum.x==loaded.collision_bounds.maximum.x &&
                model.bounds.maximum.y==loaded.collision_bounds.maximum.y &&
                model.bounds.maximum.z==loaded.collision_bounds.maximum.z);
        const auto* standing=game.actors_.find(game.player_);
        require(standing && standing->posture_state==0 && !standing->fully_crouched);
        const auto half=(standing->hull.maximum-standing->hull.minimum)*.5f;
        const auto end=transform_point(game.entity(mixed_handle).orientation,{0,0,0});
        const auto start=transform_point(game.entity(mixed_handle).orientation,{-half.x-1,0,0});
        const auto segment=end-start;
        require(!game.trace_motion(start,original_normalized(segment),original_length(segment),
            standing->hull,game.player_,0x80000000u,2u,false,game.player_,true).hit);
        const auto center=(loaded.collision_bounds.minimum+loaded.collision_bounds.maximum)*.5f;
        const auto target=transform_point(game.entity(mixed_handle).orientation,center);
        const auto pickup_start=transform_point(game.entity(mixed_handle).orientation,
            {loaded.collision_bounds.minimum.x-half.x-1,center.y,center.z});
        const auto pickup_segment=target-pickup_start;
        // The retail screen is epsilon-thin; query it with the real standing hull.
        const auto hit=game.trace_motion(pickup_start,original_normalized(pickup_segment),
            original_length(pickup_segment),standing->hull,game.player_,0x400u,2u,true,game.player_,true);
        require(game.entity(mixed_handle).alive && hit.hit && hit.entity==mixed_handle &&
                hit.mesh==loaded.mesh_name && hit.shader==loaded.shader); // Null-only 0x1000 must not make it a pickup.
    };
    game.rebuild_frame();mixed_state();

    // This real authored graph includes an absent gun_pistol_jericho.PO child.
    const auto absent_child_handle=game.host_create_entity("__save_missing_child_po",0,"chr_character_01");
    game.host_set_position(absent_child_handle,game.actors_.find(game.player_)->position+Vec3{0,0,500});
    const auto absent_child_state=[&] {
        const auto& root=game.entity(absent_child_handle).model;
        require(root.definition && root.linked.size()==20 && root.parts.size()==1 && root.parts.front().mesh);
        const auto& child=root.linked[12];
        const auto& authored=root.definition->attachments.at(12);
        require(child.name=="gun_pistol_jericho" && child.attachment &&
                child.attachment->object==authored.object && child.attachment->bone==authored.bone &&
                child.attachment->transform && authored.transform);
        close_matrix(*child.attachment->transform,*authored.transform);
        require(!child.definition && child.parts.empty() && child.linked.empty() && !child.has_bounds);
        require(child.attachment_part<root.parts.size() && root.parts[child.attachment_part].mesh);
        require(!game.assets_.contains_optional_game_file("object/po/gun_pistol_jericho.po"));
    };
    game.rebuild_frame();absent_child_state();
    const auto absent_path=cleanup.path/"retail-absent-geometry.psv";
    const auto absent_parts=parts_state(game);
    game.save(absent_path);game.load(absent_path);
    require(parts_state(game)==absent_parts);
    absent_objects();mixed_state();
    absent_child_state();
    game.configure_corpse(game.entity(tower_handle));
    require(!game.entity(tower_handle).model.has_bounds);

    // Named mesh lookup is valid without geometry; a specific parent bone is not.
    auto& unavailable_parent=game.entity(tower_handle).model;
    unavailable_parent.linked.emplace_back();
    auto& unavailable_child=unavailable_parent.linked.back();
    game.build_model(unavailable_child,"fur_light_tower_01");
    unavailable_child.attachment.emplace();
    unavailable_child.attachment->object="fur_light_tower_01";
    unavailable_child.attachment->bone="__unavailable_bone";
    bool bone_rejected=false;
    try { game.rebuild_frame(); }
    catch(const std::runtime_error& error) {
        require(std::string_view(error.what())=="attachment parent has no mesh");bone_rejected=true;
    }
    require(bone_rejected);
    const auto unavailable_bone_path=cleanup.path/"retail-unavailable-bone.psv";
    game.save(unavailable_bone_path);
    bone_rejected=false;
    try { game.load(unavailable_bone_path); }
    catch(const std::runtime_error& error) {
        require(std::string_view(error.what())=="Invalid native save: attachment parent has no mesh");bone_rejected=true;
    }
    require(bone_rejected);
    unavailable_parent.linked.clear();
    game.load(baseline_path);
}
#endif
void Game::write_save(std::ostream& output,SnapshotPurpose purpose) const {
    // Original0042d340 writes the current world inside its VM callback. Neither
    // the suspended VM nor future native callbacks belong to that checkpoint;
    // do not drain them and accidentally capture later world mutations.
    if (purpose==SnapshotPurpose::exact && !actor_events_.empty())
        throw std::runtime_error("Cannot save undelivered actor events");
    SaveWriter w{output};
    const auto media=media_.snapshot();std::set<std::uint64_t> live_audio;
    for(const auto& sound:media.sounds)live_audio.insert(sound.id);
    logical_name(level_name_);logical_name(bsp_path_);
    w.text(level_name_);w.text(bsp_path_);w.text(sound_set_);w.text(music_);
    w.real(time_);w.real(clock_);w.u32(player_);w.u32(next_entity_);
    w.u32(selected_camera_);w.u32(main_camera_);
    w.u64(next_event_serial_);
    w.u32(game_tick_);w.u32(next_tick_-game_tick_);
    w.real(camera_fov_);w.real(fade_seconds_);
    w.vector(camera_.position);w.matrix(camera_.view);w.real(camera_.near_plane);w.real(camera_.far_plane);w.real(camera_.reference_fov_degrees);
    w.flag(menu_);w.flag(quit_);w.flag(processing_);w.flag(input_processing_);w.flag(rendering_);
    w.flag(use_only_);w.flag(cinematic_);w.flag(hud_visible_);w.flag(no_weapon_);
    w.flag(fog_enabled_);w.integer(fog_type_);for(auto value:fog_color_)w.real(value);
    w.u32(fog_start_);w.u32(fog_end_);w.real(fog_density_);w.integer(environment_);
    w.flag(fullscreen_);w.flag(subtitle_);w.flag(fade_in_);
    w.flag(prerender_pending_);
    w.flag(scheduler_ready_);w.flag(player_effect_);
    w.flag(camera_blocked_);w.u32(game_tick_-camera_transition_tick_);
    w.text(fullscreen_shader_);w.text(subtitle_shader_);for(const auto& line:subtitles_)w.text(line);
    w.text(pending_level_);w.flag(pending_image_);w.flag(pending_checkpoint_);
    std::uint32_t models_left=65536;
    const auto model=[&](auto&& self,const Entity::Model& m,std::string_view resource,unsigned depth)->void {
        if(depth>64 || models_left==0)invalid_save("attachment graph limit");
        --models_left;
        if((m.definition || !m.parts.empty() || m.attachment) && resource.empty())invalid_save("missing model resource");
        w.text(m.name);
        // An absent attached PO still owns its resource through Attachment::object.
        w.text(m.definition || !m.parts.empty() || m.attachment ? resource : std::string_view{});
        w.flag(m.visible);w.geometry_matrix(m.world);w.count(m.attachment_part);
        w.flag(m.renderable);w.flag(m.frustum_cull);w.integer(m.region);
        w.flag(m.attachment.has_value());
        if(m.attachment) {
            w.text(m.attachment->object);w.flag(m.attachment->bone.has_value());
            if(m.attachment->bone)w.text(*m.attachment->bone);
            w.flag(m.attachment->transform.has_value());
            if(m.attachment->transform)w.matrix(*m.attachment->transform);
        }
        w.count(m.parts.size());
        for(const auto& part:m.parts) {
            w.text(part.mesh_name);w.text(part.shader);w.count(part.pose.size());
            for(const auto& matrix:part.pose)w.geometry_matrix(matrix);
            w.u32(part.trace_mask);w.integer(part.material);
            w.flag(part.visible);
            write_mutable_material(w,part.runtime_material);
        }
        w.count(m.linked.size());
        for(const auto& child:m.linked)
            self(self,child,child.attachment ? std::string_view(child.attachment->object) : std::string_view{},depth+1);
    };
    w.count(entities_.size());
    for(const auto& e:entities_) {
        w.u32(e.handle);w.text(e.name);w.text(e.object_name);w.integer(e.kind);w.flag(e.alive);w.flag(e.visible);
        w.geometry_vector(e.position);w.geometry_matrix(e.orientation);w.real(e.camera_fov);w.real(e.near_plane);w.real(e.far_plane);
        w.flag(e.camera_fov_explicit);
        w.integer(e.pickup_kind);for(auto value:e.pickup_weapon)w.integer(value);
        w.integer(e.pickup_ammunition_type);w.integer(e.pickup_ammunition_count);w.real(e.pickup_health);
        w.real(e.health);w.real(e.explosion_damage);w.real(e.explosion_radius);w.flag(e.destruction_started);
        model(model,e.model,e.object_name,0);
    }
    w.count(explosions_.size());
    for(const auto& explosion:explosions_) {
        w.u32(explosion.entity);w.geometry_vector(explosion.position);w.real(explosion.radius);w.real(explosion.damage);
    }
    w.section([&](auto& stream){actors_.save(stream);});
    w.section([&](auto& stream){effects_.save(stream);});
    w.section([&](auto& stream){physical_materials_.save(stream);});
    w.count(character_playbacks_.size());
    for(const auto& p:character_playbacks_) {
        w.u32(p.entity);w.text(p.clip);w.real(p.elapsed);w.flag(p.loop);
        w.integer(p.channel);w.real(p.blend);w.flag(p.active);
        w.u32(game_tick_-p.start_tick);w.u32(p.animation_mode);
        w.u32(p.flags);w.u32(game_tick_-p.pause_tick);w.u32(p.sample_tick);
        w.u32(game_tick_-p.fade_tick);w.integer(p.master_channel);
        w.text(p.master_clip);w.real(p.coefficient);
    }
    w.count(keyframe_playbacks_.size());
    for(const auto& p:keyframe_playbacks_) {
        if(!p.clip)invalid_save("unbound keyframe resource");
        const auto duration=.001f*keyframe_playback_duration_ticks(*p.clip);
        w.text(p.name);w.text(p.linked);w.text(p.event);w.real(duration);
        w.u32(game_tick_-p.start_tick);w.u32(p.frozen_phase);
        w.integer(p.mode);w.integer(p.repeats);w.integer(p.completed);w.flag(p.active);w.flag(p.paused);
        w.flag(p.pause_dirty);w.flag(p.camera_selected);
        w.count(p.targets.size());for(auto handle:p.targets)w.u32(handle);
        w.count(p.target_tracks.size());for(auto track:p.target_tracks)w.count(track);
        w.count(p.target_base.size());for(const auto& matrix:p.target_base)w.geometry_matrix(matrix);
        w.u32(p.previous_camera);w.u32(p.camera_target);
        w.count(p.transforms.size());for(const auto& matrix:p.transforms)w.geometry_matrix(matrix);
    }
    w.count(events_.size());
    for(const auto& event:events_) {w.text(event.name);w.real(event.deadline-time_);w.u64(event.serial);}
    if(!level_ || triggers_.inside.size()!=level_->triggers.size() ||
       triggers_.last_ticks.size()!=triggers_.inside.size())invalid_save("trigger state count");
    w.count(triggers_.inside.size());for(auto value:triggers_.inside)w.u8(value);
    w.count(triggers_.active.size());
    for(auto index:triggers_.active) {
        if(index>=triggers_.inside.size() || !triggers_.inside[index])invalid_save("active trigger ID");
        w.u32(index);w.u32(game_tick_-triggers_.last_ticks[index]);
    }
    w.count(sounds_.size());
    for(const auto& sound:sounds_) {
        w.text(sound.name);w.text(sound.path);w.vector(sound.position);w.u8(sound.volume);
        w.flag(sound.loop);w.flag(sound.spatial);w.flag(sound.stream);
        w.real(sound.min_distance);
        // Completed one-shots can remain in a named source's bookkeeping.
        w.count(std::count_if(sound.channels.begin(),sound.channels.end(),[&](auto id){return live_audio.contains(id);}));
        for(auto channel:sound.channels)if(live_audio.contains(channel))w.u64(channel);
    }
    w.count(variables_.size());
    for(const auto& [name,variable]:variables_) {
        w.text(name);w.integer(variable.type);w.u32(variable.value);
        // Original static-variable type is immutable, independently of string ownership.
        w.flag(variable.owns_string);w.text(variable.text);
    }
    write_media(w,media);write_interface(w,interface_.snapshot());
}

void Game::read_save(std::istream& input) {
    const auto start=input.tellg();input.seekg(0,std::ios::end);const auto end=input.tellg();input.seekg(start);
    if(start<0 || end<start || static_cast<std::uint64_t>(end-start)>save_limit)invalid_save("payload length");
    SaveReader r{input,static_cast<std::size_t>(end-start)};
    level_name_=r.name();bsp_path_=r.name();sound_set_=r.name(true);music_=r.name(true);
    time_=r.real64();clock_=r.real64();player_=r.u32();next_entity_=r.u32();
    selected_camera_=r.u32();main_camera_=r.u32();
    next_event_serial_=r.u64();
    game_tick_=r.u32();next_tick_=game_tick_+r.u32();
    camera_fov_=r.real();fade_seconds_=r.real();
    if(time_<0 || clock_<0 || fade_seconds_<0)invalid_save("clock ranges");
    camera_.position=r.vector();camera_.view=r.matrix();camera_.near_plane=r.real();camera_.far_plane=r.real();camera_.reference_fov_degrees=r.real();
    if(camera_.near_plane<=0 || camera_.far_plane<=camera_.near_plane || camera_fov_<0 || camera_fov_>=180 ||
       camera_.reference_fov_degrees<0 || camera_.reference_fov_degrees>=180)
        invalid_save("camera ranges");
    menu_=r.flag();quit_=r.flag();processing_=r.flag();input_processing_=r.flag();rendering_=r.flag();
    use_only_=r.flag();cinematic_=r.flag();hud_visible_=r.flag();no_weapon_=r.flag();
    fog_enabled_=r.flag();fog_type_=r.integer();for(auto& value:fog_color_)value=r.real();
    fog_start_=r.u32();fog_end_=r.u32();fog_density_=r.real();environment_=r.integer();
    if((fog_type_!=0x800 && fog_type_!=0x801 && fog_type_!=0x2601) || fog_density_<0 || environment_<0 || environment_>2)invalid_save("environment ranges");
    fullscreen_=r.flag();subtitle_=r.flag();fade_in_=r.flag();
    prerender_pending_=r.flag();
    scheduler_ready_=r.flag();player_effect_=r.flag();
    camera_blocked_=r.flag();camera_transition_tick_=game_tick_-r.u32();
    fullscreen_shader_=r.name(true);subtitle_shader_=r.name(true);for(auto& line:subtitles_)line=r.text();
    pending_level_=r.name(true);pending_image_=r.flag();pending_checkpoint_=r.flag();
    level_=std::make_unique<Level>(read_level(assets_.path(bsp_path_)));
    collision_=std::make_unique<CollisionWorld>(*level_,materials_);
    clear_membership(); // Corpse reconstruction relinks entities before the final frame rebuild.
    physical_materials_.set_level(level_.get());
    reset_trigger_state(triggers_,*level_);
    std::uint32_t models_left=65536;
    using PartMaterialSnapshot=std::pair<Entity::Part*,MutableMaterialSnapshot>;
    std::vector<PartMaterialSnapshot> material_snapshots;
    std::size_t material_allocation_budget=save_limit;
    const auto model=[&](auto&& self,Entity::Model& m,unsigned depth)->void {
        if(depth>64 || models_left==0)invalid_save("attachment graph limit");
        --models_left;
        const auto name=r.name(true);const auto resource=r.name(true);
        if(depth==0 && !resource.empty() && identity(name)!=identity(resource))
            invalid_save("root model resource identity");
        // 0042f3f0 constructs each entity's complete original PO graph before
        // applying saved shader names. Even authored children absent from the
        // saved topology have already made their first constructor-cache claim.
        // Reuse matching authored nodes; only changed/dynamic nodes need a build.
        if(resource.empty())m=Entity::Model{};
        else if(!std::equal(m.name.begin(),m.name.end(),resource.begin(),resource.end(),
            [](unsigned char a,unsigned char b) {
                if(a>='A'&&a<='Z')a+='a'-'A';
                if(b>='A'&&b<='Z')b+='a'-'A';
                return a==b;
            }))build_model(m,resource);
        m.name=name;
        m.visible=r.flag();m.world=r.geometry_matrix();m.attachment_part=r.count();
        m.renderable=r.flag();m.frustum_cull=r.flag();m.region=r.integer();if(m.region<0)invalid_save("hit region");
        if(r.flag()) {
            Attachment attachment;attachment.object=r.name();
            if(r.flag())attachment.bone=r.name();
            if(r.flag())attachment.transform=r.matrix();
            m.attachment=std::move(attachment);
            if(identity(resource)!=identity(m.attachment->object))invalid_save("attachment resource identity");
        } else m.attachment.reset();
        const auto count=r.count();if(count!=m.parts.size())invalid_save("model part count mismatch");
        for(auto& part:m.parts) {
            const auto mesh=r.name();if(identity(mesh)!=identity(part.mesh_name))invalid_save("model mesh mismatch");
            part.shader=r.name();resolve_part_material(part,0);const auto bones=r.count();
            if(bones!=part.pose.size())invalid_save("model bone count mismatch");
            for(auto& matrix:part.pose)matrix=r.geometry_matrix();
            part.trace_mask=r.u32();part.material=r.integer();if(part.material<-1)invalid_save("physical material ID");
            if(part.material>=0 && static_cast<std::size_t>(part.material)>=materials_.definitions().size())
                invalid_save("material ordinal bounds");
            part.visible=r.flag();
            // Charge vector growth too; part storage is stable after build_model.
            if(material_allocation_budget<2*sizeof(PartMaterialSnapshot))
                invalid_save("part material snapshot allocation bounds");
            material_allocation_budget-=2*sizeof(PartMaterialSnapshot);
            material_snapshots.emplace_back(&part,read_mutable_material(r,material_allocation_budget));
        }
        const auto children=r.count();
        if(children>models_left || children>r.remaining/87)invalid_save("attachment allocation bounds");
        m.linked.resize(children);
        for(auto& child:m.linked) {
            self(self,child,depth+1);
            if(!child.attachment || child.attachment_part>=m.parts.size())invalid_save("invalid attachment parent");
            if(child.attachment->bone) {
                const auto* parent_mesh=m.parts[child.attachment_part].mesh;
                if(!parent_mesh)invalid_save("attachment parent has no mesh");
                const auto& bones=parent_mesh->bones;
                if(std::none_of(bones.begin(),bones.end(),[&](const auto& bone){return identity(bone.name)==identity(*child.attachment->bone);}))
                    invalid_save("attachment bone");
            }
        }
    };
    const auto entity_count=r.count();
    for(std::uint32_t index=0;index<entity_count;++index) {
        entities_.emplace_back();auto& e=entities_.back();
        e.handle=r.u32();if(e.handle!=index+1)invalid_save("entity ID sequence");
        e.name=r.name();e.object_name=r.name(true);e.kind=r.integer();e.alive=r.flag();e.visible=r.flag();
        if(e.kind<0 || e.kind>3)invalid_save("entity kind");
        e.position=r.geometry_vector();e.orientation=r.geometry_matrix();e.camera_fov=r.real();e.near_plane=r.real();e.far_plane=r.real();
        e.camera_fov_explicit=r.flag();
        if(e.kind!=0 && (!std::isfinite(e.position.x) || !std::isfinite(e.position.y) || !std::isfinite(e.position.z) ||
           std::any_of(e.orientation.begin(),e.orientation.end(),[](float word){return !std::isfinite(word);})))
            invalid_save("nonfinite actor/camera orientation");
        if(e.camera_fov<=0 || e.camera_fov>=180 || e.near_plane<=0 || e.far_plane<=e.near_plane)invalid_save("entity camera ranges");
        e.pickup_kind=r.integer();for(auto& value:e.pickup_weapon)value=r.integer();
        e.pickup_ammunition_type=r.integer();e.pickup_ammunition_count=r.integer();e.pickup_health=r.real();
        if(e.pickup_kind<0 || e.pickup_kind>3 || e.pickup_ammunition_type<0 || e.pickup_ammunition_type>4 ||
           e.pickup_ammunition_count<0 || e.pickup_health<0 ||
           std::any_of(e.pickup_weapon.begin(),e.pickup_weapon.end(),[](int value){return value<0;}))
            invalid_save("pickup ranges");
        e.health=r.real();e.explosion_damage=r.real();e.explosion_radius=r.real();e.destruction_started=r.flag();
        if(e.health<0 || e.explosion_damage<0 || e.explosion_radius<0)invalid_save("prop health/damage ranges");
        model(model,e.model,0);
        if(identity(e.object_name)!=identity(e.model.name))invalid_save("entity model identity");
    }
    const auto valid_handle=[&](std::uint32_t handle,bool nullable=false,bool live=true) {
        if(nullable && handle==0)return;
        if(handle==0 || handle>entities_.size() || (live && !entities_[handle-1].alive))invalid_save("dangling entity ID");
    };
    valid_handle(player_,true);
    valid_handle(selected_camera_,true);
    valid_handle(main_camera_,true);
    if(main_camera_ && entities_[main_camera_-1].kind!=3)invalid_save("main camera kind");
    if(next_entity_<=entities_.size())invalid_save("next entity ID");
    const auto queued=r.count();std::set<std::uint32_t> explosion_ids;
    for(std::uint32_t i=0;i<queued;++i) {
        QueuedExplosion explosion;explosion.entity=r.u32();valid_handle(explosion.entity);
        explosion.position=r.geometry_vector();explosion.radius=r.real();explosion.damage=r.real();
        if(entities_[explosion.entity-1].kind!=0 || explosion.radius<0 || explosion.damage<0 ||
           !explosion_ids.insert(explosion.entity).second)invalid_save("queued explosion state");
        explosions_.push_back(explosion);
    }
    r.section([&](auto& stream){actors_.load(stream);});
    for(const auto& actor:actors_.actors()) {
        if(actor.retired) {
            if(!actor.entity || actor.entity>entities_.size())invalid_save("retired actor ID");
        } else {
            valid_handle(actor.entity);
            const auto expected=actor.kind==ActorKind::player?2:1;
            if(entities_[actor.entity-1].kind!=expected)invalid_save("actor/entity kind mismatch");
        }
        if(player_==actor.entity && actor.kind!=ActorKind::player)invalid_save("player actor kind");
    }
    if(player_ && !actors_.find(player_))invalid_save("missing player actor");
    const auto* selected_player=actors_.player();
    if(player_!=(selected_player?selected_player->entity:0))invalid_save("native player link mismatch");
    for(const auto& actor:actors_.actors()) {
        auto& e=entities_[actor.entity-1];
        if(!e.model.parts.empty())e.model.parts.front().runtime_material.reset();
        configure_actor_material(e);
        if(actor.combat_state==4 || actor.combat_state==5)configure_corpse(e);
    }
    for(auto& [part,snapshot]:material_snapshots) {
        if(snapshot && !part->runtime_material)
            part->runtime_material.emplace(materials_.construct(part->shader,0,0,true));
        apply_mutable_material(part->runtime_material,std::move(snapshot));
    }
    r.section([&](auto& stream){effects_.load(stream);});
    r.section([&](auto& stream){
        physical_materials_.load(stream);
        // This codec is formatted text: consume its writer's final delimiters,
        // while section() still rejects any remaining non-grammar bytes.
        stream>>std::ws;
    });
    const auto characters=r.count();std::set<std::pair<std::uint32_t,int>> channels;
    if(characters>entities_.size()*6 || characters>r.remaining/54)
        invalid_save("character allocation bounds");
    for(std::uint32_t i=0;i<characters;++i) {
        CharacterPlayback p;p.entity=r.u32();p.clip=r.name();p.elapsed=r.real();p.loop=r.flag();
        p.channel=r.integer();p.blend=r.real();p.active=r.flag();valid_handle(p.entity,false,p.active);
        p.start_tick=game_tick_-r.u32();p.animation_mode=r.u32();
        p.flags=r.u32();p.pause_tick=game_tick_-r.u32();p.sample_tick=r.u32();
        p.fade_tick=game_tick_-r.u32();p.master_channel=r.integer();
        p.master_clip=r.name(true);p.coefficient=r.real();
        if(!p.master_clip.empty())(void)assets_.contains(p.master_clip);
        const auto status=p.flags&3u;
        if(p.elapsed<0 || p.animation_mode>4 || p.channel<0 || p.channel>=6 || p.blend<0 || p.blend>1 ||
           !channels.emplace(p.entity,p.channel).second || !status || p.flags>31 ||
           (p.flags&12u)==12u || p.active!=(status!=3) || (status==3 && p.flags!=3) ||
           p.coefficient<0 || p.coefficient>1 ||
           (p.master_channel!=-1 && p.master_channel!=2 && p.master_channel!=3) ||
           ((p.flags&16u) && (p.channel<4 || p.master_channel==-1 || p.master_clip.empty())))
            invalid_save("character progress");
        const auto key=identity(p.clip);auto found=animations_.find(key);
        if(found==animations_.end())found=animations_.emplace(key,read_animation(assets_.path(p.clip))).first;
        auto& e=entities_[p.entity-1];
        const auto evaluate=[&](auto&& self,Entity::Model& m)->void {
            for(auto& part:m.parts) {
                if(!part.mesh)continue;
                part.layer_evaluators[p.channel].emplace(*part.mesh,&found->second);
                part.layer_animations[p.channel]=&found->second;
            }
            for(auto& child:m.linked)self(self,child);
        };
        evaluate(evaluate,e.model); // Reset slots still retain their original bound PA.
        character_playbacks_.push_back(std::move(p));
    }
    // Master identity is entity+actual slot, never a pointer into the playback
    // vector. The dependency is resolved only after every saved slot exists.
    for(const auto& p:character_playbacks_) {
        if(!p.active || !(p.flags&16u))continue;
        const auto master=std::find_if(character_playbacks_.begin(),character_playbacks_.end(),[&](const auto& candidate) {
            return candidate.entity==p.entity && candidate.channel==p.master_channel;
        });
        if(master==character_playbacks_.end())
            invalid_save("character master reference");
    }
    const auto keyframe_count=r.count();
    for(std::uint32_t i=0;i<keyframe_count;++i) {
        KeyframePlayback p;p.name=r.name();p.linked=r.name(true);p.event=r.name(true);
        p.duration=r.real();p.start_tick=game_tick_-r.u32();p.frozen_phase=r.u32();
        p.mode=r.integer();p.repeats=r.integer();p.completed=r.integer();p.active=r.flag();p.paused=r.flag();
        p.pause_dirty=r.flag();p.camera_selected=r.flag();
        if(p.duration<0 || p.mode<0 || p.mode>3 || p.repeats<0 || p.completed<0)invalid_save("keyframe progress");
        const auto key=identity(p.name);auto found=keyframes_.find(key);
        if(found==keyframes_.end())found=keyframes_.emplace(key,read_keyframes(assets_.path("object/anim/pka/"+p.name+".pka"))).first;
        p.clip=&found->second;
        p.duration=restored_keyframe_duration(p.duration,*p.clip);
        const auto targets=r.count();p.targets.reserve(targets);
        for(std::uint32_t j=0;j<targets;++j) {const auto handle=r.u32();valid_handle(handle,false,false);p.targets.push_back(handle);}
        const auto tracks=r.count();if(tracks!=targets)invalid_save("keyframe track count");
        p.target_tracks.reserve(tracks);
        for(std::uint32_t j=0;j<tracks;++j) {
            const auto track=r.count();if(track>=p.clip->tracks.size())invalid_save("keyframe track ID");
            p.target_tracks.push_back(track);
        }
        const auto bases=r.count();if(bases!=targets)invalid_save("keyframe base count");
        p.target_base.reserve(bases);for(std::uint32_t j=0;j<bases;++j)p.target_base.push_back(r.geometry_matrix());
        p.previous_camera=r.u32();p.camera_target=r.u32();
        valid_handle(p.previous_camera,true,p.active);valid_handle(p.camera_target,true,p.active);
        const auto transforms=r.count();if(transforms!=p.clip->tracks.size())invalid_save("keyframe transform count");
        p.transforms.reserve(transforms);for(std::uint32_t j=0;j<transforms;++j)p.transforms.push_back(r.geometry_matrix());
        keyframe_playbacks_.push_back(std::move(p));
    }
    const auto event_count=r.count();std::set<std::uint64_t> serials;
    for(std::uint32_t i=0;i<event_count;++i) {
        TimedEvent event;event.name=r.name();event.deadline=time_+r.real64();event.serial=r.u64();
        if(!std::isfinite(event.deadline) || !serials.insert(event.serial).second)invalid_save("timed event ordering");
        events_.push_back(std::move(event));
    }
    for(const auto& event:events_)if(event.serial>next_event_serial_)invalid_save("event serial counter");
    const auto count=r.count();if(count!=triggers_.inside.size())invalid_save("trigger state count");
    for(auto& inside:triggers_.inside)inside=r.flag();
    const auto active=r.count(count);
    for(std::uint32_t i=0;i<active;++i) {
        const auto index=r.u32();const auto elapsed=r.u32();
        if(index>=count || !triggers_.inside[index] || triggers_.encounter_stamps[index])
            invalid_save("active trigger ID/order");
        triggers_.encounter_stamps[index]=1;triggers_.active.push_back(index);
        triggers_.last_ticks[index]=game_tick_-elapsed;
    }
    for(std::uint32_t index=0;index<count;++index) {
        if(static_cast<bool>(triggers_.inside[index])!=static_cast<bool>(triggers_.encounter_stamps[index]))
            invalid_save("trigger membership mismatch");
        triggers_.encounter_stamps[index]=0;
    }
    const auto sound_count=r.count();std::set<std::uint64_t> channel_ids;
    for(std::uint32_t i=0;i<sound_count;++i) {
        SoundSource sound;sound.name=r.name();sound.path=r.name();sound.position=r.vector();sound.volume=r.u8();
        sound.loop=r.flag();sound.spatial=r.flag();sound.stream=r.flag();
        sound.min_distance=r.real();if(sound.min_distance<0)invalid_save("source minimum distance");
        const auto channels=r.count(4096);sound.channels.reserve(channels);
        // Authored sources can outlive unsuccessful/disabled playback (including
        // surlar_1's missing music). Preserve their safe names without requiring
        // an asset until a live channel exists; actual Media state stays strict.
        if(channels)assets_.path(sound.path);
        else (void)assets_.contains(sound.path); // Validate Windows path syntax even when absent.
        for(std::uint32_t j=0;j<channels;++j) {
            const auto channel=r.u64();if(!channel || !channel_ids.insert(channel).second)invalid_save("audio channel IDs");
            sound.channels.push_back(channel);
        }
        sounds_.push_back(std::move(sound));
    }
    const auto variables=r.count();
    for(std::uint32_t i=0;i<variables;++i) {
        auto name=r.name();ScriptVariable variable;variable.type=r.integer();variable.value=r.u32();
        if(variable.type<0 || variable.type>2)invalid_save("static variable type");
        variable.owns_string=r.flag();variable.text=r.text();
        if(!variable.owns_string && !variable.text.empty())invalid_save("static variable string ownership");
        if(variable.owns_string)variable.value=runtime_.allocate_string(variable.text);
        if(!variables_.emplace(identity(name),std::move(variable)).second)invalid_save("duplicate static variable");
    }
    saved_media_=read_media(r);saved_interface_=read_interface(r);
    media_.validate_state(*saved_media_);interface_.validate_state(*saved_interface_);
    std::set<std::uint64_t> live_audio;
    for(const auto& sound:saved_media_->sounds)live_audio.insert(sound.id);
    for(const auto& sound:sounds_)for(auto id:sound.channels)
        if(!live_audio.contains(id))invalid_save("missing named audio channel");
    if(r.remaining!=0)invalid_save("trailing payload bytes");
}

void Game::save(const std::filesystem::path& destination) const {
    save(destination,SnapshotPurpose::exact);
}
void Game::save(const std::filesystem::path& destination,SnapshotPurpose purpose) const {
    std::ostringstream payload(std::ios::binary);write_save(payload,purpose);const auto bytes=payload.view();
    std::array<unsigned char,SHA256_DIGEST_LENGTH> digest{};
    if(!SHA256(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size(),digest.data()))
        throw std::runtime_error("Cannot hash native save payload");
    std::ostringstream output(std::ios::binary);SaveWriter writer{output};
    writer.bytes(save_magic.data(),save_magic.size());writer.count(bytes.size(),save_limit-44);
    writer.bytes(reinterpret_cast<const char*>(digest.data()),digest.size());
    atomic_output(destination,output.view(),bytes);
}

void Game::swap_saved_state(Game& other) noexcept {
    using std::swap;
    actors_.swap_state(other.actors_);effects_.swap_state(other.effects_);
    physical_materials_.swap_state(other.physical_materials_);
    swap(level_,other.level_);swap(collision_,other.collision_);
    meshes_.swap(other.meshes_);objects_.swap(other.objects_);
    animations_.swap(other.animations_);keyframes_.swap(other.keyframes_);
    entities_.swap(other.entities_);events_.swap(other.events_);
    leaf_entities_.swap(other.leaf_entities_);membership_seen_.swap(other.membership_seen_);
    membership_stack_.swap(other.membership_stack_);swap(membership_stamp_,other.membership_stamp_);
    swap(membership_capacity_,other.membership_capacity_);swap(fallback_leaf_,other.fallback_leaf_);
    explosions_.swap(other.explosions_);explosion_candidates_.clear();
    character_playbacks_.swap(other.character_playbacks_);keyframe_playbacks_.swap(other.keyframe_playbacks_);
    sounds_.swap(other.sounds_);variables_.swap(other.variables_);language_.swap(other.language_);
    draws_.swap(other.draws_);particles_.swap(other.particles_);
    actor_events_.swap(other.actor_events_);swap(camera_,other.camera_);
    swap(saved_media_,other.saved_media_);swap(saved_interface_,other.saved_interface_);
    swap(level_name_,other.level_name_);swap(time_,other.time_);swap(clock_,other.clock_);
    swap(bsp_path_,other.bsp_path_);
    swap(player_,other.player_);swap(camera_fov_,other.camera_fov_);
    swap(menu_,other.menu_);swap(quit_,other.quit_);swap(processing_,other.processing_);
    swap(input_processing_,other.input_processing_);swap(rendering_,other.rendering_);
    swap(use_only_,other.use_only_);swap(cinematic_,other.cinematic_);swap(hud_visible_,other.hud_visible_);
    swap(pending_level_,other.pending_level_);swap(pending_image_,other.pending_image_);swap(pending_checkpoint_,other.pending_checkpoint_);
    swap(sound_set_,other.sound_set_);swap(music_,other.music_);swap(fullscreen_shader_,other.fullscreen_shader_);
    swap(subtitle_shader_,other.subtitle_shader_);swap(subtitles_,other.subtitles_);
    swap(triggers_,other.triggers_);swap(game_tick_,other.game_tick_);frame_stamp_=0;
    swap(next_entity_,other.next_entity_);swap(next_tick_,other.next_tick_);swap(main_camera_,other.main_camera_);
    swap(next_event_serial_,other.next_event_serial_);swap(next_lighting_id_,other.next_lighting_id_);
    effects_resource_revision_=0;
    swap(selected_camera_,other.selected_camera_);swap(fade_seconds_,other.fade_seconds_);
    swap(environment_,other.environment_);swap(fog_type_,other.fog_type_);swap(fog_color_,other.fog_color_);
    swap(fog_start_,other.fog_start_);swap(fog_end_,other.fog_end_);swap(fog_density_,other.fog_density_);
    swap(no_weapon_,other.no_weapon_);swap(fog_enabled_,other.fog_enabled_);
    swap(fullscreen_,other.fullscreen_);swap(subtitle_,other.subtitle_);swap(fade_in_,other.fade_in_);
    swap(prerender_pending_,other.prerender_pending_);
    swap(scheduler_ready_,other.scheduler_ready_);swap(player_effect_,other.player_effect_);
    ++player_effect_serial_; // Reset live GPU history identity; never restore a stale saved serial.
    swap(camera_blocked_,other.camera_blocked_);swap(camera_transition_tick_,other.camera_transition_tick_);
    pending_mouse_={}; // Native device deltas do not survive a checkpoint restore.
    scene_changed_=true;++scene_generation_;
}

void Game::load(const std::filesystem::path& source) {
    load(source,SnapshotPurpose::exact);
}
void Game::load_checkpoint(const std::filesystem::path& source) {
    load(source,SnapshotPurpose::checkpoint);
}
void Game::load(const std::filesystem::path& source,SnapshotPurpose purpose) {
    std::istringstream envelope(file_bytes(source),std::ios::binary);
    validate_envelope(envelope);
    // Scratch reconstruction uses the shared shader epoch; rejected loads must
    // restore the live epoch without copying it or persisting constructor history.
    struct RuntimeMaskRestore {
        const MaterialLibrary& materials;
        MaterialLibrary::RuntimeMasks previous;
        bool committed{};
        ~RuntimeMaskRestore() { if(!committed)materials.swap_runtime_masks(previous); }
    } runtime_masks{materials_,materials_.take_runtime_masks()};
    Game scratch(assets_,materials_,settings_,interface_,media_);
    scratch.read_save(envelope);
    scratch.load_language();
    register_hosts(scratch.runtime_);
    const auto program=[&](const std::string& name) {
        logical_name(name);
        return script::Program::decode(assets_.bytes("level/pcs/"+name+".pcs"),name);
    };
    scratch.runtime_.load(program(scratch.level_name_),program);
    scratch.runtime_.reset_stack();
    // Restore-specific 00435c40 removes startup and suppresses its deferred hook.
    scratch.runtime_.remove_callable("on_start");
    scratch.prerender_pending_=false;
    if(purpose==SnapshotPurpose::checkpoint) {
        // 0042f3f0 resumes gameplay, not the writer's suspended host/callback.
        // Apply only to staged state: scratch shares the live Interface/Media.
        scratch.processing_=scratch.input_processing_=scratch.rendering_=true;
        scratch.menu_=false;
        scratch.cinematic_=scratch.camera_blocked_=false;
        // Original camera exit tests the pre-load live global. Preserve its
        // age when already inactive, rebased into our restored logical clock.
        scratch.camera_transition_tick_=scratch.game_tick_-
            (camera_blocked_?0u:game_tick_-camera_transition_tick_);
        scratch.hud_visible_=true;
        scratch.actors_.set_cinematic(false); // Original reader re-enables AI.
        scratch.actors_.set_player_input_mode(true,scratch.use_only_,false);
        scratch.fade_in_=false;scratch.fade_seconds_=.5f; // DATA00475474.
        scratch.fullscreen_=false; // Final reader00431c40, independent of fade.
        auto& ui=*scratch.saved_interface_;
        ui.hud_visible=true;ui.fullscreen_visible=false;
        ui.fade_in=false;ui.fade_duration=.5f;ui.fade_elapsed=0;
        ui.fade_alpha=1;ui.fading=true;
        // Original fresh dispatch clears the outgoing slot and scheduler,
        // never the saved world timers or six-controller continuation state.
        scratch.pending_level_.clear();
        scratch.pending_image_=scratch.pending_checkpoint_=false;
        scratch.next_tick_=0;scratch.scheduler_ready_=false;
    }
    scratch.viewport_width_=viewport_width_;scratch.viewport_height_=viewport_height_;
    // Reconstructing transient weapon projection must not consume a live tick.
    const auto random_state=scratch.actors_.random_state();
    scratch.rebuild_frame();
    scratch.actors_.set_random_state(random_state);
    scratch.clear_membership();
    for(auto& e:scratch.entities_)scratch.refresh_membership(e);
    // All bounded parsing and asset/script construction complete before live
    // aliases or gameplay state are touched. Reloaded hosts capture this Game.
    runtime_.swap_state(scratch.runtime_); // Quiescence is checked before any mutation.
    swap_saved_state(scratch);
    runtime_masks.committed=true;
    media_.restore_state(*saved_media_);
    interface_.restore_state(*saved_interface_);
    interface_.set_game_active(level_!=nullptr);
    if(menu_)interface_.show_menu();else interface_.hide_menu();
    saved_media_.reset();saved_interface_.reset();
    // 0042f3f0: restored scene -> loading .9 -> 004088b0/00406340.
    // The swaps retain the staged frame's borrowed storage; frame() uses the live generation.
    if(loading_presenter_)loading_presenter_(loading_context_,loading_shader_,.9f);
    if(scene_preparer_)scene_preparer_(loading_context_,frame(),bsp_path_);
    if(loading_presenter_)loading_presenter_(loading_context_,loading_shader_,1);
}

void Game::save_checkpoint(std::string_view name) {
    // 0042d340 ->0042b490 completes before the next script instruction.
    save(checkpoint_path(name),SnapshotPurpose::checkpoint);
}
void Game::save_player_stats() {
    std::ostringstream output(std::ios::binary);actors_.save_player_stats(output,player_);
    atomic_output(user_data_directory()/"save"/"player.psv",output.view());
}
void Game::load_player_stats() {
    const auto path=user_data_directory()/"save"/"player.psv";
    if(!std::filesystem::exists(path))return; // Original reader leaves stats unchanged when absent.
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open player stats: "+path.string());
    const auto size=input.tellg();
    if(size<0 || static_cast<std::uint64_t>(size)>save_limit)invalid_save("playerstat file size limit");
    input.seekg(0);
    // ActorRuntime stages/validates the complete original .psv block including
    // EOF before committing only stats, preserving its position alias and timers.
    actors_.load_player_stats(input,player_);
}

} // namespace pusu
