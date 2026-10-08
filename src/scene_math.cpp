#include "scene_math.hpp"
#include "resources.hpp"

#include <algorithm>
#include <charconv>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pusu {

Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 operator*(Vec3 a, float b) noexcept { return {a.x*b,a.y*b,a.z*b}; }
Vec3 operator/(Vec3 a, float b) noexcept { return a*(1.f/b); }
float dot(Vec3 a, Vec3 b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b) noexcept { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float length(Vec3 a) noexcept { return std::sqrt(dot(a,a)); }
Vec3 normalized(Vec3 a) noexcept { const float n=length(a); return n>0 ? a/n : Vec3{}; }

float original_sqrt(float value) noexcept {
    // Original 00444d50 generates two quantized mantissa halves once.
    static std::array<std::uint32_t,65536> table{};
    static const bool initialized=[] {
        for(std::uint32_t i=0;i<32768;++i) {
            const auto mantissa=[](std::uint32_t bits) {
                return std::bit_cast<std::uint32_t>(std::sqrt(std::bit_cast<float>(bits)))&0x7fffffu;
            };
            table[i+32768]=mantissa((i|0x3f8000u)<<8);
            table[i]=mantissa((i|0x400000u)<<8);
        }
        return true;
    }();
    (void)initialized;
    const auto bits=std::bit_cast<std::uint32_t>(value);
    if(bits==0)return 0;
    return std::bit_cast<float>(((((bits+0xc0800000u)>>1)+0x3f800000u)&0x7f800000u)|
                                table[(bits>>8)&0xffffu]);
}
float original_length(Vec3 vector) noexcept {
    return original_sqrt((vector.x*vector.x+vector.y*vector.y)+vector.z*vector.z);
}
float original_distance(Vec3 a,Vec3 b) noexcept {
    const Vec3 delta=a-b;
    return original_sqrt((delta.z*delta.z+delta.y*delta.y)+delta.x*delta.x);
}
Vec3 original_normalized(Vec3 vector) noexcept {
    const float norm=original_length(vector);
    return norm!=0 ? vector*(1.f/norm) : vector;
}
Vec3 interpolate_actor_position(Vec3 previous,Vec3 current,std::uint32_t last_tick,
                                std::uint32_t render_tick,std::uint32_t interval) noexcept {
    // Original 00444990 keeps extended arithmetic and caches the first-interval float reciprocal.
    const std::uint32_t dt=std::min(std::uint32_t(render_tick-last_tick),interval);
    static const float reciprocal=static_cast<float>(1.L/static_cast<long double>(interval));
    const long double left=interval-dt,right=dt;
    return {
        static_cast<float>((previous.x*left+current.x*right)*reciprocal),
        static_cast<float>((previous.y*left+current.y*right)*reciprocal),
        static_cast<float>((previous.z*left+current.z*right)*reciprocal)};
}
long double original_dot(Vec3 a,Vec3 b) noexcept {
    // Original 00444420 evaluates z,y,x using x87.
    return (static_cast<long double>(a.z)*b.z+
            static_cast<long double>(a.y)*b.y)+static_cast<long double>(a.x)*b.x;
}
long double original_plane_distance(Vec3 point,const Plane& plane) noexcept {
    return original_dot(point,plane.normal)-plane.distance;
}


namespace {
Matrix invert_matrix(const Matrix& m,bool checked) {
    double rows[4][8]{};
    for (unsigned row=0;row!=4;++row) {
        for (unsigned col=0;col!=4;++col) rows[row][col]=m[col*4+row];
        rows[row][row+4]=1;
    }
    for (unsigned col=0;col!=4;++col) {
        unsigned pivot=col;
        for (unsigned row=col+1;row!=4;++row)
            if (std::abs(rows[row][col])>std::abs(rows[pivot][col])) pivot=row;
        if(checked && (!std::isfinite(rows[pivot][col]) || rows[pivot][col]==0))
            throw std::runtime_error("singular or non-finite scene matrix");
        if (pivot!=col) for (unsigned k=0;k!=8;++k) std::swap(rows[col][k],rows[pivot][k]);
        const double divisor=rows[col][col];
        for (double& value:rows[col]) value/=divisor;
        for (unsigned row=0;row!=4;++row) if (row!=col) {
            const double factor=rows[row][col];
            for (unsigned k=0;k!=8;++k) rows[row][k]-=factor*rows[col][k];
        }
    }
    Matrix out;
    for (unsigned col=0;col!=4;++col) for (unsigned row=0;row!=4;++row) {
        const double value=rows[row][col+4];
        if(checked && (!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max()))
            throw std::runtime_error("scene matrix inverse exceeds float range");
        out[col*4+row]=static_cast<float>(value);
    }
    return out;
}
}
Matrix inverse(const Matrix& m) { return invert_matrix(m,true); }
Matrix inverse_unchecked(const Matrix& m) noexcept { return invert_matrix(m,false); }

Vec3 transform_point(const Matrix& m, Vec3 p) noexcept {
    return {m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
            m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
            m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]};
}
Vec3 transform_vector(const Matrix& m, Vec3 p) noexcept {
    return {m[0]*p.x+m[4]*p.y+m[8]*p.z,
            m[1]*p.x+m[5]*p.y+m[9]*p.z,
            m[2]*p.x+m[6]*p.y+m[10]*p.z};
}
Quaternion normalized(Quaternion q) {
    const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if (!(n>0) || !std::isfinite(n)) throw std::runtime_error("invalid scene quaternion");
    return {q.x/n,q.y/n,q.z/n,q.w/n};
}
Quaternion interpolate(Quaternion a,Quaternion b,float t) {
    // Original 00445790: z,y,x,w dot order; no normalization or dot clamp.
    float cosine=((a.z*b.z+a.y*b.y)+a.x*b.x)+a.w*b.w;
    if(cosine<0) { b={-b.x,-b.y,-b.z,-b.w};cosine=0-cosine; }
    float left=1-t,right=t;
    if(1-cosine>0.001f) {
        const long double angle=std::acos(static_cast<double>(cosine));
        const long double inverse_sine=1/std::sin(angle);
        left=static_cast<float>(std::sin((1-static_cast<long double>(t))*angle)*inverse_sine);
        right=static_cast<float>(std::sin(static_cast<long double>(t)*angle)*inverse_sine);
    }
    return {b.x*right+a.x*left,b.y*right+a.y*left,b.z*right+a.z*left,a.w*left+b.w*right};
}
Matrix transform(Vec3 p, Quaternion rotation, Vec3 s) {
    // Original 00444e60 uses disk components as-is and its handedness is
    // opposite the common textbook quaternion-to-column-matrix convention.
    const auto q=rotation;
    const float xx=q.x*q.x,yy=q.y*q.y,zz=q.z*q.z;
    const float xy=q.x*q.y,xz=q.x*q.z,yz=q.y*q.z;
    const float wx=q.w*q.x,wy=q.w*q.y,wz=q.w*q.z;
    Matrix out=identity_matrix();
    out[0]=(1-2*(yy+zz))*s.x; out[1]=2*(xy-wz)*s.x; out[2]=2*(xz+wy)*s.x;
    out[4]=2*(xy+wz)*s.y; out[5]=(1-2*(xx+zz))*s.y; out[6]=2*(yz-wx)*s.y;
    out[8]=2*(xz-wy)*s.z; out[9]=2*(yz+wx)*s.z; out[10]=(1-2*(xx+yy))*s.z;
    out[12]=p.x;out[13]=p.y;out[14]=p.z;
    return out;
}

