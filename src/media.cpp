#include "media.hpp"
#include "resources.hpp"
#include <SDL.h>
#include <SDL_mixer.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pusu {
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
bool same(Vec3 a,Vec3 b)noexcept{return a.x==b.x&&a.y==b.y&&a.z==b.z;}
float dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
void vector_check(Vec3 v){if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z))throw std::runtime_error("Non-finite audio position or basis");}
Vec3 normalized(Vec3 v){vector_check(v);const double length=std::sqrt(static_cast<double>(v.x)*v.x+static_cast<double>(v.y)*v.y+static_cast<double>(v.z)*v.z);if(length<1e-12)throw std::runtime_error("Zero sound listener basis");return {static_cast<float>(v.x/length),static_cast<float>(v.y/length),static_cast<float>(v.z/length)};}
float gain(float value){if(!std::isfinite(value))throw std::runtime_error("Non-finite sound volume");return std::clamp(value,0.f,1.f);}
void unit_check(float value){if(!std::isfinite(value)||value<0||value>1)throw std::runtime_error("Audio gain outside [0,1]");}
void minimum_check(float value){if(!std::isfinite(value)||value<=0||value>1e9f)throw std::runtime_error("Invalid sound minimum distance");}
void time_check(double value){if(!std::isfinite(value)||value<0)throw std::runtime_error("Invalid media playhead");}
void delta_check(float value){time_check(value);}
void environment_check(int value){if(value<0||value>2)throw std::runtime_error("Unknown sound environment");}
constexpr std::array<std::string_view,3> environment_names{"outdoor","indoor_small","indoor_large"};
constexpr std::pair<int,int> pool(int category,std::uint32_t owner){switch(category){case 0:return {0,0};case 4:return {1,1};case 3:return {2,41};case 2:return {42,57};case 1:return {58,89};default:if(owner>=90)throw std::runtime_error("Explicit sound channel is outside 90-channel pool");return {static_cast<int>(owner),static_cast<int>(owner)};}}
constexpr int category_for_channel(int index){return index==0?0:index==1?4:index<42?3:index<58?2:1;}
static_assert(pool(3,0).second-pool(3,0).first+1==40&&pool(2,0).second-pool(2,0).first+1==16&&pool(1,0).second-pool(1,0).first+1==32);
void sound_check(const SoundState& sound){if(!sound.id||sound.id==std::numeric_limits<std::uint64_t>::max()||sound.path.empty())throw std::runtime_error("Invalid saved sound identity or path");unit_check(sound.volume);vector_check(sound.position);minimum_check(sound.min_distance);time_check(sound.seconds);pool(sound.category,sound.owner);}
unsigned char path_char(unsigned char c)noexcept{if(c=='\\')return '/';return c>='A'&&c<='Z'?c-'A'+'a':c;}
struct PathHash {using is_transparent=void;std::size_t operator()(std::string_view path)const noexcept{std::size_t hash=1469598103934665603ull;for(const unsigned char c:path){hash^=path_char(c);hash*=1099511628211ull;}return hash;}};
struct PathEqual {using is_transparent=void;bool operator()(std::string_view a,std::string_view b)const noexcept{if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(path_char(a[i])!=path_char(b[i]))return false;return true;}};
void append_component(std::string& path,std::string_view component){if(component.empty())return;path.append(component);if(path.back()!='/'&&path.back()!='\\')path.push_back('/');}
} // namespace

namespace {
class OggStream final {
public:
    OggStream(const AssetStore& assets, std::string_view path, bool loop)
        : OggStream(std::make_shared<const std::vector<std::uint8_t>>(assets.bytes(path)), loop) {}

    OggStream(std::shared_ptr<const std::vector<std::uint8_t>> encoded, bool loop)
        : source_(std::move(encoded)), loop_(loop),
          pcm_(ring_frames * channels), positions_(ring_frames),
          converted_(conversion_frames * channels) {
        if (!source_.bytes || source_.bytes->size() < 4 ||
            std::memcmp(source_.bytes->data(), "OggS", 4) != 0)
            throw std::runtime_error("Invalid OGG stream");
        if (source_.bytes->size() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("OGG stream exceeds seekable input size");
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(avio_buffer_bytes));
        if (!buffer) throw std::bad_alloc();
        source_.io = avio_alloc_context(buffer, avio_buffer_bytes, 0, &source_,
                                        read_input, nullptr, seek_input);
        if (!source_.io) { av_free(buffer); throw std::bad_alloc(); }
        source_.io->seekable = AVIO_SEEKABLE_NORMAL;
        source_.format = avformat_alloc_context();
        if (!source_.format) throw std::bad_alloc();
        source_.format->pb = source_.io;
        source_.format->flags |= AVFMT_FLAG_CUSTOM_IO;
        source_.format->error_recognition = AV_EF_CRCCHECK | AV_EF_EXPLODE;
        const auto* ogg = av_find_input_format("ogg");
        if (!ogg) throw std::runtime_error("FFmpeg OGG demuxer unavailable");
        check(avformat_open_input(&source_.format, nullptr, ogg, nullptr), "Opening OGG stream");
        check(avformat_find_stream_info(source_.format, nullptr), "Reading OGG stream metadata");
        stream_ = av_find_best_stream(source_.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        check(stream_, "Finding OGG audio stream");
        const auto* stream = source_.format->streams[stream_];
        if (stream->time_base.num <= 0 || stream->time_base.den <= 0)
            throw std::runtime_error("Invalid OGG audio time base");
        origin_ = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        origin_sample_ = av_rescale_q(origin_, stream->time_base, output_time_base());
        if (origin_sample_ < -sample_limit || origin_sample_ > sample_limit)
            throw std::runtime_error("OGG stream timestamp origin is out of range");
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
            duration_ = stream->duration * av_q2d(stream->time_base);
        else if (source_.format->duration != AV_NOPTS_VALUE && source_.format->duration > 0)
            duration_ = static_cast<double>(source_.format->duration) / AV_TIME_BASE;
        if (!std::isfinite(duration_) || duration_ <= 0 || duration_ > static_cast<double>(sample_limit) / rate)
            throw std::runtime_error("OGG stream has no valid authored duration");
        const auto* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) throw std::runtime_error("FFmpeg OGG audio codec unavailable");
        source_.codec = avcodec_alloc_context3(decoder);
        if (!source_.codec) throw std::bad_alloc();
        check(avcodec_parameters_to_context(source_.codec, stream->codecpar), "Reading OGG codec parameters");
        source_.codec->pkt_timebase = stream->time_base;
        source_.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_EXPLODE;
        check(avcodec_open2(source_.codec, decoder, nullptr), "Opening OGG audio codec");
        source_.packet = av_packet_alloc();
        source_.frame = av_frame_alloc();
        if (!source_.packet || !source_.frame) throw std::bad_alloc();
        prime();
        if (written_.load(std::memory_order_relaxed) == 0)
            throw std::runtime_error("OGG stream has no decodable PCM samples");
    }

    ~OggStream() = default;
    OggStream(const OggStream&) = delete;
    OggStream& operator=(const OggStream&) = delete;
    OggStream(OggStream&&) = delete;
    OggStream& operator=(OggStream&&) = delete;

