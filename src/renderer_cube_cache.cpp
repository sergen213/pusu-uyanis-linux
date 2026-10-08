#include "renderer_cube_cache.hpp"
#include "resources.hpp"
#include "scene_math.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>

namespace pusu {
using CubeCacheFd=cube_cache_detail::Fd;
std::uint32_t cube_read_word(std::span<const std::uint8_t> bytes,std::size_t offset) {
    if(offset>bytes.size()||bytes.size()-offset<4)throw CubeCacheValidationError("Cube cache metadata has a truncated DWORD");
    return std::uint32_t(bytes[offset])|(std::uint32_t(bytes[offset+1])<<8)|
        (std::uint32_t(bytes[offset+2])<<16)|(std::uint32_t(bytes[offset+3])<<24);
}
std::vector<CubeCacheEntry> cube_cache_entries(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<16||bytes.size()%16)throw CubeCacheValidationError("Cube cache metadata must contain complete 16-byte records and a sentinel");
    std::vector<CubeCacheEntry> entries;entries.reserve(bytes.size()/16-1);
    for(std::size_t offset=0;offset<bytes.size();offset+=16){
        std::uint32_t x=cube_read_word(bytes,offset),y=cube_read_word(bytes,offset+4),
            z=cube_read_word(bytes,offset+8),word=cube_read_word(bytes,offset+12);
        if(word==99999999){
            if(x!=99999999||y!=99999999||z!=99999999||offset!=bytes.size()-16)
                throw CubeCacheValidationError("Cube cache metadata has an incomplete or nonterminal sentinel");
            return entries;
        }
        Vec3 center{std::bit_cast<float>(x),std::bit_cast<float>(y),std::bit_cast<float>(z)};
        if(std::bit_cast<std::int32_t>(word)<0)throw CubeCacheValidationError("Cube cache metadata contains a negative RGB offset");
        entries.push_back({center,word});
    }
    throw CubeCacheValidationError("Cube cache metadata is missing its terminal sentinel");
}
int cube_cache_quality(std::string_view marker) {
    marker=marker.substr(0,marker.find_first_of("\r\n"));
    auto first=marker.find_first_not_of(" \t\v\f");
    if(first==std::string_view::npos)throw CubeCacheValidationError("Cube cache quality marker has no decimal token on its first line");
    marker.remove_prefix(first);marker=marker.substr(0,marker.find_first_of(" \t\v\f"));
    if(marker.front()=='+')marker.remove_prefix(1);
    int quality{};auto parsed=std::from_chars(marker.data(),marker.data()+marker.size(),quality,10);
    if(parsed.ec!=std::errc{}||parsed.ptr!=marker.data()+marker.size()||quality<=0)
        throw CubeCacheValidationError("Cube cache quality marker must start with a bounded positive decimal token");
    return quality;
}
std::optional<CubeCache> read_cube_cache(AssetStore& assets,std::string_view cache_base,int reflection_divisor) {
    std::string base(cache_base),marker_name=base+".txt",info_name=base+"_info.txt";
    if(!assets.contains(base)||!assets.contains(marker_name)||!assets.contains(info_name))return std::nullopt;
    if(cube_cache_quality(assets.text(marker_name))!=reflection_divisor)return std::nullopt;
    auto info=assets.bytes(info_name);
    CubeCache cache{cube_cache_entries(info),assets.bytes(base)};
    for(const auto& entry:cache.entries)if(entry.offset>=cache.rgb.size())
        throw CubeCacheValidationError("Cube cache RGB offset is outside payload: "+base);
    return cache;
}
std::uint32_t cube_cache_offset(const CubeCache& cache,Vec3 center) {
    constexpr long double tolerance=std::bit_cast<float>(std::uint32_t{0x3dcccccd});
    for(const auto& entry:cache.entries)
        if(std::abs(static_cast<long double>(entry.center.x)-center.x)<tolerance&&
           std::abs(static_cast<long double>(entry.center.y)-center.y)<tolerance&&
           std::abs(static_cast<long double>(entry.center.z)-center.z)<tolerance)return entry.offset;
    // The original defines offset zero for no match; it is not a recapture request.
    return 0;
}
void validate_cube_cache_geometry(const CubeCache& cache) {
    if(cache.entries.empty()){
        if(!cache.rgb.empty())throw CubeCacheValidationError("Cube cache RGB payload has no geometry records");
        return;
    }
    if(cache.entries.front().offset!=0)throw CubeCacheValidationError("Cube cache geometry must start at RGB offset zero");
    for(std::size_t i=0;i<cache.entries.size();++i){
        const std::size_t start=cache.entries[i].offset,end=i+1<cache.entries.size()?cache.entries[i+1].offset:cache.rgb.size();
        if(end<=start||end>cache.rgb.size())throw CubeCacheValidationError("Cube cache geometry offsets must be strictly ordered inside the payload");
        const std::size_t bytes=end-start;
        if(bytes%18)throw CubeCacheValidationError("Cube cache geometry region must contain six complete RGB faces");
        const std::size_t pixels=bytes/18;
        std::size_t low=1,high=pixels;bool square=false;
        while(low<=high){
            const std::size_t side=low+(high-low)/2,quotient=pixels/side;
            if(side==quotient&&pixels%side==0){square=true;break;}
            if(side>quotient)high=side-1;else low=side+1;
        }
        if(!square)throw CubeCacheValidationError("Cube cache geometry region must contain square RGB faces");
    }
}
std::size_t cube_cache_region_bytes(const CubeCache& cache,std::uint32_t offset) {
    for(std::size_t i=0;i<cache.entries.size();++i)if(cache.entries[i].offset==offset){
        const std::size_t end=i+1<cache.entries.size()?cache.entries[i+1].offset:cache.rgb.size();
        return end-offset;
    }
    throw CubeCacheValidationError("Cube cache RGB offset does not identify a geometry record");
}
std::span<const std::uint8_t> cube_cache_faces_at_offset(const CubeCache& cache,std::uint32_t offset,int side) {
    if(side<=0)throw CubeCacheValidationError("Cube cache face side must be positive");
    const std::size_t size=std::size_t(side),maximum=std::numeric_limits<std::size_t>::max();
    if(size>maximum/size||size*size>maximum/18)throw CubeCacheValidationError("Cube cache six-face RGB size overflows");
    const std::size_t count=size*size*18;
    if(offset>cache.rgb.size()||count>cache.rgb.size()-offset)
        throw CubeCacheValidationError("Cube cache payload does not contain all six requested RGB faces");
    return std::span<const std::uint8_t>(cache.rgb).subspan(offset,count);
}
std::span<const std::uint8_t> cube_cache_faces(const CubeCache& cache,Vec3 center,int side) {
    return cube_cache_faces_at_offset(cache,cube_cache_offset(cache,center),side);
}
std::span<const std::uint8_t> owned_cube_cache_faces(const CubeCache& cache,std::size_t index,Vec3 center,int side) {
    // Owned publication and the side-directory key share capture-registry order.
    // Original imported lookup remains positional: coincident centers need not identify the same cube.
    if(index>=cache.entries.size())throw CubeCacheValidationError("Owned cube cache is missing a capture record");
    const auto& entry=cache.entries[index];
    if(std::bit_cast<std::uint32_t>(entry.center.x)!=std::bit_cast<std::uint32_t>(center.x)||
       std::bit_cast<std::uint32_t>(entry.center.y)!=std::bit_cast<std::uint32_t>(center.y)||
       std::bit_cast<std::uint32_t>(entry.center.z)!=std::bit_cast<std::uint32_t>(center.z))
        throw CubeCacheValidationError("Owned cube cache record center differs from its capture registry");
    auto faces=cube_cache_faces_at_offset(cache,entry.offset,side);
    const std::size_t end=index+1<cache.entries.size()?cache.entries[index+1].offset:cache.rgb.size();
    if(end<entry.offset||end>cache.rgb.size()||end-entry.offset!=faces.size())
        throw CubeCacheValidationError("Owned cube cache record geometry differs from its directory shape");
    return faces;
}
namespace {
namespace fs = std::filesystem;
constexpr std::string_view cube_cache_owner = "Pusu native owned cubemap cache 1\n";
[[noreturn]] void cube_cache_io(std::string_view operation) {
    throw std::system_error(errno,std::generic_category(),std::string(operation));
}
void cube_cache_owned(const struct stat& status,bool directory) {
    if(status.st_uid!=::geteuid()||(directory?!S_ISDIR(status.st_mode):!S_ISREG(status.st_mode))||
       (status.st_mode&0022)||(!directory&&status.st_nlink!=1))
        throw std::runtime_error("Private cubemap cache has an unsafe owner, type, permissions, or hard link");
}
struct stat cube_cache_stat(int fd) {
    struct stat status{};if(::fstat(fd,&status)<0)cube_cache_io("stat private cubemap cache");return status;
}
void cube_cache_sync(int fd) {
    if(::fsync(fd)<0)cube_cache_io("fsync private cubemap cache");
}
void cube_cache_write(int fd,std::span<const std::uint8_t> bytes) {
    while(!bytes.empty()){
        ssize_t count=::write(fd,bytes.data(),bytes.size());
        if(count<0){if(errno==EINTR)continue;cube_cache_io("write private cubemap cache");}
        if(!count)throw std::runtime_error("Private cubemap cache write made no progress");
        bytes=bytes.subspan(std::size_t(count));
    }
}
void cube_cache_write(int fd,std::string_view text) {
    cube_cache_write(fd,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
template<class Visit> void cube_cache_visit(int fd,Visit visit) {
    int copy=::openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(copy<0)cube_cache_io("open private cubemap directory stream");
    DIR* directory=::fdopendir(copy);
    if(!directory){int error=errno;::close(copy);errno=error;cube_cache_io("read private cubemap directory");}
    struct Close { DIR* directory;~Close() noexcept { ::closedir(directory); } } close{directory};
    std::size_t count=0;
    for(;;){
        errno=0;auto* entry=::readdir(directory);
        if(!entry){if(errno)cube_cache_io("read private cubemap directory");break;}
        std::string_view name(entry->d_name);if(name=="."||name=="..")continue;
        if(++count>65536)throw std::runtime_error("Private cubemap state directory exceeds its entry bound");
        struct stat status{};
        if(::fstatat(fd,entry->d_name,&status,AT_SYMLINK_NOFOLLOW)<0)cube_cache_io("stat private cubemap entry");
        visit(name,status);
    }
}
bool cube_cache_component(std::string_view name) {
    if(name.empty()||name=="."||name==".."||name.ends_with(".lock")||name.ends_with(".pending"))return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c){
        return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
    });
}
CubeCacheFd cube_cache_directory(int parent,const char* name,bool create,bool owned=true) {
    int fd=::openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0&&errno==ENOENT&&create){
        if(::mkdirat(parent,name,0700)<0&&errno!=EEXIST)cube_cache_io("create private cubemap directory");
        cube_cache_sync(parent);
        fd=::openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    }
    if(fd<0){if(errno==ENOENT&&!create)return {};cube_cache_io("open private cubemap directory");}
    CubeCacheFd result(fd);if(owned)cube_cache_owned(cube_cache_stat(fd),true);return result;
}
void cube_cache_single_directory(int fd,std::string_view allowed) {
    cube_cache_visit(fd,[&](std::string_view name,const struct stat& status){
        if(name!=allowed)throw std::runtime_error("Unexpected entry in private cubemap cache root: "+std::string(name));
        cube_cache_owned(status,true);
    });
}
void cube_cache_check_stamp(int fd) {
    const auto status=cube_cache_stat(fd);cube_cache_owned(status,false);
    if((status.st_mode&0777)!=0600||status.st_size!=off_t(cube_cache_owner.size()))
        throw std::runtime_error("Private cubemap cache lock has no valid ownership stamp");
    std::array<char,64> bytes{};ssize_t count;
    do{count=::pread(fd,bytes.data(),cube_cache_owner.size(),0);}while(count<0&&errno==EINTR);
    if(count<0)cube_cache_io("read private cubemap ownership stamp");
    if(count!=ssize_t(cube_cache_owner.size())||std::string_view(bytes.data(),std::size_t(count))!=cube_cache_owner)
        throw std::runtime_error("Private cubemap cache lock has a foreign ownership stamp");
}
void cube_cache_state_directory(int fd) {
    cube_cache_visit(fd,[&](std::string_view name,const struct stat& status){
        if(S_ISDIR(status.st_mode)){
            cube_cache_owned(status,true);
            if(name.ends_with(".pending")){
                std::string base(name.substr(0,name.size()-8)),lock=base+".lock";struct stat stamp{};
                if(!cube_cache_component(base)||::fstatat(fd,lock.c_str(),&stamp,AT_SYMLINK_NOFOLLOW)<0)
                    throw std::runtime_error("Unowned pending private cubemap cache directory");
                cube_cache_owned(stamp,false);
            }else if(!cube_cache_component(name))throw std::runtime_error("Unexpected private cubemap state directory");
            return;
        }
        if(!name.ends_with(".lock")||!cube_cache_component(name.substr(0,name.size()-5)))
            throw std::runtime_error("Unexpected private cubemap state entry: "+std::string(name));
        cube_cache_owned(status,false);
        std::string lock_name(name);CubeCacheFd lock(::openat(fd,lock_name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
        if(lock.value<0)cube_cache_io("open private cubemap ownership lock");
        if(::flock(lock.value,LOCK_SH|LOCK_NB)<0){
            if(errno==EWOULDBLOCK||errno==EAGAIN)return;
            cube_cache_io("lock private cubemap ownership stamp");
        }
        cube_cache_check_stamp(lock.value);
    });
}
std::array<std::string,3> cube_cache_names(std::string_view basename) {
    return {std::string(basename),std::string(basename)+"_info.txt",std::string(basename)+".txt"};
}
unsigned cube_cache_leaf(int fd,const std::array<std::string,3>& names) {
    unsigned present=0;
    cube_cache_visit(fd,[&](std::string_view name,const struct stat& status){
        auto found=std::find(names.begin(),names.end(),name);
        if(found==names.end())throw std::runtime_error("Unexpected entry in owned private cubemap leaf: "+std::string(name));
        cube_cache_owned(status,false);present|=1u<<std::size_t(found-names.begin());
    });return present;
}
void cube_cache_remove_stage(int parent,const std::string& name,int stage,const std::array<std::string,3>& files) {
    struct stat current{};
    if(::fstatat(parent,name.c_str(),&current,AT_SYMLINK_NOFOLLOW)<0)cube_cache_io("stat private cubemap pending directory");
    const auto opened=cube_cache_stat(stage);
    if(current.st_dev!=opened.st_dev||current.st_ino!=opened.st_ino)
        throw std::runtime_error("Private cubemap pending directory changed during transaction");
    cube_cache_leaf(stage,files);
    for(const auto& file:files)if(::unlinkat(stage,file.c_str(),0)<0&&errno!=ENOENT)
        cube_cache_io("remove owned private cubemap staging file");
    cube_cache_sync(stage);
    if(::unlinkat(parent,name.c_str(),AT_REMOVEDIR)<0)cube_cache_io("remove owned private cubemap staging directory");
    cube_cache_sync(parent);
}
}
CubeCacheOutput::CubeCacheOutput(CubeCacheFd parent,CubeCacheFd lock,const std::string& leaf_name,std::string_view basename):
        parent_(std::move(parent)),lock_(std::move(lock)),
        pending_(leaf_name+".pending"),names_(cube_cache_names(basename)) {
        final_=cube_cache_directory(parent_.value,leaf_name.c_str(),true);
        cube_cache_leaf(final_.value,names_);
        auto stale=cube_cache_directory(parent_.value,pending_.c_str(),false);
        if(stale.value>=0){
            cube_cache_leaf(stale.value,names_);
            std::cerr<<"Discarding stale owned private cubemap staging directory: "<<pending_<<'\n';
            cube_cache_remove_stage(parent_.value,pending_,stale.value,names_);
        }
        stage_=cube_cache_directory(parent_.value,pending_.c_str(),true);stage_owned_=true;
        try{
            for(unsigned i=0;i<files_.size();++i){
                files_[i]=CubeCacheFd(::openat(stage_.value,names_[i].c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
                if(files_[i].value<0)cube_cache_io("create private cubemap staging file");
            }
        }catch(...){cleanup();throw;}
    }
CubeCacheOutput::~CubeCacheOutput() noexcept { cleanup(); }
CubeCacheOutput::CubeCacheOutput(CubeCacheOutput&& other) noexcept:
        parent_(std::move(other.parent_)),lock_(std::move(other.lock_)),final_(std::move(other.final_)),
        stage_(std::move(other.stage_)),files_(std::move(other.files_)),
        pending_(std::move(other.pending_)),names_(std::move(other.names_)),offset_(other.offset_),
        info_size_(other.info_size_),face_size_(other.face_size_),faces_(other.faces_),
        has_cube_(other.has_cube_),stage_owned_(std::exchange(other.stage_owned_,false)),committed_(other.committed_) {}
void CubeCacheOutput::begin_cube(Vec3 center) {
        require_open();
        if(has_cube_&&faces_!=6)throw std::runtime_error("Private cubemap output requires six faces before the next center");
        if(offset_==99999999)
            throw std::runtime_error("Private cubemap output RGB offset collides with the metadata sentinel");
        if(info_size_>std::uint64_t(INT32_MAX)-32)throw std::runtime_error("Private cubemap metadata exceeds signed 32-bit bounds");
        write_record({std::bit_cast<std::uint32_t>(center.x),std::bit_cast<std::uint32_t>(center.y),
                      std::bit_cast<std::uint32_t>(center.z),std::uint32_t(offset_)});
        has_cube_=true;faces_=0;face_size_=0;
    }
void CubeCacheOutput::face(std::span<const std::uint8_t> rgb) {
        require_open();
        if(!has_cube_||faces_>=6||rgb.empty()||rgb.size()%3||(faces_&&rgb.size()!=face_size_))
            throw std::runtime_error("Private cubemap output requires six equally sized RGB faces per center");
        if(rgb.size()>std::uint64_t(INT32_MAX)-offset_)
            throw std::runtime_error("Private cubemap RGB payload exceeds signed 32-bit bounds");
        cube_cache_write(files_[0].value,rgb);offset_+=rgb.size();face_size_=rgb.size();++faces_;
    }
void CubeCacheOutput::commit(int divisor) {
        require_open();
        if(divisor<=0||(has_cube_&&faces_!=6))throw std::runtime_error("Cannot commit incomplete private cubemap output");
        write_record({99999999,99999999,99999999,99999999});
        std::array<char,32> decimal{};auto result=std::to_chars(decimal.data(),decimal.data()+decimal.size(),divisor);
        if(result.ec!=std::errc{})throw std::runtime_error("Cannot encode private cubemap quality");
        cube_cache_write(files_[2].value,std::string_view(decimal.data(),std::size_t(result.ptr-decimal.data())));
        for(const auto& file:files_)cube_cache_sync(file.value);
        cube_cache_sync(stage_.value);
        // The original quality file is the commit marker; readers hold the same lock.
        cube_cache_leaf(final_.value,names_);
        if(::unlinkat(final_.value,names_[2].c_str(),0)<0&&errno!=ENOENT)cube_cache_io("invalidate private cubemap quality marker");
        cube_cache_sync(final_.value);
        for(unsigned i=0;i<2;++i)if(::renameat(stage_.value,names_[i].c_str(),final_.value,names_[i].c_str())<0)
            cube_cache_io("publish private cubemap RGB or metadata");
        cube_cache_sync(final_.value);
        if(::renameat(stage_.value,names_[2].c_str(),final_.value,names_[2].c_str())<0)
            cube_cache_io("publish private cubemap quality marker");
        cube_cache_sync(final_.value);committed_=true;
        cube_cache_remove_stage(parent_.value,pending_,stage_.value,names_);stage_owned_=false;
    }
void CubeCacheOutput::require_open() const {
        if(!stage_owned_||committed_)throw std::runtime_error("Private cubemap output is not an active transaction");
    }
void CubeCacheOutput::write_record(std::array<std::uint32_t,4> words) {
        std::array<std::uint8_t,16> bytes{};
        for(unsigned i=0;i<4;++i)for(unsigned byte=0;byte<4;++byte)bytes[i*4+byte]=std::uint8_t(words[i]>>(byte*8));
        cube_cache_write(files_[1].value,bytes);info_size_+=bytes.size();
    }
void CubeCacheOutput::cleanup() noexcept {
        if(!stage_owned_)return;
        try{cube_cache_remove_stage(parent_.value,pending_,stage_.value,names_);stage_owned_=false;}
        catch(const std::exception& error){::fprintf(stderr,"Private cubemap staging cleanup refused: %s\n",error.what());}
    }

PrivateCubeCache::PrivateCubeCache(fs::path cache_root,std::string_view normalized_extensionless_bsp,std::string_view shape):
        root_(fs::absolute(std::move(cache_root))),level_(normalized_extensionless_bsp) {
        if(level_.empty()||level_.is_absolute())throw std::runtime_error("Private cubemap BSP cache key must be relative");
        std::size_t depth=0;
        for(const auto& part:level_)if(!cube_cache_component(part.string())||++depth>64)
            throw std::runtime_error("Private cubemap BSP cache key has an invalid component");
        basename_=level_.filename().string();
        fs::path shape_path(shape);
        if(shape_path.empty()||shape_path.is_absolute())throw std::runtime_error("Private cubemap shape key must be relative");
        for(const auto& part:shape_path)if(part.native().size()>200||!cube_cache_component(part.native()))
            throw std::runtime_error("Private cubemap shape key has an invalid component");
        level_/=shape_path;leaf_name_=level_.filename().string();dir_=root_/"textures"/"cubemaps"/level_;
        for(const auto& part:root_)if(part=="..")throw std::runtime_error("Private cubemap cache root contains parent traversal");
    }
std::optional<CubeCache> PrivateCubeCache::read(int reflection_divisor) {
        auto parent=open_parent(false);if(parent.value<0)return std::nullopt;
        auto lock=open_lock(parent.value,false);
        if(lock.value<0)return std::nullopt;
        if(::flock(lock.value,LOCK_SH|LOCK_NB)<0){
            if(errno!=EWOULDBLOCK&&errno!=EAGAIN)cube_cache_io("lock private cubemap cache for reading");
            std::cerr<<"Private cubemap cache is busy: "<<dir_<<'\n';return std::nullopt;
        }
        cube_cache_check_stamp(lock.value);
        auto leaf=cube_cache_directory(parent.value,leaf_name_.c_str(),false);if(leaf.value<0)return std::nullopt;
        auto names=cube_cache_names(basename_);
        if(cube_cache_leaf(leaf.value,names)!=7){
            std::cerr<<"Incomplete owned private cubemap cache will be regenerated: "<<dir_<<'\n';return std::nullopt;
        }
        AssetStore assets(dir_);
        try{return read_cube_cache(assets,basename_,reflection_divisor);}
        catch(const CubeCacheValidationError& error){
            std::cerr<<"Malformed owned private cubemap cache will be regenerated: "<<dir_<<": "<<error.what()<<'\n';
            return std::nullopt;
        }
    }
CubeCacheOutput PrivateCubeCache::begin_output() {
        bool created=false;
        auto parent=open_parent(true);auto lock=open_lock(parent.value,true,&created);
        if(::flock(lock.value,LOCK_EX|LOCK_NB)<0)cube_cache_io("lock private cubemap cache for output");
        if(created){
            cube_cache_write(lock.value,cube_cache_owner);cube_cache_sync(lock.value);cube_cache_sync(parent.value);
        }
        cube_cache_check_stamp(lock.value);
        return CubeCacheOutput(std::move(parent),std::move(lock),leaf_name_,basename_);
    }
CubeCacheFd PrivateCubeCache::open_parent(bool create) const {
        CubeCacheFd current(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
        if(current.value<0)cube_cache_io("open private cubemap filesystem root");
        for(const auto& part:root_.relative_path()){
            auto next=cube_cache_directory(current.value,part.c_str(),create,false);
            if(next.value<0)return {};current=std::move(next);
        }
        cube_cache_owned(cube_cache_stat(current.value),true);
        current=cube_cache_directory(current.value,"textures",create);if(current.value<0)return {};
        cube_cache_single_directory(current.value,"cubemaps");
        current=cube_cache_directory(current.value,"cubemaps",create);if(current.value<0)return {};
        cube_cache_state_directory(current.value);
        for(const auto& part:level_.parent_path()){
            current=cube_cache_directory(current.value,part.c_str(),create);if(current.value<0)return {};
            cube_cache_state_directory(current.value);
        }return current;
    }
CubeCacheFd PrivateCubeCache::open_lock(int parent,bool create,bool* created) const {
        std::string name=leaf_name_+".lock";
        CubeCacheFd lock(::openat(parent,name.c_str(),O_RDWR|O_NOFOLLOW|O_CLOEXEC));
        if(lock.value<0){
            if(errno!=ENOENT)cube_cache_io("open private cubemap ownership lock");
            struct stat existing{};
            for(const auto& candidate:{leaf_name_,leaf_name_+".pending"}){
                if(::fstatat(parent,candidate.c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0)
                    throw std::runtime_error("Refusing to adopt an unstamped private cubemap cache directory: "+candidate);
                if(errno!=ENOENT)cube_cache_io("stat private cubemap cache ownership");
            }
            if(!create)return {};
            lock=CubeCacheFd(::openat(parent,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
            if(lock.value<0)cube_cache_io("create private cubemap ownership lock");
            if(created)*created=true;
        }
        cube_cache_owned(cube_cache_stat(lock.value),false);return lock;
    }
} // namespace pusu