namespace {
Matrix axis_rotation(Vec3 axis,float degrees,bool local=false) noexcept {
    Matrix rotation=identity_matrix();
    const float norm=length(axis);
    // Original 00436930 compares against double 00477640.
    if(!(norm>0.0001))return rotation;
    axis=axis/norm;
    const long double radians=static_cast<long double>(degrees)*0.01745329238474369f;
    const float sine=static_cast<float>(std::sin(local ? static_cast<long double>(static_cast<float>(radians)) : radians));
    const float cosine=static_cast<float>(std::cos(radians)),one=1-cosine;
    rotation[0]=axis.x*axis.x*one+cosine;
    rotation[1]=axis.x*axis.y*one+sine*axis.z;
    rotation[2]=axis.x*axis.z*one-sine*axis.y;
    rotation[4]=axis.x*axis.y*one-sine*axis.z;
    rotation[5]=axis.y*axis.y*one+cosine;
    rotation[6]=axis.y*axis.z*one+sine*axis.x;
    rotation[8]=axis.x*axis.z*one+sine*axis.y;
    rotation[9]=axis.y*axis.z*one-sine*axis.x;
    rotation[10]=axis.z*axis.z*one+cosine;
    return rotation;
}
}
void rotate_local(Matrix& matrix,Vec3 angles) noexcept {
    // Scene operands are a,b,c, but original application is Rx(a),Rz(c),Ry(b).
    if(angles.x!=0)matrix=multiply(matrix,axis_rotation({1,0,0},angles.x,true));
    if(angles.z!=0)matrix=multiply(matrix,axis_rotation({0,0,1},angles.z,true));
    if(angles.y!=0)matrix=multiply(matrix,axis_rotation({0,1,0},angles.y,true));
}
void rotate_world(Matrix& matrix,Vec3 angles) noexcept {
    // Original world helpers use current orientation rows, then postmultiply.
    if(angles.x!=0)matrix=multiply(matrix,axis_rotation({matrix[1],matrix[5],matrix[9]},angles.x));
    if(angles.z!=0)matrix=multiply(matrix,axis_rotation({matrix[0],matrix[4],matrix[8]},angles.z));
    if(angles.y!=0)matrix=multiply(matrix,axis_rotation({matrix[2],matrix[6],matrix[10]},angles.y));
}

std::uint32_t animation_duration_ms(const Animation& animation) noexcept {
    return animation.frame_count ? animation.header_word*(animation.frame_count-1) : 0;
}
namespace {
std::uint32_t controller_milliseconds(std::uint32_t ticks) noexcept {
    // 0044f200/430 spills unsigned clock differences to float before FRNDINT.
    const float seconds=static_cast<float>(ticks)/1000.f;
    const float milliseconds=seconds*1000.f;
    const double rounded=std::nearbyint(static_cast<double>(milliseconds));
    // FISTP DWORD's defined x87 integer-indefinite result on signed overflow.
    return rounded>std::numeric_limits<std::int32_t>::max()?0x80000000u:
        static_cast<std::uint32_t>(rounded);
}
std::uint32_t controller_anchor(std::uint32_t now,std::uint32_t phase) noexcept {
    // 0044f6cc/734 uses FTOL after float(now) - phase*.001f*timerFrequency.
    const long double value=static_cast<float>(now)-
        static_cast<long double>(phase)*.001f*1000;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(value));
}
void controller_sample(AnimationControllerState& state) noexcept {
    state.elapsed=state.sample_tick*.001f;
    state.loop=state.animation_mode==2;
    state.active=(state.flags&3)!=3;
}
}
void reset_animation_controller(AnimationControllerState& state) noexcept {
    // 0044f190 retains the bound clip, mode, sample and dependency metadata.
    state.flags=3;state.start_tick=state.pause_tick=0;state.blend=1;
    controller_sample(state);
}
void play_animation_controller(AnimationControllerState& state,std::uint32_t mode,
                               std::uint32_t now) noexcept {
    const auto status=state.flags&3;
    if(status==3) state.start_tick=state.pause_tick=now;
    else if(status==2) state.start_tick+=now-state.pause_tick;
    state.flags=(state.flags&~2u)|1u;
    state.animation_mode=mode;
    controller_sample(state);
}
void reverse_animation_controller(AnimationControllerState& state,std::uint32_t mode,
                                  std::uint32_t duration,std::uint32_t now) noexcept {
    state.animation_mode=mode;
    auto phase=controller_milliseconds(now-state.start_tick);
    if(duration && phase>duration)phase=duration-5;
    phase=duration?duration-phase:0;
    state.start_tick=controller_anchor(now,phase);
    fade_animation_controller(state,8,duration,now);
}
void pause_animation_controller(AnimationControllerState& state,std::uint32_t duration,
                                std::uint32_t now) noexcept {
    state.sample_tick=controller_milliseconds(now-state.start_tick);
    if(duration==0) {
        // Approved sole-frame policy: keep authored duration zero and sample0.
        state.sample_tick=0;
        if(state.animation_mode==0 && now!=state.start_tick) reset_animation_controller(state);
    } else {
        if(state.sample_tick>=duration) {
            if(state.animation_mode==0) reset_animation_controller(state);
            else if(state.animation_mode==2) state.sample_tick%=duration;
            else state.sample_tick=duration-1;
        }
        if(state.animation_mode==3) state.sample_tick=duration-state.sample_tick;
    }
    state.pause_tick=now;state.flags=(state.flags&~1u)|2u;
    controller_sample(state);
}
void fade_animation_controller(AnimationControllerState& state,std::uint32_t flag,
                               std::uint32_t duration,std::uint32_t now) noexcept {
    state.flags=(state.flags&~(4u|8u|16u))|flag;
    state.fade_tick=now;
    pause_animation_controller(state,duration,now);
}
void advance_animation_controller(AnimationControllerState& state,std::uint32_t duration,
                                  std::uint32_t now,const AnimationControllerState* master,
                                  bool master_matches) noexcept {
    if(!state.active || (state.flags&3)==3)return;
    if(duration!=0 && (state.flags&3)==2 && state.animation_mode==3)
        state.sample_tick=duration-state.sample_tick;
    if(state.flags&16) {
        if(master) state.blend=master->blend*state.coefficient;
        if(!master || !master->active || (master->flags&3)==3 || !master_matches) {
            reset_animation_controller(state);return;
        }
    } else if(state.flags&8) {
        const long double ticks=static_cast<float>(std::uint32_t(now-state.fade_tick));
        state.blend=static_cast<float>((ticks/1000)*6+state.blend);
        state.fade_tick=now;
        if(state.blend>=1) {
            state.blend=1;state.flags&=~8u;
            play_animation_controller(state,state.animation_mode,now);
        }
    } else if(state.flags&4) {
        const long double ticks=static_cast<float>(std::uint32_t(now-state.fade_tick));
        state.blend=static_cast<float>(state.blend-(ticks/1000)*6);
        state.fade_tick=now;
        if(state.blend<=0) {
            state.blend=0;state.flags&=~4u;
            reset_animation_controller(state);return;
        }
    }
    if(state.animation_mode==4) {controller_sample(state);return;}
    if(duration==0) {
        state.sample_tick=0;
        if((state.flags&3)==1 && state.animation_mode==0 && now!=state.start_tick)
            reset_animation_controller(state);
        controller_sample(state);return;
    }
    if((state.flags&3)==1) {
        state.sample_tick=controller_milliseconds(now-state.start_tick);
        if(state.sample_tick>=duration) {
            if(state.animation_mode==0) {reset_animation_controller(state);return;}
            if(state.animation_mode==2) {
                state.sample_tick%=duration;
                state.start_tick=controller_anchor(now,state.sample_tick);
            } else if(state.animation_mode==1 || state.animation_mode==3) {
                state.sample_tick=duration-5;
                state.start_tick=controller_anchor(now,duration-1);
            }
        }
    }
    if(state.animation_mode==3) {
        if(state.sample_tick==0)state.sample_tick=5;
        state.sample_tick=duration-state.sample_tick;
    }
    controller_sample(state);
}

namespace {
bool same_name(std::string_view a,std::string_view b) noexcept {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i) {
        const auto fold=[](unsigned char c){return c>='A'&&c<='Z'?c+('a'-'A'):c;};
        if(fold(a[i])!=fold(b[i]))return false;
    }
    return true;
}

[[noreturn]] void scene_error(std::string_view reason) {
    throw std::runtime_error("read_scene: "+std::string(reason));
}

bool scene_token_byte(unsigned char c) noexcept {
    if(c>=' '&&c<='~')return true;
    constexpr std::array<unsigned char,12> extra{
        0xfd,0xdd,0xdc,0xfc,0xde,0xfe,0xd0,0xf0,0xc7,0xe7,0xd6,0xf6};
    return std::find(extra.begin(),extra.end(),c)!=extra.end();
}