    // Bounded work and storage. FFmpeg owns its codec/demux scratch allocations;
    // this class never resizes/allocates its PCM buffers while pumping audio.
    void update() {
        if (producer_done_.load(std::memory_order_relaxed)) return;
        for (unsigned work = 0; work < pump_steps; ++work) {
            publish_pending();
            if (pending_offset_ != pending_frames_ || free_frames() == 0) return;
            if (decoder_eof_) {
                if (source_.swr) {
                    auto* output = reinterpret_cast<std::uint8_t*>(converted_.data());
                    const int frames = swr_convert(source_.swr, &output, conversion_frames, nullptr, 0);
                    check(frames, "Draining OGG audio resampler");
                    if (frames > 0) { retain_output(frames); continue; }
                }
                // A metadata-valid restored position must have actual tail
                // PCM. Looping must not hide a seek past the decoded endpoint.
                if (require_seek_tail_ && written_.load(std::memory_order_relaxed) == 0)
                    throw std::runtime_error("OGG seek target has no decoded audio before end of stream");
                // next_sample_ is the actual decoded/resampled endpoint, not a
                // wall-clock estimate or the length of a requested seek tail.
                if (have_position_ && next_sample_ > 0)
                    loop_end_.store(static_cast<std::uint64_t>(next_sample_), std::memory_order_release);
                if (!loop_) {
                    if (!cycle_decoded_ && discard_before_ == 0)
                        throw std::runtime_error("OGG stream contains no decoded audio");
                    producer_done_.store(true, std::memory_order_release);
                    return;
                }
                if ((!cycle_decoded_ || next_sample_ <= 0) && discard_before_ == 0)
                    throw std::runtime_error("Cannot loop OGG stream without decoded audio");
                // A restored tail is followed by a genuine seek to the full
                // original stream; never loop only the restored suffix.
                reset_decoder(0);
                continue;
            }
            const int received = avcodec_receive_frame(source_.codec, source_.frame);
            if (received == 0) {
                convert_frame();
                av_frame_unref(source_.frame);
                continue;
            }
            if (received == AVERROR_EOF) { decoder_eof_ = true; continue; }
            if (received != AVERROR(EAGAIN)) check(received, "Decoding OGG audio");
            if (demux_eof_) {
                if (flush_sent_)
                    throw std::runtime_error("OGG codec stalled while draining decoded audio");
                const int sent = avcodec_send_packet(source_.codec, nullptr);
                if (sent != AVERROR_EOF) check(sent, "Flushing OGG audio decoder");
                flush_sent_ = true;
                continue;
            }
            const int fetched = av_read_frame(source_.format, source_.packet);
            if (fetched == AVERROR_EOF) { demux_eof_ = true; continue; }
            check(fetched, "Demuxing OGG audio");
            if (source_.packet->flags & AV_PKT_FLAG_CORRUPT)
                throw std::runtime_error("Corrupted OGG packet");
            if (source_.packet->stream_index == stream_)
                check(avcodec_send_packet(source_.codec, source_.packet), "Submitting OGG audio packet");
            av_packet_unref(source_.packet);
        }
        publish_pending();
    }

    // Parent stages seek before publication, or halts/detaches the callback,
    // and suspends update() for this operation.
    // The target is rounded up to the first real output sample not before it.
    void seek(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > duration_)
            throw std::runtime_error("OGG seek is outside authored audio duration");
        const auto target = static_cast<std::int64_t>(std::ceil(seconds * rate));
        reset_decoder(target);
        read_.store(0, std::memory_order_relaxed);
        written_.store(0, std::memory_order_relaxed);
        playhead_.store(static_cast<std::uint64_t>(target), std::memory_order_relaxed);
        producer_done_.store(false, std::memory_order_relaxed);
        require_seek_tail_ = seconds < duration_;
        try { prime(); }
        catch (...) { require_seek_tail_ = false; throw; }
        require_seek_tail_ = false;
    }

    double seconds() const noexcept {
        auto position = playhead_.load(std::memory_order_acquire);
        const auto end = loop_end_.load(std::memory_order_acquire);
        if (loop_ && end) position %= end;
        return std::min(static_cast<double>(position) / rate, duration_);
    }

    double duration() const noexcept { return duration_; }

    bool finished() const noexcept {
        if (!producer_done_.load(std::memory_order_acquire)) return false;
        return read_.load(std::memory_order_acquire) == written_.load(std::memory_order_acquire);
    }

    // SDL_mixer's effect receives stereo AUDIO_S16SYS bytes from a tiny looping
    // silent carrier. Replace them with real decoded PCM; underrun padding is
    // neither consumed audio nor a playhead advance. No locks/allocations/throws.
    void render(void* bytes, int length) noexcept {
        if (length <= 0 || !bytes) return;
        auto* output = static_cast<std::uint8_t*>(bytes);
        const auto read = read_.load(std::memory_order_relaxed);
        const auto written = written_.load(std::memory_order_acquire);
        const auto available = written - read;
        const auto requested = static_cast<std::uint64_t>(length / bytes_per_frame);
        const auto count = static_cast<std::size_t>(std::min(requested, available));
        const auto first = static_cast<std::size_t>(read % ring_frames);
        const auto contiguous = std::min(count, ring_frames - first);
        if (contiguous)
            std::memcpy(output, pcm_.data() + first * channels, contiguous * bytes_per_frame);
        if (count > contiguous)
            std::memcpy(output + contiguous * bytes_per_frame, pcm_.data(),
                        (count - contiguous) * bytes_per_frame);
        const auto copied = count * bytes_per_frame;
        if (copied < static_cast<std::size_t>(length))
            std::memset(output + copied, 0, static_cast<std::size_t>(length) - copied);
        if (count) {
            const auto position = positions_[(read + count - 1) % ring_frames];
            playhead_.store(position, std::memory_order_release);
            read_.store(read + count, std::memory_order_release);
        }
    }

private:
    static constexpr int rate = 44100;
    static constexpr int channels = 2;
    static constexpr int bytes_per_frame = channels * sizeof(std::int16_t);
    static constexpr std::size_t ring_frames = rate * 3;
    static constexpr int conversion_frames = (1024 * 1024) / bytes_per_frame;
    static constexpr int avio_buffer_bytes = 32768;
    static constexpr unsigned pump_steps = 256;
    static constexpr std::int64_t sample_limit = std::numeric_limits<std::int64_t>::max() / 4;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "OGG callback requires lock-free 64-bit atomics");
    static constexpr AVRational output_time_base() noexcept { return {1, rate}; }

    // A fully constructed RAII member cleans up even if OggStream construction
    // throws. AVFormat does not own our custom AVIO buffer/context.
    struct Source {
        explicit Source(std::shared_ptr<const std::vector<std::uint8_t>> data)
            : bytes(std::move(data)) {}
        ~Source() {
            swr_free(&swr);
            av_channel_layout_uninit(&input_layout);
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&codec);
            avformat_close_input(&format);
            if (io) { av_freep(&io->buffer); avio_context_free(&io); }
        }
        std::shared_ptr<const std::vector<std::uint8_t>> bytes;
        std::int64_t cursor{};
        AVIOContext* io{};
        AVFormatContext* format{};
        AVCodecContext* codec{};
        AVPacket* packet{};
        AVFrame* frame{};
        SwrContext* swr{};
        AVChannelLayout input_layout{};
        int input_rate{};
        AVSampleFormat input_format{AV_SAMPLE_FMT_NONE};
    } source_;
    const bool loop_;
    std::vector<std::int16_t> pcm_;
    // Each queued frame carries its source playhead after consumption. This
    // preserves seek and loop boundaries even when one callback spans both.
    std::vector<std::uint64_t> positions_;
    std::vector<std::int16_t> converted_;
    std::atomic<std::uint64_t> read_{0}, written_{0}, playhead_{0}, loop_end_{0};
    std::atomic<bool> producer_done_{false};
    int stream_{-1};
    double duration_{};
    std::int64_t origin_{}, origin_sample_{}, next_sample_{}, discard_before_{};
    std::int64_t pending_start_{};
    std::size_t pending_frames_{}, pending_offset_{};
    bool have_position_{}, demux_eof_{}, flush_sent_{}, decoder_eof_{}, cycle_decoded_{};
    bool require_seek_tail_{};

    static void check(int result, std::string_view operation) {
        if (result >= 0) return;
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }

    static int read_input(void* opaque, std::uint8_t* output, int length) noexcept {
        auto& source = *static_cast<Source*>(opaque);
        if (length <= 0) return AVERROR(EINVAL);
        const auto remaining = static_cast<std::int64_t>(source.bytes->size()) - source.cursor;
        if (remaining == 0) return AVERROR_EOF;
        const auto count = static_cast<int>(std::min<std::int64_t>(length, remaining));
        std::memcpy(output, source.bytes->data() + static_cast<std::size_t>(source.cursor),
                    static_cast<std::size_t>(count));
        source.cursor += count;
        return count;
    }

    static std::int64_t seek_input(void* opaque, std::int64_t offset, int whence) noexcept {
        auto& source = *static_cast<Source*>(opaque);
        const auto size = static_cast<std::int64_t>(source.bytes->size());
        if (whence & AVSEEK_SIZE) return size;
        whence &= ~AVSEEK_FORCE;
        std::int64_t base;
        if (whence == SEEK_SET) base = 0;
        else if (whence == SEEK_CUR) base = source.cursor;
        else if (whence == SEEK_END) base = size;
        else return AVERROR(EINVAL);
        if (offset < -base || offset > size - base) return AVERROR(EINVAL);
        source.cursor = base + offset;
        return source.cursor;
    }

    std::size_t free_frames() const noexcept {
        const auto written = written_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        return ring_frames - static_cast<std::size_t>(written - read);
    }

    void publish_pending() noexcept {
        if (pending_offset_ == pending_frames_) return;
        const auto written = written_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        const auto count = std::min(pending_frames_ - pending_offset_,
                                   ring_frames - static_cast<std::size_t>(written - read));
        const auto first = static_cast<std::size_t>(written % ring_frames);
        const auto contiguous = std::min(count, ring_frames - first);
        if (contiguous)
            std::memcpy(pcm_.data() + first * channels,
                        converted_.data() + pending_offset_ * channels, contiguous * bytes_per_frame);
        if (count > contiguous)
            std::memcpy(pcm_.data(), converted_.data() + (pending_offset_ + contiguous) * channels,
                        (count - contiguous) * bytes_per_frame);
        for (std::size_t i = 0; i < count; ++i)
            positions_[(written + i) % ring_frames] = static_cast<std::uint64_t>(
                pending_start_ + static_cast<std::int64_t>(pending_offset_ + i) + 1);
        pending_offset_ += count;
        written_.store(written + count, std::memory_order_release);
    }

    void retain_output(int frames) {
        if (frames < 0 || frames > conversion_frames || next_sample_ > sample_limit - frames)
            throw std::runtime_error("OGG decoded output exceeds bounded audio capacity");
        pending_start_ = next_sample_;
        pending_frames_ = static_cast<std::size_t>(frames);
        const auto skipped = std::clamp<std::int64_t>(discard_before_ - next_sample_, 0, frames);
        pending_offset_ = static_cast<std::size_t>(skipped);
        next_sample_ += frames;
    }

    void convert_frame() {
        auto* frame = source_.frame;
        if (frame->flags & AV_FRAME_FLAG_CORRUPT)
            throw std::runtime_error("Corrupted decoded OGG audio frame");
        if (frame->nb_samples <= 0 || frame->sample_rate <= 0 ||
            frame->ch_layout.nb_channels <= 0 || frame->ch_layout.nb_channels > 64 ||
            !av_channel_layout_check(&frame->ch_layout) ||
            av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format)) <= 0)
            throw std::runtime_error("Invalid decoded OGG audio format");
        if (!source_.swr) {
            source_.input_rate = frame->sample_rate;
            source_.input_format = static_cast<AVSampleFormat>(frame->format);
            check(av_channel_layout_copy(&source_.input_layout, &frame->ch_layout), "Retaining OGG channel layout");
            const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            check(swr_alloc_set_opts2(&source_.swr, &stereo, AV_SAMPLE_FMT_S16, rate,
                                      &source_.input_layout, source_.input_format, source_.input_rate,
                                      0, nullptr), "Creating OGG stereo resampler");
            check(swr_init(source_.swr), "Initializing OGG stereo resampler");
        } else if (frame->sample_rate != source_.input_rate || frame->format != source_.input_format ||
                   av_channel_layout_compare(&frame->ch_layout, &source_.input_layout) != 0) {
            throw std::runtime_error("OGG audio format changes within a streamed sound");
        }
        if (!have_position_) {
            const auto stamp = frame->best_effort_timestamp != AV_NOPTS_VALUE ?
                               frame->best_effort_timestamp : frame->pts;
            if (stamp == AV_NOPTS_VALUE) {
                if (discard_before_ != 0)
                    throw std::runtime_error("OGG seek cannot establish decoded sample timestamps");
                next_sample_ = 0;
            } else {
                const auto sample = av_rescale_q(stamp, source_.format->streams[stream_]->time_base,
                                                output_time_base());
                if (sample < -sample_limit || sample > sample_limit)
                    throw std::runtime_error("OGG decoded timestamp is out of range");
                next_sample_ = sample - origin_sample_;
                if (next_sample_ < -sample_limit || next_sample_ > sample_limit)
                    throw std::runtime_error("OGG decoded sample origin is out of range");
            }
            have_position_ = true;
        }
        const int capacity = swr_get_out_samples(source_.swr, frame->nb_samples);
        check(capacity, "Sizing bounded OGG audio conversion");
        if (capacity > conversion_frames)
            throw std::runtime_error("OGG decoded frame exceeds fixed 1 MiB conversion buffer");
        std::array<const std::uint8_t*, 64> input;
        const int planes = av_sample_fmt_is_planar(source_.input_format) ? frame->ch_layout.nb_channels : 1;
        for (int i = 0; i < planes; ++i) {
            input[static_cast<std::size_t>(i)] = frame->extended_data[i];
            if (!input[static_cast<std::size_t>(i)])
                throw std::runtime_error("OGG decoded frame is missing an audio plane");
        }
        auto* output = reinterpret_cast<std::uint8_t*>(converted_.data());
        const int frames = swr_convert(source_.swr, &output, conversion_frames,
                                       input.data(), frame->nb_samples);
        check(frames, "Converting streamed OGG audio to stereo S16");
        cycle_decoded_ = true;
        retain_output(frames);
    }

    void reset_decoder(std::int64_t target) {
        const auto* stream = source_.format->streams[stream_];
        const auto relative = av_rescale_q_rnd(target, output_time_base(), stream->time_base, AV_ROUND_DOWN);
        if ((origin_ > 0 && relative > std::numeric_limits<std::int64_t>::max() - origin_) ||
            (origin_ < 0 && relative < std::numeric_limits<std::int64_t>::min() - origin_))
            throw std::runtime_error("OGG seek timestamp is out of range");
        check(av_seek_frame(source_.format, stream_, origin_ + relative, AVSEEK_FLAG_BACKWARD),
              "Seeking original OGG audio stream");
        avcodec_flush_buffers(source_.codec);
        av_packet_unref(source_.packet);
        av_frame_unref(source_.frame);
        if (source_.swr) {
            swr_close(source_.swr);
            check(swr_init(source_.swr), "Resetting OGG resampler after seek");
        }
        discard_before_ = target;
        pending_frames_ = pending_offset_ = 0;
        next_sample_ = 0;
        have_position_ = demux_eof_ = flush_sent_ = decoder_eof_ = cycle_decoded_ = false;
    }

    void prime() {
        // Pre-publication constructor/seek work may inspect preroll until the
        // first audible sample, while each individual update remains bounded.
        do { update(); }
        while (read_.load(std::memory_order_relaxed) == written_.load(std::memory_order_relaxed) &&
               !producer_done_.load(std::memory_order_relaxed));
    }
};