bool scene_line(std::string_view text,std::size_t& position,
                std::vector<std::string_view>& tokens) {
    tokens.clear();
    if(position==text.size())return false;
    const auto end=text.find('\n',position);
    const auto line=text.substr(position,(end==text.npos?text.size():end)-position);
    position=end==text.npos?text.size():end+1;
    if(line.size()>65536)scene_error("line exceeds limit");
    if(line.find('\0')!=line.npos)scene_error("embedded NUL in scene");
    for(std::size_t begin=0;begin<line.size();) {
        while(begin<line.size()&&
              (!scene_token_byte(static_cast<unsigned char>(line[begin]))||line[begin]==' '))++begin;
        if(begin==line.size())break;
        bool quoted=false,had_quote=false;
        std::size_t finish=begin;
        while(finish<line.size()) {
            const auto c=static_cast<unsigned char>(line[finish]);
            if(!scene_token_byte(c)||(c==' '&&!quoted))break;
            if(c=='"'){quoted=!quoted;had_quote=true;}
            ++finish;
        }
        if(quoted)scene_error("unterminated quoted scene token");
        auto token=line.substr(begin,finish-begin);
        if(had_quote&&token.size()>=2)token=token.substr(1,token.size()-2);
        if(token.starts_with("//"))break;
        if(token.size()>8192||tokens.size()==4096)scene_error("token limit exceeded");
        tokens.push_back(token);
        begin=finish;
    }
    return true;
}

std::string_view scene_numeric_token(std::string_view token) noexcept {
    while(!token.empty()&&(token.front()==' '||token.front()=='\t'||token.front()=='\r'||
          token.front()=='\n'||token.front()=='\v'||token.front()=='\f'))token.remove_prefix(1);
    if(token.starts_with('+')) {
        token.remove_prefix(1);
        if(token.starts_with('-')||token.starts_with('+'))token.remove_prefix(token.size());
    }
    return token;
}

double scene_number(std::string_view token) {
    token=scene_numeric_token(token);
    double value{};
    const auto parsed=std::from_chars(token.data(),token.data()+token.size(),value);
    if(parsed.ec==std::errc::result_out_of_range||!std::isfinite(value))
        scene_error("overflowing or non-finite numeric value");
    // The original atof accepts a numeric prefix, and returns zero when none exists.
    if(parsed.ec!=std::errc{})return 0;
    return value;
}

float scene_float(double value) {
    if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())
        scene_error("numeric value exceeds float range");
    return static_cast<float>(value);
}

std::int32_t scene_integer(std::string_view token) {
    token=scene_numeric_token(token);
    std::int32_t value{};
    const auto parsed=std::from_chars(token.data(),token.data()+token.size(),value);
    if(parsed.ec==std::errc::result_out_of_range)scene_error("overflowing integer value");
    // Original atoi ignores trailing text and returns zero for a nondigit prefix.
    if(parsed.ec!=std::errc{})return 0;
    return value;
}

Vec3 scene_position(const std::vector<std::string_view>& tokens,Vec3 offset={}) {
    return {scene_float(scene_number(tokens[1])+offset.x),
            scene_float(scene_number(tokens[2])+offset.y),
            scene_float(scene_number(tokens[3])+offset.z)};
}

class SceneReader {
public:
    explicit SceneReader(const AssetStore& assets):assets_(assets){}
    SceneDefinition read(std::string_view name,Vec3 offset) {
        scene_float(offset.x);scene_float(offset.y);scene_float(offset.z);
        expand(name,offset);
        return std::move(result_);
    }
private:
    const AssetStore& assets_;
    SceneDefinition result_;
    std::vector<std::string> active_;
    std::size_t work_{};

    void resource_name(std::string_view name) const {
        // AssetStore performs the shared strict Windows-relative path checks.
        (void)assets_.contains(name);
    }

    void property(SceneEntry& entry,const std::vector<std::string_view>& tokens,Vec3 offset) {
        const auto command=tokens[0];
        if(same_name(command,"pos")&&tokens.size()==4)entry.position=scene_position(tokens,offset);
        else if(same_name(command,"orientation")&&tokens.size()==13) {
            constexpr std::array<unsigned,12> indices{0,1,2,4,5,6,8,9,10,12,13,14};
            for(std::size_t i=0;i<indices.size();++i)
                entry.orientation[indices[i]]=scene_float(scene_number(tokens[i+1]));
            // Unlike pos, authored orientation translation deliberately drops the include offset.
            entry.position={entry.orientation[12],entry.orientation[13],entry.orientation[14]};
        } else if((same_name(command,"rotlocal")||same_name(command,"rotworld"))&&tokens.size()==4) {
            const auto angles=scene_position(tokens);
            auto& rotation=same_name(command,"rotlocal")?entry.local_rotation:entry.world_rotation;
            // Original zero operands leave previously authored values untouched.
            if(angles.x!=0)rotation.x=angles.x;
            if(angles.y!=0)rotation.y=angles.y;
            if(angles.z!=0)rotation.z=angles.z;
        } else if(same_name(command,"mesh")&&tokens.size()==2) {
            (void)assets_.contains_optional_game_file(tokens[1]);entry.mesh=tokens[1];
        } else if(same_name(command,"weapon")&&tokens.size()==9) {
            resource_name(tokens[1]);
            entry.weapon=SceneWeapon{std::string(tokens[1]),scene_integer(tokens[2])};
        } else if(same_name(command,"take_weapon")&&tokens.size()==3) {
            resource_name(tokens[1]);resource_name(tokens[2]);
            entry.take_weapons={std::string(tokens[1]),std::string(tokens[2])};
        } else if(same_name(command,"ammo")&&tokens.size()==3) {
            resource_name(tokens[1]);
            entry.ammo=SceneWeapon{std::string(tokens[1]),scene_integer(tokens[2])};
        } else if(same_name(command,"drop_ammo")&&tokens.size()==3) {
            const auto count=(std::int64_t(scene_integer(tokens[1]))+scene_integer(tokens[2]))*10;
            if(count<std::numeric_limits<std::int32_t>::min()||
               count>std::numeric_limits<std::int32_t>::max())scene_error("drop_ammo exceeds integer range");
            entry.drop_ammo=static_cast<std::int32_t>(count);
        } else if(same_name(command,"drop_health")&&tokens.size()==2) {
            if(scene_integer(tokens[1])!=0)entry.drop_health=true;
        } else {
            constexpr std::array<std::string_view,3> positive{"align+x","align+y","align+z"};
            constexpr std::array<std::string_view,3> negative{"align-x","align-y","align-z"};
            for(std::size_t axis=0;axis!=3;++axis) {
                if(same_name(command,positive[axis]))entry.align_positive[axis]=true;
                if(same_name(command,negative[axis]))entry.align_negative[axis]=true;
            }
        }
    }

    void append(SceneEntry entry) {
        if(result_.entries.size()==65536)scene_error("expanded entity count exceeds limit");
        result_.entries.push_back(std::move(entry));
    }