class AviDecoder final {
    static constexpr std::size_t packet_capacity = 128;

    static void check(int result, const char* operation) {
        if (result >= 0) return;
        char text[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, text, sizeof(text));
        throw std::runtime_error(std::string(operation) + ": " + text);
    }
    struct Packets {
        std::array<AVPacket*, packet_capacity> slots{};
        std::size_t first{}, count{};
        ~Packets() { for (auto*& packet : slots) av_packet_free(&packet); }
        void allocate() {
            for (auto*& packet : slots) {
                packet = av_packet_alloc();
                if (!packet) throw std::bad_alloc();
            }
        }
        void clear() noexcept {
            for (auto* packet : slots) if (packet) av_packet_unref(packet);
            first = count = 0;
        }
        bool full() const noexcept { return count == slots.size(); }
        AVPacket* front() noexcept { return slots[first]; }
        void push(AVPacket* packet) noexcept {
            av_packet_move_ref(slots[(first + count) % slots.size()], packet);
            ++count;
        }
        void pop() noexcept {
            av_packet_unref(slots[first]);
            first = (first + 1) % slots.size();
            --count;
        }
    };
    // A member owner, rather than constructor-body cleanup, also covers partial construction.
    struct Resources {
        std::vector<std::uint8_t> bytes;
        std::int64_t offset{};
        AVIOContext* io{};
        AVFormatContext* format{};
        AVCodecContext* video{};
        AVPacket* demux_packet{};
        AVFrame* video_frame{};
        SwsContext* scaler{};
        Packets video_packets;
        ~Resources() {
            sws_freeContext(scaler);
            av_frame_free(&video_frame);
            av_packet_free(&demux_packet);
            avcodec_free_context(&video);
            avformat_close_input(&format);
            if (io) {
                // libavformat may replace the original AVIO buffer during probing.
                av_freep(&io->buffer);
                avio_context_free(&io);
            }
        }
    } resources_;
    std::vector<std::uint8_t> pixels_, next_pixels_;
    int video_stream_{-1}, width_{}, height_{};
    bool demux_eof_{}, video_eof_{}, video_flush_sent_{}, pending_video_{}, have_picture_{};
    double picture_time_{}, next_time_{};
    struct TextureInfo {
        std::uint32_t scale{}, rate{}, start{}, frames{}, frame_ms{};
        bool ready{};
    };
    mutable TextureInfo texture_;
    std::int64_t picture_index_{-1}, next_index_{-1};
    std::uint32_t requested_index_{};