    void expand(std::string_view name,Vec3 offset) {
        resource_name(name);
        std::string key(name);
        for(char& c:key) {
            if(c=='\\')c='/';
            else if(c>='A'&&c<='Z')c=static_cast<char>(c+('a'-'A'));
        }
        if(std::find(active_.begin(),active_.end(),key)!=active_.end())
            scene_error("cyclic scene include: "+key);
        if(active_.size()==128)scene_error("scene include depth exceeds limit");
        const auto text=assets_.text(name);
        if(text.size()>16U*1024U*1024U||text.size()>64U*1024U*1024U-work_)
            scene_error("scene parsing work exceeds limit");
        work_+=text.size();
        active_.push_back(std::move(key));
        std::vector<std::string_view> tokens;
        std::optional<SceneEntry> entry;
        std::string include;
        bool pending=false,including=false;
        std::size_t depth=0,position=0;
        while(scene_line(text,position,tokens)) {
            if(tokens.empty())continue;
            const auto command=tokens[0];
            if(command.starts_with("{")) {
                if(!pending||depth==128)scene_error("unexpected or excessively nested opening brace");
                ++depth;
            } else if(command.starts_with("}")) {
                if(depth==0)scene_error("unbalanced closing brace");
                if(--depth==0) {
                    if(entry)append(std::move(*entry));
                    entry.reset();include.clear();pending=false;including=false;
                }
            } else if(depth==0) {
                // Unbraced top-level junk (e.g. surlar_2_martilar's numeric line)
                // is superseded by the next name, just as in the original dispatcher.
                entry.reset();
                if(command.empty())scene_error("empty scene entity name");
                include="level/scene/"+std::string(command)+".txt";
                including=assets_.contains_optional_game_file(include);
                pending=true;
                if(!including) {
                    entry.emplace();
                    entry->name=command;entry->source=name;
                    include.clear();
                }
            } else if(including) {
                if(same_name(command,"pos")&&tokens.size()==4) {
                    const auto nested_offset=scene_position(tokens,offset);
                    // 004282e0 clears the include name after the first pos.
                    // Later pos lines therefore try level/scene/.txt, not the previous scene.
                    const auto nested=include.empty()?std::string("level/scene/.txt"):include;
                    if(assets_.contains_optional_game_file(nested))expand(nested,nested_offset);
                    include.clear();
                }
            } else if(entry)property(*entry,tokens,offset);
        }
        if(depth!=0)scene_error("unclosed scene block");
        active_.pop_back();
    }
};
}

SceneDefinition read_scene(const AssetStore& assets,std::string_view logical_name,Vec3 offset) {
    return SceneReader(assets).read(logical_name,offset);
}