    void texture_info() const {
        if (texture_.ready) return;
        const auto& bytes = resources_.bytes;
        const auto word = [&](std::size_t at) -> std::uint32_t {
            if (at > bytes.size() || bytes.size() - at < 4)
                throw std::runtime_error("Truncated AVI stream metadata");
            return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1]) << 8) |
                (std::uint32_t(bytes[at + 2]) << 16) | (std::uint32_t(bytes[at + 3]) << 24);
        };
        const auto tag = [&](std::size_t at, const char* name) {
            return word(at) == (std::uint32_t(static_cast<unsigned char>(name[0])) |
                (std::uint32_t(static_cast<unsigned char>(name[1])) << 8) |
                (std::uint32_t(static_cast<unsigned char>(name[2])) << 16) |
                (std::uint32_t(static_cast<unsigned char>(name[3])) << 24));
        };
        if (bytes.size() < 12 || !tag(0, "RIFF") || !tag(8, "AVI ") ||
            word(4) < 4 || word(4) > bytes.size() - 8)
            throw std::runtime_error("Invalid AVI RIFF metadata extent");
        TextureInfo info;
        int stream_index = -1;
        bool found = false;
        // Only the RIFF/hdrl/strl metadata hierarchy is traversed; movi and unknown chunks are skipped.
        const auto walk = [&](auto&& self, std::size_t begin, std::size_t end, int level) -> void {
            for (auto at = begin; at < end;) {
                if (end - at < 8) throw std::runtime_error("Truncated AVI metadata chunk");
                const auto size = word(at + 4);
                const auto payload = at + 8;
                if (size > end - payload) throw std::runtime_error("Invalid AVI metadata chunk extent");
                const auto chunk_end = payload + size;
                if ((size & 1u) && chunk_end == end)
                    throw std::runtime_error("Missing AVI metadata chunk padding");
                if (tag(at, "LIST")) {
                    if (size < 4) throw std::runtime_error("Truncated AVI metadata LIST");
                    if (level == 0 && tag(payload, "hdrl"))
                        self(self, payload + 4, chunk_end, 1);
                    else if (level == 1 && tag(payload, "strl")) {
                        if (stream_index == std::numeric_limits<int>::max())
                            throw std::runtime_error("Too many AVI stream headers");
                        ++stream_index;
                        self(self, payload + 4, chunk_end, 2);
                    }
                } else if (level == 2 && tag(at, "strh") && stream_index == video_stream_) {
                    if (found || size < 56 || !tag(payload, "vids"))
                        throw std::runtime_error("Invalid AVI video stream header");
                    info.scale = word(payload + 20);
                    info.rate = word(payload + 24);
                    info.start = word(payload + 28);
                    info.frames = word(payload + 32);
                    found = true;
                }
                at = chunk_end + (size & 1u);
            }
        };
        walk(walk, 12, std::size_t(word(4)) + 8, 0);
        if (!found || !info.scale || !info.rate || !info.frames ||
            info.frames > std::uint32_t(std::numeric_limits<std::int32_t>::max()))
            throw std::runtime_error("Empty or invalid indexed AVI timing metadata");
        // Windows' imported SampleToTime rounding was not supplied. Bound its integer result,
        // rather than claim a DLL formula: all retail material AVIs yield the same quotient
        // for floor, nearest, or ceil total milliseconds (66 ms TVs/subtitles, 40 ms gallery).
        const auto lower = av_rescale_rnd(info.frames, std::int64_t(info.scale) * 1000,
                                         info.rate, AV_ROUND_DOWN);
        const auto upper = av_rescale_rnd(info.frames, std::int64_t(info.scale) * 1000,
                                         info.rate, AV_ROUND_UP);
        if (lower < 0 || upper < lower || upper > std::numeric_limits<std::int32_t>::max() ||
            lower / info.frames == 0 || lower / info.frames != upper / info.frames)
            throw std::runtime_error("Unrepresentable or rounding-dependent indexed AVI frame milliseconds");
        info.frame_ms = static_cast<std::uint32_t>(lower / info.frames);
        info.ready = true;
        texture_ = info;
    }
    std::int64_t indexed_timestamp(const AVFrame* frame) const {
        auto stamp = frame->best_effort_timestamp;
        if (stamp == AV_NOPTS_VALUE) stamp = frame->pts;
        if (stamp == AV_NOPTS_VALUE) stamp = frame->pkt_dts;
        const auto base = resources_.format->streams[video_stream_]->time_base;
        if (stamp == AV_NOPTS_VALUE || base.num <= 0 || base.den <= 0)
            throw std::runtime_error("Indexed AVI frame has no generated sample timestamp");
        const auto numerator = std::int64_t(base.num) * texture_.rate;
        const auto denominator = std::int64_t(base.den) * texture_.scale;
        const auto sample = av_rescale_rnd(stamp, numerator, denominator, AV_ROUND_NEAR_INF);
        // A timestamp must uniquely identify an authored sample, not approximate a PTS time.
        if (numerator > denominator || sample < texture_.start ||
            sample - texture_.start >= texture_.frames ||
            av_rescale_rnd(sample, denominator, numerator, AV_ROUND_NEAR_INF) != stamp)
            throw std::runtime_error("Indexed AVI timestamp does not identify an original sample");
        return sample;
    }
    void seek_index(std::uint32_t index) {
        const auto base = resources_.format->streams[video_stream_]->time_base;
        if (base.num <= 0 || base.den <= 0) throw std::runtime_error("Invalid indexed AVI time base");
        const auto stamp = av_rescale_rnd(index,
            std::int64_t(texture_.scale) * base.den, std::int64_t(texture_.rate) * base.num, AV_ROUND_DOWN);
        check(av_seek_frame(resources_.format, video_stream_, stamp, AVSEEK_FLAG_BACKWARD),
              "Seeking indexed AVI sample");
        avcodec_flush_buffers(resources_.video);
        resources_.video_packets.clear();
        av_packet_unref(resources_.demux_packet);
        av_frame_unref(resources_.video_frame);
        demux_eof_ = video_eof_ = video_flush_sent_ = pending_video_ = have_picture_ = false;
        picture_index_ = next_index_ = -1;
    }

    static int read_bytes(void* opaque, std::uint8_t* destination, int count) noexcept {
        auto& r = *static_cast<Resources*>(opaque);
        if (count <= 0) return AVERROR(EINVAL);
        const auto available = static_cast<std::int64_t>(r.bytes.size()) - r.offset;
        if (available == 0) return AVERROR_EOF;
        const auto copied = std::min<std::int64_t>(available, count);
        std::memcpy(destination, r.bytes.data() + r.offset, static_cast<std::size_t>(copied));
        r.offset += copied;
        return static_cast<int>(copied);
    }
    static std::int64_t seek_bytes(void* opaque, std::int64_t offset, int whence) noexcept {
        auto& r = *static_cast<Resources*>(opaque);
        const auto size = static_cast<std::int64_t>(r.bytes.size());
        if (whence & AVSEEK_SIZE) return size;
        whence &= ~AVSEEK_FORCE;
        std::int64_t base;
        switch (whence) {
            case SEEK_SET: base = 0; break;
            case SEEK_CUR: base = r.offset; break;
            case SEEK_END: base = size; break;
            default: return AVERROR(EINVAL);
        }
        if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 && offset < -base)) return AVERROR(EINVAL);
        const auto position = base + offset;
        if (position < 0 || position > size) return AVERROR(EINVAL);
        r.offset = position;
        return position;
    }
    AVCodecContext* open_codec(int index) {
        const auto* parameters = resources_.format->streams[index]->codecpar;
        const auto* decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder) throw std::runtime_error("AVI codec is unavailable");
        auto* context = avcodec_alloc_context3(decoder);
        if (!context) throw std::bad_alloc();
        try {
            check(avcodec_parameters_to_context(context, parameters), "AVI codec parameters");
            context->pkt_timebase = resources_.format->streams[index]->time_base;
            check(avcodec_open2(context, decoder, nullptr), "Opening AVI codec");
        } catch (...) {
            avcodec_free_context(&context);
            throw;
        }
        return context;
    }
    bool receive_video() {
        if (pending_video_ || video_eof_) return false;
        const int result = avcodec_receive_frame(resources_.video, resources_.video_frame);
        if (result == AVERROR_EOF) { video_eof_ = true; return true; }
        if (result == AVERROR(EAGAIN)) return false;
        check(result, "Decoding AVI video");
        auto* frame = resources_.video_frame;
        if (frame->width <= 0 || frame->height <= 0) throw std::runtime_error("Invalid decoded AVI video frame");
        next_index_ = indexed_timestamp(frame);
        next_time_ = static_cast<double>(next_index_ - texture_.start) * texture_.scale / texture_.rate;
        if (next_index_ != requested_index_) {
            av_frame_unref(frame);
            pending_video_ = true;
            return true; // Keyframe preroll needs decoding, not RGBA conversion.
        }
        resources_.scaler = sws_getCachedContext(resources_.scaler, frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format), width_, height_, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!resources_.scaler) throw std::runtime_error("AVI RGBA converter is unavailable");
        std::uint8_t* output[]{next_pixels_.data()};
        int strides[]{width_ * 4};
        const int lines = sws_scale(resources_.scaler, frame->data, frame->linesize, 0, frame->height, output, strides);
        if (lines != height_) throw std::runtime_error("Incomplete AVI RGBA conversion");
        av_frame_unref(frame);
        pending_video_ = true;
        return true;
    }
    bool send_packets(AVCodecContext* codec, Packets& queue, bool& flushed, bool blocked, const char* operation) {
        if (!codec || blocked || flushed) return false;
        if (queue.count) {
            const int result = avcodec_send_packet(codec, queue.front());
            if (result == AVERROR(EAGAIN)) return false; // Keep the exact packet until receive makes room.
            check(result, operation);
            queue.pop();
            return true;
        }
        if (demux_eof_) {
            const int result = avcodec_send_packet(codec, nullptr);
            if (result == AVERROR(EAGAIN)) return false; // Drain received frames before retrying null.
            if (result != AVERROR_EOF) check(result, operation);
            flushed = true;
            return true;
        }
        return false;
    }
    bool demux() {
        if (demux_eof_ || video_eof_ || pending_video_ || resources_.video_packets.full()) return false;
        const int result = av_read_frame(resources_.format, resources_.demux_packet);
        if (result == AVERROR_EOF) { demux_eof_ = true; return true; }
        check(result, "Demuxing AVI");
        if (resources_.demux_packet->stream_index == video_stream_)
            resources_.video_packets.push(resources_.demux_packet);
        else av_packet_unref(resources_.demux_packet);
        return true;
    }

public:
    AviDecoder(const AssetStore& assets, std::string_view path) {
        auto& r = resources_;
        r.bytes = assets.bytes(path);
        if (r.bytes.empty() || r.bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("Empty or oversized AVI: " + std::string(path));
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(32768));
        if (!buffer) throw std::bad_alloc();
        r.io = avio_alloc_context(buffer, 32768, 0, &r, read_bytes, nullptr, seek_bytes);
        if (!r.io) { av_free(buffer); throw std::bad_alloc(); }
        r.io->seekable = AVIO_SEEKABLE_NORMAL;
        r.format = avformat_alloc_context();
        if (!r.format) throw std::bad_alloc();
        r.format->pb = r.io;
        r.format->flags |= AVFMT_FLAG_CUSTOM_IO;
        check(avformat_open_input(&r.format, nullptr, nullptr, nullptr), "Opening protected AVI bytes");
        check(avformat_find_stream_info(r.format, nullptr), "Reading AVI stream information");
        video_stream_ = av_find_best_stream(r.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(video_stream_, "Finding AVI video stream");
        texture_info();
        r.video = open_codec(video_stream_);
        width_ = r.video->width;
        height_ = r.video->height;
        if (width_ <= 0 || height_ <= 0 || width_ > 16384 || height_ > 16384)
            throw std::runtime_error("Invalid AVI dimensions");
        const auto pixel_count = static_cast<std::size_t>(width_) * height_ * 4;
        pixels_.resize(pixel_count);
        next_pixels_.resize(pixel_count);
        r.demux_packet = av_packet_alloc();
        r.video_frame = av_frame_alloc();
        if (!r.demux_packet || !r.video_frame) throw std::bad_alloc();
        r.video_packets.allocate();
    }
    AviDecoder(const AviDecoder&) = delete;
    AviDecoder& operator=(const AviDecoder&) = delete;
    ~AviDecoder() = default;

    std::uint32_t texture_rate() const { texture_info(); return texture_.rate; }
    std::uint32_t texture_frames() const { texture_info(); return texture_.frames; }
    std::uint32_t texture_frame_ms() const { texture_info(); return texture_.frame_ms; }
    int video_width() const noexcept { return width_; }
    int video_height() const noexcept { return height_; }
    VideoFrame select_frame(std::uint32_t index) {
        texture_info();
        if (index < texture_.start || std::uint64_t(index) - texture_.start >= texture_.frames)
            throw std::runtime_error("AVI sample index is beyond its stream sample range");
        if (have_picture_ && picture_index_ == index) return frame();
        requested_index_ = index;
        // Keep the decoder and compressed queues for adjacent samples; seeks start at an indexed
        // keyframe near the requested sample, never an unconditional replay from frame zero.
        if (!have_picture_ || std::int64_t(index) != picture_index_ + 1)
            seek_index(index);
        for (;;) {
            if (pending_video_) {
                if (next_index_ > index) throw std::runtime_error("Requested AVI sample has no decoded frame");
                pending_video_ = false;
                if (next_index_ == index) {
                    pixels_.swap(next_pixels_);
                    picture_time_ = next_time_;
                    picture_index_ = next_index_;
                    have_picture_ = true;
                    return frame();
                }
            }
            if (video_eof_) throw std::runtime_error("AVI ended before the requested sample");
            bool progress = receive_video();
            progress = send_packets(resources_.video, resources_.video_packets, video_flush_sent_,
                                    pending_video_ || video_eof_, "Submitting indexed AVI packet") || progress;
            progress = demux() || progress;
            if (!progress) throw std::runtime_error("Indexed AVI decoding made no progress");
        }
    }
    VideoFrame frame() const noexcept {
        if (!have_picture_) return {width_, height_, {}, 0};
        return {width_, height_, pixels_, picture_time_};
    }
};
} // namespace