namespace {
struct FramePair {
    std::uint32_t first{}, second{};
    float fraction{};
    bool active{true};
};

FramePair pose_frames(const Animation& animation,std::uint32_t duration_ms,
                      float seconds,std::uint32_t mode) {
    if(!std::isfinite(seconds)||seconds<0)
        throw std::invalid_argument("invalid pose sample time");
    if(mode>4)throw std::invalid_argument("invalid PA playback mode");
    if(mode==4)return {0,0,0,false}; // 0044f55b -> 0044f4d8 returns "skip sampling".
    float elapsed=std::nearbyint(seconds*1000.f);
    if(!std::isfinite(elapsed))throw std::invalid_argument("pose sample time exceeds timer range");
    // 0044f8f0 allocates exactly frames*tracks*32; 00448420 reads j=1 beyond
    // a one-frame allocation. Approved memory-safety boundary: use the sole
    // authored key, not undefined old heap data, a fabricated key, or asset repair.
    if(animation.frame_count==1)return {};
    if(mode==0&&elapsed>=duration_ms)return {0,0,0,false};
    if(mode==2)elapsed=std::fmod(elapsed,static_cast<float>(duration_ms));
    std::uint32_t milliseconds;
    if(mode!=2&&mode!=4&&elapsed>=duration_ms)milliseconds=duration_ms-5u;
    else {
        if(static_cast<long double>(elapsed)>std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("PA sample cursor exceeds original timer range");
        milliseconds=static_cast<std::uint32_t>(elapsed);
    }
    if(mode==3) {
        if(milliseconds==0)milliseconds=5;
        milliseconds=duration_ms-milliseconds;
    }
    const auto first=milliseconds/animation.header_word;
    if(first>=animation.frame_count-1)
        throw std::invalid_argument("PA sample cursor exceeds authored frames");
    return {first,first+1,
            static_cast<float>(milliseconds%animation.header_word)/
            static_cast<float>(animation.header_word)};
}

BoneFrame sample_bone(const BoneTrack& track,FramePair pair) {
    const auto& a=track.frames[pair.first];
    if(pair.first==pair.second)return a;
    const auto& b=track.frames[pair.second];
    return {b.position*pair.fraction+a.position*(1-pair.fraction),
            interpolate(a.rotation,b.rotation,pair.fraction)};
}

// 00443360's stored-float product/addition order, including the fourth row.
Matrix pose_product(const Matrix& a,const Matrix& b) noexcept {
    Matrix result{};
    for(unsigned column=0;column<4;++column) {
        const auto offset=column*4;
        for(unsigned row=0;row<4;++row) {
            const float p0=a[row]*b[offset],p1=a[row+4]*b[offset+1];
            const float p2=a[row+8]*b[offset+2],p3=a[row+12]*b[offset+3];
            if(column==0)result[offset+row]=((p1+p0)+p3)+p2;
            else if(column==3)result[offset+row]=((p1+p3)+p2)+p0;
            else result[offset+row]=((p0+p1)+p3)+p2;
        }
    }
    return result;
}

FramePair keyframe_frames(std::uint32_t count,std::uint32_t elapsed_ms,
                         std::uint32_t duration_ms,bool loop) {
    if(count<2||duration_ms==0)
        throw std::invalid_argument("invalid keyframe sample timing");
    if(loop&&elapsed_ms>duration_ms)elapsed_ms%=duration_ms;
    const float elapsed=static_cast<float>(elapsed_ms);
    const float step=static_cast<float>(
        static_cast<long double>(duration_ms)/static_cast<long double>(count-1));
    const long double reciprocal=1/static_cast<long double>(step);
    const long double quotient=static_cast<long double>(elapsed)*reciprocal;
    if(quotient>=count-1)return {count-2,count-1,1};
    const auto first=static_cast<std::uint32_t>(quotient);
    const float fraction=static_cast<float>(
        std::fmod(static_cast<long double>(elapsed),static_cast<long double>(step))*
        static_cast<float>(reciprocal));
    return {first,first+1,fraction};
}

Matrix sample_transform(const TransformTrack& track,FramePair pair,bool camera) {
    const auto& b=track.frames[pair.second];
    if(camera)return transform(b.position,b.rotation,b.scale);
    const auto& a=track.frames[pair.first];
    const float left=1-pair.fraction;
    return transform(a.position*left+b.position*pair.fraction,
                     interpolate(a.rotation,b.rotation,pair.fraction),
                     a.scale*left+b.scale*pair.fraction);
}
}

PoseEvaluator::PoseEvaluator(const Mesh& mesh,const Animation* animation)
    :mesh_(mesh),animation_(animation),tracks_(mesh.bones.size()) {
    order_.reserve(mesh.bones.size());
    std::vector<std::uint8_t> state(mesh.bones.size());
    const auto visit=[&](auto&& self,std::uint32_t bone)->void {
        if(state[bone]==2)return;
        if(state[bone]==1)throw std::invalid_argument("cyclic mesh bone hierarchy");
        state[bone]=1;
        const auto parent=mesh.bones[bone].parent;
        if(parent<-1||parent>=static_cast<std::int64_t>(mesh.bones.size()))
            throw std::invalid_argument("mesh bone parent is out of range");
        if(parent>=0)self(self,static_cast<std::uint32_t>(parent));
        state[bone]=2;
        order_.push_back(bone);
    };
    for(std::uint32_t bone=0;bone<mesh.bones.size();++bone)visit(visit,bone);
    if(!animation)return;
    if(!animation->frame_count||!animation->header_word)
        throw std::invalid_argument("invalid PA frame count or sample spacing");
    const auto duration=std::uint64_t(animation->frame_count-1)*animation->header_word;
    if(duration>std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("PA duration exceeds original timer range");
    duration_ms_=static_cast<std::uint32_t>(duration);
    track_bones_.reserve(animation->tracks.size());
    for(const auto& track:animation->tracks) {
        if(track.frames.size()!=animation->frame_count)
            throw std::invalid_argument("PA track frame count mismatch");
        std::uint32_t bone=0;
        for(std::uint32_t candidate=0;candidate<mesh.bones.size();++candidate)
            if(same_name(track.name,mesh.bones[candidate].name)) {
                bone=candidate;
                break;
            }
        // 0044f8f0 retains every authored track; an unknown name maps to bone zero.
        track_bones_.push_back(bone);
        if(!tracks_.empty()&&!tracks_[bone])tracks_[bone]=&track;
    }
}

float PoseEvaluator::duration_seconds() const noexcept {
    return duration_ms_*0.001f;
}

void PoseEvaluator::evaluate(float seconds,bool loop,std::span<Matrix> global_pose) const {
    if(global_pose.size()!=mesh_.bones.size())
        throw std::invalid_argument("pose output size mismatch");
    if(!animation_||mesh_.bones.empty()) {
        for(std::size_t bone=0;bone<mesh_.bones.size();++bone)
            global_pose[bone]=mesh_.bones[bone].global_bind;
        return;
    }
    const auto pair=pose_frames(*animation_,duration_ms_,seconds,loop ? 2u : 1u);
    for(std::size_t index=0;index<animation_->tracks.size();++index) {
        const auto& track=animation_->tracks[index];
        const auto bone=track_bones_[index];
        const auto frame=sample_bone(track,pair);
        const auto matrix=transform(frame.position,frame.rotation);
        if(tracks_[bone]==&track)global_pose[bone]=matrix;
        else for(unsigned component=0;component<16;++component)
            global_pose[bone][component]=matrix[component]+global_pose[bone][component];
    }
    for(const auto bone:order_) {
        const auto& bind=mesh_.bones[bone];
        Matrix base=bind.local_bind;
        if(bind.parent>=0)base=pose_product(global_pose[bind.parent],base);
        global_pose[bone]=tracks_[bone] ? pose_product(base,global_pose[bone]) : base;
    }
}

void evaluate_layered_pose(const Mesh& mesh,std::span<const AnimationLayer> layers,
                           std::span<Matrix> global_pose,std::span<BoneFrame> local_scratch,
                           std::span<std::uint8_t> touched_scratch) {
    if(global_pose.size()!=mesh.bones.size()||local_scratch.size()!=mesh.bones.size()||
       touched_scratch.size()!=mesh.bones.size())
        throw std::invalid_argument("layered pose output size mismatch");
    const PoseEvaluator* topology=nullptr;
    for(const auto& layer:layers)if(layer.binding) {
        if(&layer.binding->mesh_!=&mesh)
            throw std::invalid_argument("animation layer is bound to another mesh");
        topology=layer.binding;
    }
    // 00447cd0 resets flags, not bone transforms or key data.
    std::fill(touched_scratch.begin(),touched_scratch.end(),0);
    if(!topology||mesh.bones.empty()) {
        for(std::size_t bone=0;bone<mesh.bones.size();++bone)
            global_pose[bone]=mesh.bones[bone].global_bind;
        return;
    }
    for(const auto& layer:layers) {
        const auto* binding=layer.binding;
        if(!binding||!binding->animation_)continue;
        const auto& animation=*binding->animation_;
        const auto pair=pose_frames(animation,binding->duration_ms_,layer.seconds,
                                    layer.loop ? 2u : layer.mode);
        if(!pair.active)continue;
        for(std::size_t index=0;index<animation.tracks.size();++index) {
            const auto bone=binding->track_bones_[index];
            local_scratch[bone]=sample_bone(animation.tracks[index],pair);
            const auto& frame=local_scratch[bone];
            const auto matrix=transform(frame.position,frame.rotation);
            for(unsigned component=0;component<16;++component) {
                const float weighted=matrix[component]*layer.weight;
                global_pose[bone][component]=touched_scratch[bone] ?
                    weighted+global_pose[bone][component] : weighted;
            }
            touched_scratch[bone]=1;
        }
    }
    for(const auto bone:topology->order_) {
        const auto& bind=mesh.bones[bone];
        Matrix base=bind.local_bind;
        if(bind.parent>=0)base=pose_product(global_pose[bind.parent],base);
        global_pose[bone]=touched_scratch[bone] ? pose_product(base,global_pose[bone]) : base;
    }
}

std::uint32_t keyframe_duration_ticks(const Keyframes& animation) noexcept {
    // Original 0x0041d8b0: uncleaned 32-byte cdecl arguments make FILD [ESP+44]
    // read the duration header, then multiply by float 0x3a83126f and 1000.
    // FTOL truncates positive ticks; narrowing its integer result preserves the low DWORD.
    const long double ticks=(static_cast<long double>(animation.header_word)*
        std::bit_cast<float>(0x3a83126fu))*1000.L;
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(ticks));
}

std::uint32_t keyframe_playback_duration_ticks(const Keyframes& animation) {
    const auto ticks=keyframe_duration_ticks(animation);
    if(!ticks)throw std::runtime_error("Zero-duration PKA cannot be played");
    return ticks;
}

void evaluate_keyframes(const Keyframes& animation,std::uint32_t elapsed_ms,std::uint32_t duration_ms,
                        bool loop,std::span<Matrix> transforms) {
    if(transforms.size()!=animation.tracks.size())
        throw std::invalid_argument("keyframe output size mismatch");
    const auto pair=keyframe_frames(animation.frame_count,elapsed_ms,duration_ms,loop);
    for(std::size_t track=0;track<animation.tracks.size();++track) {
        if(animation.tracks[track].frames.size()!=animation.frame_count)
            throw std::invalid_argument("keyframe track frame count mismatch");
        transforms[track]=sample_transform(animation.tracks[track],pair,
            animation.tracks[track].name.find("camera_")!=std::string::npos);
    }
}

Matrix evaluate_keyframe_track(const TransformTrack& track,std::uint32_t elapsed_ms,
                               std::uint32_t duration_ms,bool camera) {
    if(track.frames.size()>std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("keyframe track exceeds original frame count range");
    return sample_transform(track,keyframe_frames(static_cast<std::uint32_t>(track.frames.size()),
                            elapsed_ms,duration_ms,false),camera);
}

void skin_mesh(const Mesh& mesh, std::span<const Matrix> pose,
               std::span<Vec3> positions,std::span<Vec3> normals) {
    if (pose.size()!=mesh.bones.size() || positions.size()!=mesh.positions.size() ||
        normals.size()!=mesh.normals.size() || mesh.vertex_bones.size()!=mesh.positions.size())
        throw std::invalid_argument("mesh skin output size mismatch");
    std::copy(mesh.positions.begin(),mesh.positions.end(),positions.begin());
    std::copy(mesh.normals.begin(),mesh.normals.end(),normals.begin());
    for(std::size_t bone=0;bone<mesh.bones.size();++bone) {
        const Matrix skin=multiply(pose[bone],mesh.bones[bone].inverse_bind);
        for(const auto vertex:mesh.bones[bone].vertices) {
            if(vertex>=positions.size() || vertex>=normals.size())
                throw std::runtime_error("mesh bone vertex is out of range");
            positions[vertex]=transform_point(skin,mesh.positions[vertex]);
            normals[vertex]=transform_vector(skin,mesh.normals[vertex]);
        }
    }
}

Matrix attachment_transform(const Matrix& world,const Mesh& mesh,
                            std::span<const Matrix> pose,const Attachment& attachment) {
    if(pose.size()!=mesh.bones.size())throw std::invalid_argument("attachment pose size mismatch");
    Matrix result=world;
    if(attachment.bone) {
        const auto bone=std::find_if(mesh.bones.begin(),mesh.bones.end(),[&](const Bone& b){return same_name(b.name,*attachment.bone);});
        if(bone==mesh.bones.end())throw std::runtime_error("attachment bone not found: "+*attachment.bone);
        // Captured child offsets are MODEL-space. Original 00411ef0 rebases
        // against inverse bind (+0x40); 00414480 applies animated bone (+0x80).
        result=multiply(multiply(result,pose[static_cast<std::size_t>(bone-mesh.bones.begin())]),bone->inverse_bind);
    }
    if(attachment.transform)result=multiply(result,*attachment.transform);
    return result;
}

namespace {
float hull_support(Vec3 normal,Bounds hull) noexcept {
    const Vec3 half=(hull.maximum-hull.minimum)*0.5f;
    return static_cast<float>(
        (static_cast<long double>(std::abs(normal.z))*half.z+
         static_cast<long double>(std::abs(normal.y))*half.y)+
         static_cast<long double>(std::abs(normal.x))*half.x);
}
}

CollisionWorld::CollisionWorld(const Level& level)
    :level_(level),shader_flags_(level.shaders.size()),shader_cull_(level.shaders.size(),0x405),
     materials_(level.shaders.size()) {
    for(std::size_t i=0;i<level.shaders.size();++i) {
        const auto& shader=level.shaders[i];
        // Original 0040cee0 derives fresh runtime flags, not raw PL flags.
        shader_flags_[i]=0x108000u|((shader.contents&0x10001u)?0x600u:0x400u);
        if(shader.surface_flags&0x10u)shader_flags_[i]&=~0x400u;
    }
}
CollisionWorld::CollisionWorld(const Level& level,const MaterialLibrary& materials)
    :level_(level),shader_flags_(level.shaders.size()),shader_cull_(level.shaders.size(),0x405),
     materials_(level.shaders.size()) {
    for(std::size_t i=0;i<level.shaders.size();++i) {
        const auto& shader=level.shaders[i];
        const auto& material=materials.construct(shader.name,shader.contents,shader.surface_flags,false);
        shader_flags_[i]=materials.runtime_trace_mask(shader.name);
        materials_[i]=&material;
        shader_cull_[i]=material.cull==MaterialCull::none?0:
                       material.cull==MaterialCull::front?0x404:0x405;
    }
}

bool CollisionWorld::intersects_brush(std::uint32_t index,Vec3 origin,Bounds hull) const {
    const auto& brush=level_.brushes.at(index);
    const Vec3 center=origin+(hull.minimum+hull.maximum)*0.5f;
    for(std::uint32_t i=0;i<brush.side_count;++i) {
        const auto& plane=level_.planes.at(level_.brush_sides.at(brush.first_side+i).plane);
        // xPointInBrush 1000d750 includes distances <= +0.001.
        if(static_cast<float>(original_plane_distance(center,plane)-
                              hull_support(plane.normal,hull))>0.001f)return false;
    }
    return true;
}

TriggerTrace CollisionWorld::trace_trigger(std::uint32_t index,Vec3 start,Vec3 end,Bounds hull) const {
    const auto& brush=level_.brushes.at(index);
    const Vec3 center=(hull.minimum+hull.maximum)*0.5f;
    start=start+center;end=end+center;
    TriggerTrace result=TriggerTrace::inside;
    // Original EXE 00422a70: stored-side clipping; LAST transition wins.
    for(std::uint32_t i=0;i<brush.side_count;++i) {
        const auto& plane=level_.planes.at(level_.brush_sides.at(brush.first_side+i).plane);
        const float support=hull_support(plane.normal,hull);
        const float a=static_cast<float>(original_plane_distance(start,plane)-support);
        const float b=static_cast<float>(original_plane_distance(end,plane)-support);
        if(a> -0.001f) {
            if(b> -0.001f)return TriggerTrace::miss;
            start=start+(end-start)*(a/(a-b));
            result=TriggerTrace::enter;
        } else if(a<0.001f && b>=0.001f) {
            end=start+(end-start)*(a/(a-b));
            result=TriggerTrace::exit;
        }
    }
    return result;
}

std::uint32_t CollisionWorld::point_contents(Vec3 point) const {
    const auto& leaf=level_.leaves.at(static_cast<std::size_t>(leaf_at(point)));
    std::uint32_t contents=0;
    for(std::uint32_t i=0;i<leaf.brush_count;++i) {
        const auto brush=level_.leaf_brushes.at(leaf.first_brush+i);
        if(intersects_brush(brush,point,{}))
            contents|=level_.shaders.at(level_.brushes.at(brush).shader).contents;
    }
    return contents;
}

std::int32_t CollisionWorld::leaf_at(Vec3 point) const {
    if(level_.leaves.empty())throw std::runtime_error("collision world has no BSP leaves");
    if(level_.nodes.empty())return 0;
    std::int32_t child=0;
    std::size_t remaining=level_.nodes.size()+1;
    while(child>=0) {
        if(!remaining-- || static_cast<std::size_t>(child)>=level_.nodes.size())
            throw std::runtime_error("invalid or cyclic BSP node tree");
        const auto& node=level_.nodes[static_cast<std::size_t>(child)];
        const auto& plane=level_.planes.at(node.plane);
        // EXE 00423ee0 / DLL xGetPointLeaf 1000c780: front is > -0.001.
        child=node.children[static_cast<float>(original_plane_distance(point,plane))> -0.001f ? 0 : 1];
    }
    const auto leaf=~child;
    if(static_cast<std::size_t>(leaf)>=level_.leaves.size())
        throw std::runtime_error("invalid BSP leaf index");
    return leaf;
}

bool CollisionWorld::trace_brush(std::uint32_t index,Vec3 start,Vec3 end,Vec3 direction,
                                 Bounds hull,Trace& candidate) const {
    const auto& brush=level_.brushes.at(index);
    Vec3 a=start,b=end;
    bool hit=false;
    Plane hit_plane{};
    std::uint32_t hit_shader{};
    // Original EXE 00421ec0 clips mutable endpoints in stored side order.
    for(std::uint32_t i=0;i<brush.side_count;++i) {
        const auto& side=level_.brush_sides.at(brush.first_side+i);
        const auto& plane=level_.planes.at(side.plane);
        const float radius=hull_support(plane.normal,hull);
        const auto cull=shader_cull_.at(side.shader);
        const auto qa=original_plane_distance(a,plane);
        const auto qb=original_plane_distance(b,plane);
        const float shift=cull==0x405 ? -radius :
                          cull==0x404 ? radius : qa>0 ? -radius : radius;
        const float da=static_cast<float>(qa+shift),db=static_cast<float>(qb+shift);
        if(da> -0.001f) {
            if(db> -0.001f)return false;
            hit_plane=plane;hit_shader=side.shader;hit=true;
            a=a+(b-a)*(da/(da-db));
        } else if(da<0.001f && db>=0.001f) {
            if(cull==0 || cull==0x404) {
                hit_plane=plane;hit_shader=side.shader;hit=true;
            }
            b=b+(a-b)*(db/(db-da));
        }
    }
    if(!hit)return false;
    candidate.hit=true;candidate.end=a;candidate.normal=hit_plane.normal;
    candidate.plane_distance=hit_plane.distance;
    candidate.distance=static_cast<float>(original_dot(a-start,direction));
    candidate.leaf_distance=candidate.distance;
    candidate.brush=static_cast<std::int32_t>(index);
    candidate.shader=static_cast<std::int32_t>(hit_shader);
    candidate.contents=level_.shaders.at(brush.shader).contents;
    candidate.surface_flags=level_.shaders.at(hit_shader).surface_flags;
    candidate.shader_name=level_.shaders.at(hit_shader).name;
    if(const auto* material=materials_.at(hit_shader)) {
        candidate.reflection=material->reflection;
        candidate.material_name=material->physical_material_name;
    }
    return true;
}

bool CollisionWorld::visible(Vec3 from,Vec3 to) const {
    const auto a=level_.leaves.at(static_cast<std::size_t>(leaf_at(from))).cluster;
    const auto b=level_.leaves.at(static_cast<std::size_t>(leaf_at(to))).cluster;
    return a<0 || b<0 || level_.visibility.visible(static_cast<std::uint32_t>(a),static_cast<std::uint32_t>(b));
}

bool CollisionWorld::trace_leaf(std::uint32_t index,Vec3 start,Vec3 end,Vec3 direction,
                               Bounds hull,std::uint32_t mask,Trace& result) const {
    const auto& leaf=level_.leaves.at(index);
    float nearest=std::numeric_limits<float>::max();
    bool hit=false;
    // Original 004237d0 resets nearest in EACH leaf, compares strictly smaller,
    // and does not deduplicate brushes occurring in different leaf intervals.
    for(std::uint32_t i=0;i<leaf.brush_count;++i) {
        const auto index_brush=level_.leaf_brushes.at(leaf.first_brush+i);
        const auto& brush=level_.brushes.at(index_brush);
        if(!(shader_flags_.at(brush.shader)&mask))continue;
        Trace candidate;
        if(trace_brush(index_brush,start,end,direction,hull,candidate) && candidate.distance<nearest) {
            nearest=candidate.distance;
            result=candidate;
            hit=true;
        }
    }
    return hit;
}

bool CollisionWorld::trace_node(std::int32_t index,Vec3 start,Vec3 finish,
                               Vec3 direction,Bounds hull,std::uint32_t mask,
                               Trace& result,std::size_t depth) {
    if(depth>level_.nodes.size())throw std::runtime_error("cyclic collision BSP");
    if(index<0)return trace_leaf(static_cast<std::uint32_t>(~index),start,finish,direction,hull,mask,result);
    const auto& node=level_.nodes.at(static_cast<std::size_t>(index));
    const auto& plane=level_.planes.at(node.plane);
    const float radius=hull_support(plane.normal,hull);
    float a=static_cast<float>(original_plane_distance(start,plane)-radius);
    float b=static_cast<float>(original_plane_distance(finish,plane)-radius);
    bool hit=false;
    // Original expanded traversal 00424150 visits BACK first, then clips its
    // end to the back hit before checking FRONT. Thresholds do not bias t.
    if(a<0.001f || b<0.001f) {
        Vec3 local_start=start,local_end=finish;
        if(a>=0.001f)local_start=start+(finish-start)*(a/(a-b));
        else if(b>=0.001f)local_end=start+(finish-start)*(a/(a-b));
        hit=trace_node(node.children[1],local_start,local_end,direction,hull,mask,result,depth+1);
        if(hit)finish=result.end;
    }
    a=static_cast<float>(original_plane_distance(start,plane)+radius);
    b=static_cast<float>(original_plane_distance(finish,plane)+radius);
    if(a> -0.001f || b> -0.001f) {
        Vec3 local_start=start,local_end=finish;
        if(a<= -0.001f)local_start=start+(finish-start)*(a/(a-b));
        else if(b<= -0.001f)local_end=start+(finish-start)*(a/(a-b));
        const bool front_hit=trace_node(node.children[0],local_start,local_end,direction,hull,mask,result,depth+1);
        hit=hit||front_hit;
    }
    return hit;
}

bool CollisionWorld::trace_node_point(std::int32_t index,Vec3 start,Vec3 finish,
                                     Vec3 direction,std::uint32_t mask,
                                     Trace& result,std::size_t depth) {
    if(depth>level_.nodes.size())throw std::runtime_error("cyclic collision BSP");
    if(index<0)return trace_leaf(static_cast<std::uint32_t>(~index),start,finish,direction,{},mask,result);
    const auto& node=level_.nodes.at(static_cast<std::size_t>(index));
    const auto& plane=level_.planes.at(node.plane);
    const float a=static_cast<float>(original_plane_distance(start,plane));
    const float b=static_cast<float>(original_plane_distance(finish,plane));
    const unsigned side_a=a> -0.001f?0:1,side_b=b> -0.001f?0:1;
    if(side_a==side_b)return trace_node_point(node.children[side_a],start,finish,direction,mask,result,depth+1);
    const Vec3 middle=start+(finish-start)*(a/(a-b));
    if(trace_node_point(node.children[side_a],start,middle,direction,mask,result,depth+1))return true;
    return trace_node_point(node.children[side_b],middle,finish,direction,mask,result,depth+1);
}

Trace CollisionWorld::trace_segment(Vec3 start,Vec3 end,Vec3 direction,float travel,
                                    Bounds hull,std::uint32_t mask) {
    const auto finite=[](Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
    if(!finite(start)||!finite(end)||!finite(direction)||!finite(hull.minimum)||!finite(hull.maximum)||
       !std::isfinite(travel)||travel<0 || hull.minimum.x>hull.maximum.x||
       hull.minimum.y>hull.maximum.y||hull.minimum.z>hull.maximum.z)
        throw std::invalid_argument("invalid collision ray or hull");
    Trace result;
    result.end=end;
    result.distance=travel;
    if(level_.leaves.empty())throw std::runtime_error("collision BSP has no leaves");
    const Vec3 center=(hull.minimum+hull.maximum)*0.5f;
    const Vec3 extent=hull.maximum-hull.minimum;
    const auto root=level_.nodes.empty()?-1:0;
    const bool point=extent.x==0&&extent.y==0&&extent.z==0;
    result.hit=point ? trace_node_point(root,start+center,result.end+center,direction,mask,result,0) :
                      trace_node(root,start+center,result.end+center,direction,hull,mask,result,0);
    if(result.hit) {
        result.end=result.end-center;
        result.distance=static_cast<float>(original_dot(result.end-start,direction));
        result.fraction=travel!=0?result.distance/travel:0;
    }
    return result;
}
Trace CollisionWorld::trace_ray(Vec3 start,Vec3 direction,float travel,Bounds hull,std::uint32_t mask) {
    return trace_segment(start,start+direction*travel,direction,travel,hull,mask);
}
Trace CollisionWorld::trace_leaf(std::uint32_t leaf,Vec3 start,Vec3 end,Bounds hull,
                                std::uint32_t mask,Vec3 direction) const {
    Trace result;
    result.end=end;
    const Vec3 center=(hull.minimum+hull.maximum)*0.5f;
    result.hit=trace_leaf(leaf,start+center,end+center,direction,hull,mask,result);
    if(result.hit)result.end=result.end-center;
    return result;
}
Trace CollisionWorld::trace(Vec3 start,Vec3 end,Bounds hull,std::uint32_t mask) {
    const Vec3 delta=end-start;
    return trace_segment(start,end,original_normalized(delta),original_length(delta),hull,mask);
}

namespace {
constexpr float motion_tick=0.03333333507180214f;
constexpr float motion_half_tick_squared=0.0005555556272156537f;

void stop_mover(CharacterMotionState& state) noexcept {
    // Original 0041e120 / 0041e9a0 leave gas and command slots untouched.
    state.movement_speed=0;
    state.movement_step=0;
    state.movement_direction={};
}
void stop_gravity(CharacterMotionState& state) noexcept {
    state.vertical_speed=0;
    state.gravity_step=0;
}
Trace motion_trace(CollisionWorld& world,CharacterMotionHooks hooks,
                   Vec3 start,Vec3 direction,float distance,Bounds hull,
                   CharacterTraceKind kind=CharacterTraceKind::actor_hull) {
    return hooks.trace ? hooks.trace(hooks.context,start,direction,distance,hull,0x200,kind) :
                         world.trace_ray(start,direction,distance,hull,0x200);
}
Vec3 motion_projection(Vec3 endpoint,Vec3 contact,const Trace& hit,Bounds hull) noexcept {
    // 0041e7ff..0041e859 / 0041c4fd..0041c559: preserve the stored
    // float plane-distance intermediate rather than cancelling its terms.
    const Vec3 extent=(hull.maximum-hull.minimum)*0.5f;
    const float radius=static_cast<float>(
        (static_cast<long double>(std::abs(hit.normal.y))*extent.y+
         static_cast<long double>(std::abs(hit.normal.z))*extent.z)+
         static_cast<long double>(std::abs(hit.normal.x))*extent.x);
    const float a=static_cast<float>((original_dot(contact,hit.normal)-hit.plane_distance)-radius);
    const float scalar=static_cast<float>(a-((original_dot(endpoint,hit.normal)-hit.plane_distance)-radius));
    return endpoint+hit.normal*scalar;
}
bool advance_mover(CharacterMotionState& state,CharacterMotionInput input,
                   CollisionWorld& world,CharacterMotionHooks hooks) {
    const bool bypass=input.player&&input.movement_bypass;
    const auto posture=bypass ? 0 : state.posture_state;
    const bool airborne=!bypass&&state.airborne;
    if((state.movement_speed==0&&!state.input_gas)||(!state.fully_crouched&&posture!=0))
        return false;
    const auto& slots=state.movement_commands;
    float cap;
    if(posture==0&&!state.movement_walk&&!state.movement_command_flags[3])cap=state.run_speed;
    else if((slots[3].x==0&&slots[3].y==0&&slots[3].z==0)||posture!=0)cap=state.walk_speed;
    else cap=state.walk_speed*0.6200000047683716f;
    const float increment=state.acceleration*motion_tick;
    const float coefficient=increment*motion_tick;
    const float bias=state.acceleration*motion_half_tick_squared;
    float travel;
    if(state.input_gas) {
        travel=static_cast<float>(state.movement_step)*coefficient+bias;
        if(!airborne||state.movement_speed==0) {
            state.movement_direction=original_normalized({
                ((slots[3].x+slots[2].x)+slots[1].x)+slots[0].x,
                ((slots[2].y+slots[1].y)+slots[0].y)+slots[3].y,
                ((slots[3].z+slots[2].z)+slots[1].z)+slots[0].z});
            state.movement_speed+=increment;
            state.movement_command_flags[0]=(slots[0].y+slots[0].z)+slots[0].x!=0;
            state.movement_command_flags[1]=(slots[1].y+slots[1].z)+slots[1].x!=0;
            state.movement_command_flags[2]=(slots[2].y+slots[2].z)+slots[2].x!=0;
            state.movement_command_flags[3]=(slots[3].z+slots[3].y)+slots[3].x!=0;
        }
        if(state.movement_speed<=cap) {
            if(!airborne)++state.movement_step;
        } else {
            state.movement_speed=cap;
            state.movement_step=static_cast<std::int32_t>(cap/increment);
        }
    } else {
        travel=static_cast<float>(state.movement_step)*coefficient-bias;
        if(!airborne) {
            state.movement_speed-=increment;
            --state.movement_step;
        }
        if(state.movement_speed<0||state.movement_step<1)stop_mover(state);
    }
    Vec3 direction=state.movement_direction;
    const Vec3 start=state.position,endpoint=start+direction*travel;
    const Trace first=motion_trace(world,hooks,start,direction,travel,state.hull);
    if(!first.hit||bypass) {
        if(travel<0.005) { stop_mover(state);return false; }
        state.position=endpoint;
        return true;
    }
    // Original 0041e230: support-gated lift, then forward and down-40.
    if(motion_trace(world,hooks,start,state.gravity_direction,4,state.hull).hit) {
        const Trace ceiling=motion_trace(world,hooks,start,{0,0,1},20,state.hull);
        const float rise=ceiling.hit ? ceiling.distance : 20;
        Vec3 raised=start-state.gravity_direction*rise;
        if(!motion_trace(world,hooks,raised,direction,travel,state.hull).hit) {
            raised=raised+direction*travel;
            const Trace down=motion_trace(world,hooks,raised,state.gravity_direction,40,state.hull);
            if(!down.hit) { state.position=raised;return true; }
            if(down.normal.z>0.699999988079071f) {
                state.position=raised+state.gravity_direction*down.distance;
                return true;
            }
        }
    }
    Vec3 contact=start+direction*first.distance;
    const long double alignment=original_dot(first.normal,state.movement_direction);
    if(alignment> -1.01&&alignment< -0.99) {
        state.position=contact;
        if(first.distance<0.005)stop_mover(state);
        return true;
    }
    const Vec3 projected=motion_projection(endpoint,contact,first,state.hull)-contact;
    travel=static_cast<float>(std::sqrt(
        static_cast<long double>(projected.y)*projected.y+
        static_cast<long double>(projected.x)*projected.x));
    const float reciprocal=1.f/travel;
    direction={projected.x*reciprocal,projected.y*reciprocal,0.f*reciprocal};
    const Trace slide=motion_trace(world,hooks,contact,direction,travel,state.hull);
    if(slide.hit) { travel=slide.distance;stop_mover(state); }
    state.position=contact+direction*travel;
    if(travel<0.005)stop_mover(state);
    return true;
}
bool advance_gravity(CharacterMotionState& state,CharacterMotionInput input,
                     CollisionWorld& world,CharacterMotionHooks hooks) {
    Vec3 start=state.position;
    float height=state.hull.maximum.z;
    constexpr float crouch_height=32.673500061035156f,standing_height=49.08399963378906f;
    constexpr float posture_increment=1.0940333604812622f;
    if(state.posture_state==1||state.posture_state==3) {
        float delta=posture_increment;
        if(height-delta<crouch_height) {
            delta=height-crouch_height;
            state.posture_state=3;
            state.fully_crouched=true;
        } else state.fully_crouched=false;
        if(delta!=0) {
            height-=delta;
            state.hull.minimum.z=-height;state.hull.maximum.z=height;
            start.z-=delta*0.5f;
            state.position=start;
        }
    } else if(state.posture_state==2) {
        state.fully_crouched=false;
        float delta=posture_increment;
        if(height+delta>standing_height)delta=standing_height-height;
        if(delta==0) { state.posture_state=0;stop_mover(state); }
        else {
            const Trace up=motion_trace(world,hooks,start,{0,0,1},delta*2,state.hull);
            if(up.hit)delta=up.distance*0.5f;
            height+=delta;
            state.hull.minimum.z=-height;state.hull.maximum.z=height;
            start.z+=delta;
            state.position=start;
        }
    }
    const float increment=state.gravity*motion_tick;
    const float coefficient=increment*motion_tick;
    const float bias=state.gravity*motion_half_tick_squared;
    float travel;
    if(!state.jumping) {
        travel=static_cast<float>(state.gravity_step)*coefficient+bias;
        state.vertical_speed+=increment;
        ++state.gravity_step;
    } else {
        travel=static_cast<float>(state.gravity_step)*coefficient-bias;
        state.vertical_speed-=increment;
        --state.gravity_step;
        if(state.vertical_speed<0||state.gravity_step<1) {
            stop_gravity(state);travel=0;
            state.jumping=false;state.crouching=true;state.crouch_phase=2;
            state.gravity_direction=state.gravity_direction*-1;
        }
    }
    Vec3 direction=state.gravity_direction;
    const Vec3 endpoint=start+direction*travel;
    const Trace hit=motion_trace(world,hooks,start,direction,travel,state.hull);
    if(!hit.hit) {
        if(!state.airborne) {
            state.fall_time_tick=input.tick;
            if(!state.jumping)state.crouch_phase=2;
        }
        state.airborne=true;state.position=endpoint;
        return true;
    }
    if(!state.jumping) {
        if(state.airborne) {
            const auto phase=state.crouch_phase;
            state.airborne=false;state.crouch_phase=4;state.crouching=false;
            if(hooks.land&&phase!=5&&input.tick-state.fall_time_tick>input.ticks_per_second/2)
                hooks.land(hooks.context,start,hit);
        } else state.airborne=false;
        state.fall_time_tick=0;
    } else {
        state.jumping=false;
        state.gravity_direction=state.gravity_direction*-1;
    }
    Vec3 contact=start+direction*hit.distance;
    if(hit.normal.z<=0.699999988079071f) {
        const Vec3 projected=motion_projection(endpoint,contact,hit,state.hull);
        if(projected.x!=contact.x||projected.y!=contact.y||projected.z!=contact.z) {
            direction=projected-contact;
            const float norm=static_cast<float>(std::sqrt(original_dot(direction,direction)));
            direction=direction*(1.f/norm);
            travel=norm*3;
            const Trace slide=motion_trace(world,hooks,contact,direction,travel,state.hull);
            if(slide.hit)travel=slide.distance;
            contact=contact+direction*travel;
        }
    }
    stop_gravity(state);
    if(state.position.x==contact.x&&state.position.y==contact.y&&state.position.z==contact.z)return false;
    state.position=contact;
    return true;
}
}

bool advance_character(CharacterMotionState& state,CharacterMotionInput input,
                       CollisionWorld& world,CharacterMotionHooks hooks) {
    // Original EXE 004179f0 dispatches mover first, not gravity first.
    const bool moved=advance_mover(state,input,world,hooks);
    if(!input.player&&!moved&&!state.airborne) {
        state.airborne=false;state.jumping=false;
        return false;
    }
    if(input.player&&input.movement_bypass)return moved;
    const bool fell=advance_gravity(state,input,world,hooks);
    return moved||fell;
}
bool jump_character(CharacterMotionState& state,float speed,CollisionWorld& world,
                    CharacterMotionHooks hooks) {
    const Trace support=motion_trace(world,hooks,state.position,{0,0,-1},100,{},
                                    CharacterTraceKind::jump_support);
    if(!support.hit||original_dot({0,0,1},support.normal)<std::cos(0.7853981852531433L)||
       state.airborne||state.jumping)return false;
    state.jumping=true;state.crouch_phase=1;state.vertical_speed=speed;
    state.gravity_step=static_cast<std::int32_t>(speed/(state.gravity*motion_tick));
    state.gravity_direction=state.gravity_direction*-1;
    return true;
}

} // namespace pusu