struct VideoTexture::Impl {
    AviDecoder movie;
    std::uint32_t start{},last{},rate{},count{},milliseconds{};
    Impl(const AssetStore& assets,std::string_view path,std::uint32_t initial):movie(assets,path),start(initial),last(initial){
        rate=movie.texture_rate();count=movie.texture_frames();milliseconds=movie.texture_frame_ms();
    }
    void refresh(std::uint32_t now){
        if(static_cast<std::uint32_t>(now-last)<=1000u/rate)return;
        last=now;
        // 401768 explicitly rounds elapsed milliseconds to FLOAT32; the
        // subsequent x87 operations and _ftol truncation use extended precision.
        const long double elapsed=static_cast<float>(static_cast<std::uint32_t>(now-start));
        auto index=static_cast<std::uint64_t>(((elapsed/1000.L)*1000.L)/milliseconds);
        if(index>=count){start=now;index=0;}
        movie.select_frame(static_cast<std::uint32_t>(index));
    }
};
VideoTexture::VideoTexture(const AssetStore& assets,std::string_view path,std::uint32_t initial):impl_(std::make_unique<Impl>(assets,path,initial)){}
VideoTexture::~VideoTexture()=default;
VideoFrame VideoTexture::frame()const noexcept{return impl_->movie.frame();}
void VideoTexture::update_clock(std::uint32_t now){impl_->refresh(now);}

struct Media::Impl {
    const AssetStore& assets;const Settings& settings;
    struct ChunkDeleter {void operator()(Mix_Chunk* p)const noexcept{Mix_FreeChunk(p);}};
    std::unordered_map<std::string,std::unique_ptr<Mix_Chunk,ChunkDeleter>,PathHash,PathEqual> chunks;
    std::unordered_map<std::string,std::shared_ptr<const std::vector<std::uint8_t>>,PathHash,PathEqual> streams;
    struct Channel {
        SoundId id{};std::uint32_t owner{};int category{3};float volume{1},minimum{400};Vec3 position{};bool spatial{},loop{};
        const std::string* path{};Mix_Chunk* original{};Mix_Chunk view{};
        std::unique_ptr<OggStream> stream;std::atomic<std::uint64_t> frames{0};std::uint64_t offset{},tail_frames{};bool tail{};
    };
    std::array<Channel,90> channels{};std::array<std::int16_t,2048> silence{};
    SoundId next_id{1};int environment{};Vec3 listener{},forward{0,0,-1},up{0,1,0},raw_forward{0,0,-1},raw_up{0,1,0};
    float applied_master{-1},applied_sfx{-1},applied_music{-1};
    bool game_paused{},pause_applied{};
    struct Variants {std::vector<std::string> paths;};
    std::unordered_map<std::string,Variants,PathHash,PathEqual> variations;std::string variation_key;
    void* random_context{};std::uint32_t (*random_word)(void*){};std::uint32_t random_state{1};
    std::uint32_t draw_random(){
        if(random_word)return random_word(random_context);
        random_state=random_state*214013u+2531011u;return (random_state>>16)&32767u;
    }
    bool audio_initialized{},mixer_initialized{},audio_open{};
    Impl(const AssetStore& a,const Settings& s):assets(a),settings(s){
        settings_check();
        try {
            if(SDL_InitSubSystem(SDL_INIT_AUDIO)!=0)throw std::runtime_error(SDL_GetError());audio_initialized=true;
            mixer_initialized=true;if((Mix_Init(MIX_INIT_OGG)&MIX_INIT_OGG)==0)throw std::runtime_error(Mix_GetError());
            if(Mix_OpenAudio(44100,AUDIO_S16SYS,2,1024)!=0)throw std::runtime_error(Mix_GetError());audio_open=true;
            int rate{},count{};Uint16 format{};if(!Mix_QuerySpec(&rate,&format,&count)||rate!=44100||format!=AUDIO_S16SYS||count!=2)throw std::runtime_error("Mixer did not open stereo S16 at 44100 Hz");
            if(Mix_AllocateChannels(90)!=90)throw std::runtime_error("Cannot allocate original 90 sound channels");
        }catch(...){cleanup();throw;}
    }
    ~Impl(){cleanup();}
    void cleanup()noexcept{
        if(audio_open){Mix_HaltChannel(-1);for(auto& c:channels)c.stream.reset();chunks.clear();Mix_CloseAudio();audio_open=false;}
        if(mixer_initialized){Mix_Quit();mixer_initialized=false;}if(audio_initialized){SDL_QuitSubSystem(SDL_INIT_AUDIO);audio_initialized=false;}
    }
    void settings_check()const{unit_check(settings.master_volume);unit_check(settings.sfx_volume);unit_check(settings.music_volume);}
    static void effect(int,void* bytes,int length,void* data)noexcept{
        auto& c=*static_cast<Channel*>(data);
        if(c.stream)c.stream->render(bytes,length);
        else{
            const auto count=static_cast<std::uint64_t>(length)/4;
            const auto total=c.frames.fetch_add(count,std::memory_order_relaxed)+count;
            // The mixer reads the looping chunk AFTER this effect; restore the
            // original view when its initial seek tail has just been consumed.
            if(c.tail&&total>=c.tail_frames){c.view.abuf=c.original->abuf;c.view.alen=c.original->alen;c.tail=false;}
        }
        // Original 3D source load mode 0x41000 includes FSOUND_FORCEMONO.
        if(c.spatial){auto* pcm=static_cast<std::int16_t*>(bytes);for(int i=0;i+1<length/2;i+=2){const auto mono=static_cast<std::int16_t>((static_cast<int>(pcm[i])+pcm[i+1])/2);pcm[i]=pcm[i+1]=mono;}}
    }
    auto chunk(std::string_view path){
        auto found=chunks.find(path);if(found!=chunks.end())return found;
        auto bytes=assets.bytes(path);if(bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("Sound is too large");
        auto* rw=SDL_RWFromConstMem(bytes.data(),static_cast<int>(bytes.size()));if(!rw)throw std::runtime_error(SDL_GetError());
        std::unique_ptr<Mix_Chunk,ChunkDeleter> loaded(Mix_LoadWAV_RW(rw,1));if(!loaded)throw std::runtime_error("Sound "+std::string(path)+": "+Mix_GetError());
        if(!loaded->alen||loaded->alen%4)throw std::runtime_error("Empty or unaligned sound PCM");
        return chunks.emplace(std::string(path),std::move(loaded)).first;
    }
    auto stream_data(std::string_view path){
        auto found=streams.find(path);if(found!=streams.end())return found;
        return streams.emplace(std::string(path),std::make_shared<const std::vector<std::uint8_t>>(assets.bytes(path))).first;
    }
    double seconds(const Channel& c)const noexcept{
        if(c.stream)return c.stream->seconds();if(!c.original)return 0;
        const auto total=static_cast<std::uint64_t>(c.original->alen)/4;const auto played=c.offset+c.frames.load(std::memory_order_relaxed);
        return static_cast<double>(c.loop?played%total:std::min(played,total))/44100;
    }
    double duration(const Channel& c)const noexcept{return c.stream?c.stream->duration():static_cast<double>(c.original->alen)/4/44100;}
    bool playing(int index)const noexcept{const auto& c=channels[index];return c.id&&Mix_Playing(index)&&(!c.stream||!c.stream->finished());}
    int choose(int category,std::uint32_t owner)const{
        const auto range=pool(category,owner);
        if(owner)for(int i=range.first;i<=range.second;++i)if(channels[i].owner==owner)return i;
        int selected=range.first;double fraction=-1;
        for(int i=range.first;i<=range.second;++i){if(!playing(i))return i;const auto score=seconds(channels[i])/duration(channels[i]);if(score>fraction){fraction=score;selected=i;}}
        return selected;
    }
    void stop_channel(int index){Mix_HaltChannel(index);auto& c=channels[index];c.id=0;c.stream.reset();c.path=nullptr;c.original=nullptr;}
    void spatial(int index){
        auto& c=channels[index];float attenuation=1;int angle=0;
        if(c.spatial){const double x=static_cast<double>(c.position.x)-listener.x,y=static_cast<double>(c.position.y)-listener.y,z=static_cast<double>(c.position.z)-listener.z;const double d=std::sqrt(x*x+y*y+z*z);
            // Recovered sample/stream virtuals set min_distance and max=1e9.
            // FMOD default inverse rolloff uses their ratio; distance factor40
            // scales both distances and therefore cancels for static sources.
            attenuation=static_cast<float>(c.minimum/std::max<double>(c.minimum,std::min(d,1e9)));
            const auto right=normalized(cross(forward,up));const double a=std::atan2(x*right.x+y*right.y+z*right.z,x*forward.x+y*forward.y+z*forward.z)*180/std::numbers::pi;angle=(static_cast<int>(a)+360)%360;
        }
        const auto effective=category_for_channel(index);const float category_gain=effective==0||effective==4?settings.music_volume:settings.sfx_volume;
        Mix_Volume(index,static_cast<int>(MIX_MAX_VOLUME*c.volume*settings.master_volume*category_gain*attenuation));
        if(!Mix_SetPosition(index,static_cast<Sint16>(angle),0))throw std::runtime_error(Mix_GetError());
    }
    void start(int index,SoundState& state,const std::string* path,Mix_Chunk* original,std::unique_ptr<OggStream> stream){
        settings_check();stop_channel(index);
        auto& c=channels[index];c.id=state.id;c.owner=state.owner;c.category=state.category;c.volume=state.volume;c.minimum=state.min_distance;c.position=state.position;c.spatial=state.spatial;c.loop=state.loop;c.original=original;c.path=path;c.stream=std::move(stream);c.frames.store(0);c.offset=0;c.tail=false;
        if(c.stream)c.view={0,reinterpret_cast<Uint8*>(silence.data()),static_cast<Uint32>(sizeof(silence)),MIX_MAX_VOLUME};
        else{c.offset=static_cast<std::uint64_t>(std::llround(state.seconds*44100));c.view=*original;c.view.allocated=0;c.view.abuf=original->abuf+c.offset*4;c.view.alen=original->alen-static_cast<Uint32>(c.offset*4);c.tail=state.loop&&c.offset!=0;c.tail_frames=c.view.alen/4;}
        try {
            if(!Mix_RegisterEffect(index,effect,nullptr,&c))throw std::runtime_error(Mix_GetError());
            spatial(index);
            if(Mix_PlayChannel(index,&c.view,c.stream||state.loop?-1:0)<0)throw std::runtime_error(Mix_GetError());
        }catch(...){Mix_UnregisterAllEffects(index);stop_channel(index);throw;}
    }
    struct PreparedSound {SoundState state;int channel{-1};const std::string* path{};Mix_Chunk* chunk{};std::unique_ptr<OggStream> stream;};
    struct Prepared {std::vector<PreparedSound> sounds;};
    Prepared prepare(const MediaSnapshot& state){
        settings_check();environment_check(state.environment);
        if(state.sounds.size()>90)throw std::runtime_error("Save has more than 90 sound channels");
        Prepared result;result.sounds.reserve(state.sounds.size());
        for(const auto& sound:state.sounds){sound_check(sound);const auto range=pool(sound.category,sound.owner);
            for(const auto& earlier:result.sounds){if(earlier.state.id==sound.id)throw std::runtime_error("Duplicate saved sound identity");const auto other=pool(earlier.state.category,earlier.state.owner);if(sound.owner&&earlier.state.owner==sound.owner&&range.first<=other.second&&other.first<=range.second)throw std::runtime_error("Duplicate saved sound owner in overlapping categories");if(sound.stream&&earlier.state.stream&&PathEqual{}(sound.path,earlier.state.path))throw std::runtime_error("Save duplicates a non-overlapping streaming source");}
            PreparedSound prepared;prepared.state=sound;
            if(sound.stream){const auto loaded=stream_data(sound.path);prepared.path=&loaded->first;prepared.stream=std::make_unique<OggStream>(loaded->second,sound.loop);if(sound.seconds>=prepared.stream->duration())throw std::runtime_error("Saved stream seek is beyond audio");prepared.stream->seek(sound.seconds);}
            else{const auto loaded=chunk(sound.path);prepared.path=&loaded->first;prepared.chunk=loaded->second.get();if(sound.seconds>=static_cast<double>(prepared.chunk->alen)/4/44100||std::llround(sound.seconds*44100)>=prepared.chunk->alen/4)throw std::runtime_error("Saved sound seek is beyond PCM samples");}
            result.sounds.push_back(std::move(prepared));
        }
        std::array<bool,90> used{};
        for(int pass=0;pass<2;++pass)for(auto& sound:result.sounds){const auto range=pool(sound.state.category,sound.state.owner);if((range.first==range.second)!=(pass==0))continue;for(int i=range.first;i<=range.second;++i)if(!used[i]){used[i]=true;sound.channel=i;break;}if(sound.channel<0)throw std::runtime_error("Save overflows original channel pools");}
        return result;
    }
};
Media::Media(const AssetStore& assets,const Settings& settings):impl_(std::make_unique<Impl>(assets,settings)){apply_settings();}
Media::~Media()=default;
Media::SoundId Media::sound(std::string_view path,float volume,bool loop,Vec3 position,bool spatial,int category,bool stream,std::uint32_t owner){
    auto& s=*impl_;volume=gain(volume);vector_check(position);pool(category,owner);s.settings_check();
    if(path.empty()||(!s.chunks.contains(path)&&!s.streams.contains(path)&&!s.assets.contains(path)))return 0;
    if(s.next_id==std::numeric_limits<SoundId>::max())throw std::runtime_error("Sound identity exhausted");
    SoundState state;state.id=s.next_id;state.volume=volume;state.loop=loop;state.position=position;state.spatial=spatial;state.category=category;state.owner=owner;state.stream=stream;
    std::unique_ptr<OggStream> streamed;const std::string* key{};Mix_Chunk* loaded{};
    if(stream){const auto found=s.stream_data(path);key=&found->first;streamed=std::make_unique<OggStream>(found->second,loop);}else{auto found=s.chunk(path);key=&found->first;loaded=found->second.get();}
    if(stream)for(int i=0;i<90;++i)if(s.channels[i].id&&s.channels[i].stream&&PathEqual{}(*s.channels[i].path,path))s.stop_channel(i);
    const int index=s.choose(category,owner);s.start(index,state,key,loaded,std::move(streamed));++s.next_id;return state.id;
}
void Media::stop(SoundId id){if(!id)return;for(int i=0;i<90;++i)if(impl_->channels[i].id==id)impl_->stop_channel(i);}
void Media::clear_game_sounds(){
    for(int i=0;i<90;++i)if(i!=1&&impl_->channels[i].id)impl_->stop_channel(i);
}
void Media::volume(SoundId id,float value){value=gain(value);impl_->settings_check();for(int i=0;i<90;++i)if(id&&impl_->channels[i].id==id&&impl_->channels[i].volume!=value){impl_->channels[i].volume=value;impl_->spatial(i);}}
void Media::position(SoundId id,Vec3 value){vector_check(value);for(int i=0;i<90;++i)if(id&&impl_->channels[i].id==id&&!same(impl_->channels[i].position,value)){impl_->channels[i].position=value;impl_->spatial(i);}}
void Media::min_distance(SoundId id,float value){minimum_check(value);for(int i=0;i<90;++i)if(id&&impl_->channels[i].id==id&&impl_->channels[i].minimum!=value){impl_->channels[i].minimum=value;impl_->spatial(i);}}
void Media::environment(int type){environment_check(type);impl_->environment=type;}
void Media::set_random_source(void* context,std::uint32_t (*word)(void*)){if(!word)throw std::runtime_error("Null media random source");impl_->random_context=context;impl_->random_word=word;}
Media::SoundId Media::environment_sound(std::string_view category,std::string_view name,std::string_view object,float volume,Vec3 position,bool spatial,bool apply_environment,std::uint32_t owner,std::uint32_t* previous){
    auto& s=*impl_;volume=gain(volume);vector_check(position);pool(spatial?2:3,owner);s.settings_check();
    auto& key=s.variation_key;key.clear();key="sound/";append_component(key,category);if(apply_environment)append_component(key,environment_names[s.environment]);append_component(key,object);
    const auto directory_length=key.size();key.append(name);auto found=s.variations.find(key);
    if(found==s.variations.end()){
        Impl::Variants variants;const std::string directory(key.data(),directory_length);
        for(auto& path:s.assets.names(directory)){const auto slash=path.find_last_of("/\\");if(slash==std::string::npos||!PathEqual{}(std::string_view(path).substr(0,slash+1),directory))continue;const auto filename=std::string_view(path).substr(slash+1);if(filename.size()<name.size()+4||!PathEqual{}(filename.substr(0,name.size()),name)||!PathEqual{}(filename.substr(filename.size()-4),".ogg"))continue;variants.paths.push_back(std::move(path));}
        found=s.variations.emplace(key,std::move(variants)).first;
    }
    const auto& paths=found->second.paths;if(paths.empty())return 0;std::size_t choice=s.draw_random()%paths.size();
    if(previous&&paths.size()>1)while(choice==*previous%paths.size())choice=s.draw_random()%paths.size();
    if(previous)*previous=static_cast<std::uint32_t>(choice);
    return sound(paths[choice],volume,false,position,spatial,spatial?2:3,false,owner);
}
void Media::music(std::string_view path,bool loop){if(path.empty()){impl_->stop_channel(0);return;}sound(path,1,loop,{},false,0,true,0);}
void Media::stop_music(){impl_->stop_channel(0);impl_->stop_channel(1);}
int Media::channel_index(SoundId id)const noexcept{for(int i=0;i<90;++i)if(id&&impl_->channels[i].id==id&&impl_->playing(i))return i;return -1;}
void Media::preload_sound(std::string_view path,bool stream){if(stream)return;if(impl_->assets.contains(path))impl_->chunk(path);}
void Media::pause_game(bool paused){auto& s=*impl_;if(s.pause_applied&&s.game_paused==paused)return;s.pause_applied=true;s.game_paused=paused;for(int i=0;i<90;++i)if(s.channels[i].id){if((i==1)!=paused)Mix_Pause(i);else Mix_Resume(i);}}
void Media::listener(Vec3 position,Vec3 forward,Vec3 up){vector_check(position);vector_check(forward);vector_check(up);auto& s=*impl_;if(same(position,s.listener)&&same(forward,s.raw_forward)&&same(up,s.raw_up))return;const auto f=normalized(forward),u=normalized(up);if(dot(cross(f,u),cross(f,u))<1e-8f)throw std::runtime_error("Parallel sound listener basis");s.listener=position;s.raw_forward=forward;s.raw_up=up;s.forward=f;s.up=u;for(int i=0;i<90;++i)if(s.channels[i].id&&s.channels[i].spatial)s.spatial(i);}
void Media::apply_settings(){auto& s=*impl_;s.settings_check();const auto& v=s.settings;if(s.applied_master==v.master_volume&&s.applied_sfx==v.sfx_volume&&s.applied_music==v.music_volume)return;
    for(int i=0;i<90;++i)if(s.channels[i].id)s.spatial(i);s.applied_master=v.master_volume;s.applied_sfx=v.sfx_volume;s.applied_music=v.music_volume;
}
void Media::update(float seconds){delta_check(seconds);auto& s=*impl_;apply_settings();for(int i=0;i<90;++i){auto& c=s.channels[i];if(!c.id)continue;if(c.stream)c.stream->update();if(!s.playing(i))s.stop_channel(i);}
}
MediaSnapshot Media::snapshot()const{const auto& s=*impl_;MediaSnapshot state;state.environment=s.environment;state.sounds.reserve(90);
    for(int i=0;i<90;++i){const auto& c=s.channels[i];if(!s.playing(i))continue;SoundState sound;sound.id=c.id;sound.path=*c.path;sound.volume=c.volume;sound.loop=c.loop;sound.spatial=c.spatial;sound.stream=static_cast<bool>(c.stream);sound.position=c.position;sound.category=c.category;sound.min_distance=c.minimum;sound.seconds=s.seconds(c);sound.owner=c.owner;const auto duration=s.duration(c);if(sound.seconds>=duration){if(!sound.loop)continue;sound.seconds=std::fmod(sound.seconds,duration);}state.sounds.push_back(std::move(sound));}
    return state;
}
void Media::validate_state(const MediaSnapshot& state)const{auto prepared=impl_->prepare(state);(void)prepared;}
void Media::restore_state(const MediaSnapshot& state){auto& s=*impl_;auto prepared=s.prepare(state);
    stop_music();for(int i=0;i<90;++i){s.stop_channel(i);s.channels[i].owner=0;}s.environment=state.environment;
    for(auto& sound:prepared.sounds){s.start(sound.channel,sound.state,sound.path,sound.chunk,std::move(sound.stream));s.next_id=std::max(s.next_id,sound.state.id+1);}
    if(s.pause_applied)for(int i=0;i<90;++i)if(s.channels[i].id){if((i==1)!=s.game_paused)Mix_Pause(i);else Mix_Resume(i);}
    apply_settings();
}

} // namespace pusu
