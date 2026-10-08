#include "game.hpp"
#include "resources.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pusu {
namespace {
bool audio_name_equal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        return x == y;
    });
}
bool audio_printable(unsigned char c) {
    //00463c60's CRT-printable test plus its original Turkish-byte whitelist.
    return (c >= 32 && c <= 126) ||
        std::string_view("\xfd\xdd\xdc\xfc\xde\xfe\xd0\xf0\xc7\xe7\xd6\xf6").find(static_cast<char>(c)) != std::string_view::npos;
}
void audio_words(std::string_view line, std::vector<std::string_view>& words) {
    words.clear();
    while (!line.empty()) {
        while (!line.empty() && (line.front() == ' ' || !audio_printable(static_cast<unsigned char>(line.front()))))
            line.remove_prefix(1);
        if (line.empty()) break;
        bool quoted = false, had_quote = false;
        std::size_t length{};
        for (; length < line.size(); ++length) {
            const auto c = static_cast<unsigned char>(line[length]);
            if (!audio_printable(c) || (c == ' ' && !quoted)) break;
            if (c == '"') { quoted = !quoted; had_quote = true; }
        }
        auto word = line.substr(0, length);
        line.remove_prefix(length);
        if (had_quote && !quoted && word.size() >= 2) word = word.substr(1, word.size() - 2);
        //Comment prefixes are tested after stripping quotes, not within filenames.
        if (word.starts_with("//")) break;
        words.push_back(word);
    }
}
float audio_scalar(std::string_view word) {
    while (!word.empty() && std::string_view(" \t\r\n\v\f").find(word.front()) != std::string_view::npos)
        word.remove_prefix(1);
    if (word.starts_with('+')) word.remove_prefix(1);
    float value{};
    const auto parsed = std::from_chars(word.data(), word.data() + word.size(), value);
    if (parsed.ec != std::errc{} || !std::isfinite(value))
        throw std::runtime_error("Invalid original sound scalar: " + std::string(word));
    return value;
}
std::uint8_t audio_byte(std::string_view word) {
    while (!word.empty() && std::string_view(" \t\r\n\v\f").find(word.front()) != std::string_view::npos)
        word.remove_prefix(1);
    bool negative = false;
    if (!word.empty() && (word.front() == '-' || word.front() == '+')) {
        negative = word.front() == '-';
        word.remove_prefix(1);
    }
    // Original00468337/004682df: decimal atoi, suffix ignored, low byte retained.
    std::uint32_t value{};
    for (const char c : word) {
        if (c < '0' || c > '9') break;
        value = value * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return static_cast<std::uint8_t>(negative ? 0u - value : value);
}
void source_channels(Media& media, std::vector<Media::SoundId>& channels, bool stream) {
    if (stream) {
        //0043f2f0 closes the source's previous stream before reopening it.
        for (const auto channel : channels) media.stop(channel);
        channels.clear();
    } else {
        std::erase_if(channels, [&](auto channel) { return media.channel_index(channel) < 0; });
    }
}
}

void Game::create_sound_sources(std::string_view name) {
    //00429680 ->00429280: the argument is a stem, never a general asset path.
    const auto path = "level/sound/" + std::string(name) + ".txt";
    sound_set_ = name; //00429680 stores the requested stem before attempting the open.
    if (!assets_.contains(path)) return; //00429280's original null-file branch; not a substitute source.
    const auto text = assets_.text(path);
    std::string_view remaining(text);
    std::vector<std::string_view> words;
    words.reserve(4);
    std::optional<SoundSource> source;
    unsigned depth{};
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        auto line = remaining.substr(0, newline);
        remaining = newline == remaining.npos ? std::string_view{} : remaining.substr(newline + 1);
        audio_words(line, words);
        if (words.empty()) continue;
        const auto directive = words[0];
        if (directive.starts_with('{')) {
            if (!source) throw std::runtime_error("Original sound block has no name");
            if (depth == std::numeric_limits<unsigned>::max()) throw std::runtime_error("Sound block nesting overflow");
            ++depth;
        } else if (directive.starts_with('}')) {
            if (!source || !depth) throw std::runtime_error("Unmatched original sound block close");
            if (--depth == 0) {
                //00437500 prefixes sound\\ verbatim; streamed0043ec50 stores only its path.
                source->path = "sound/" + source->path;
                if (!source->spatial) {
                    //00429280 applies the authored position/distance setters only to 3D sources.
                    source->position = {};
                    source->min_distance = 400;
                }
                if (!source->stream) media_.preload_sound(source->path);
                sounds_.push_back(std::move(*source));
                source.reset();
            }
        } else if (audio_name_equal(directive, "2d") || audio_name_equal(directive, "path") ||
                   audio_name_equal(directive, "pos") || audio_name_equal(directive, "streaming") ||
                   audio_name_equal(directive, "no_loop") || audio_name_equal(directive, "min_distance") ||
                   audio_name_equal(directive, "volume")) {
            if (!source) throw std::runtime_error("Original sound property has no definition");
            const auto require = [&](std::size_t count) {
                if (words.size() < count) throw std::runtime_error("Missing original sound property value: " + std::string(directive));
            };
            if (audio_name_equal(directive, "2d")) source->spatial = false;
            else if (audio_name_equal(directive, "streaming")) source->stream = true;
            else if (audio_name_equal(directive, "no_loop")) source->loop = false;
            else if (audio_name_equal(directive, "path")) { require(2); source->path = words[1]; }
            else if (audio_name_equal(directive, "pos")) {
                require(4);
                source->position = {audio_scalar(words[1]), audio_scalar(words[2]), audio_scalar(words[3])};
            } else if (audio_name_equal(directive, "min_distance")) {
                require(2); source->min_distance = audio_scalar(words[1]);
            } else {
                require(2);
                source->volume = audio_name_equal(words[1], "min") ? 0 :
                    audio_name_equal(words[1], "max") ? 255 : audio_byte(words[1]);
            }
        } else if (depth == 0) {
            //00429280 starts a record only outside braces; unknown inner directives are ignored.
            source.emplace();
            source->name = directive;
        }
    }
    if (source || depth) throw std::runtime_error("Unterminated original sound definition");
}

void Game::host_sound(std::string_view file, bool loop, bool stream, int category) {
    //00437a90 caches by filename+_2D, loop/storage/nonspatial identity; newest match wins.
    const auto name = std::string(file) + "_2D";
    auto source = std::find_if(sounds_.rbegin(), sounds_.rend(), [&](const SoundSource& value) {
        return audio_name_equal(value.name, name) && value.loop == loop &&
               value.stream == stream && !value.spatial;
    });
    if (source == sounds_.rend()) {
        SoundSource value;
        value.name = name;
        value.path = "sound/" + std::string(file);
        value.loop = loop; value.stream = stream; value.spatial = false;
        if (!stream) media_.preload_sound(value.path);
        sounds_.push_back(std::move(value));
        source = sounds_.rbegin();
    }
    source_channels(media_, source->channels, source->stream);
    source->channels.push_back(media_.sound(source->path, source->volume / 255.0f,
        source->loop, source->position, false, category, source->stream));
}

void Game::host_named_sound(std::string_view name, int category) {
    //00446c20 inserts at the hash-list head;00437c00 plays every matching definition.
    //Missing names deliberately return zero in the original; no filename fallback is performed.
    for (auto source = sounds_.rbegin(); source != sounds_.rend(); ++source) {
        if (!audio_name_equal(source->name, name)) continue;
        source_channels(media_, source->channels, source->stream);
        const auto channel = media_.sound(source->path, source->volume / 255.0f,
            source->loop, source->position, source->spatial, category, source->stream);
        media_.min_distance(channel, source->min_distance);
        source->channels.push_back(channel);
    }
}
void Game::host_stop_sound(std::string_view name) {
    //00437d40 ->00437090: all duplicate definitions and all their physical channels.
    for (auto source = sounds_.rbegin(); source != sounds_.rend(); ++source) {
        if (!audio_name_equal(source->name, name)) continue;
        for (const auto channel : source->channels) media_.stop(channel);
        source->channels.clear();
    }
}
void Game::host_sound_volume(std::string_view name, std::uint32_t volume) {
    //00437c70 stores a BYTE, then updates only the lowest physical channel for each source.
    const auto byte = static_cast<std::uint8_t>(volume);
    for (auto source = sounds_.rbegin(); source != sounds_.rend(); ++source) {
        if (!audio_name_equal(source->name, name)) continue;
        source->volume = byte;
        int first = std::numeric_limits<int>::max();
        Media::SoundId selected{};
        for (const auto channel : source->channels) {
            const int index = media_.channel_index(channel);
            if (index >= 0 && index < first) { first = index; selected = channel; }
        }
        if (selected) media_.volume(selected, byte / 255.0f);
    }
}
void Game::host_music(std::string_view file) {
    //00426250 ->00437a90 ->00437500, not an inferred sound/music subdirectory.
    host_sound(file, true, true, 0);
    music_ = file;
}
void Game::host_stop_music() {
    media_.stop_music();
    music_.clear();
}
void game_audio_check() {
    std::vector<std::string_view> words;
    audio_words("\tpath \"effect/indoor_small/DIS M(2).ogg\" // trailing comment\r", words);
    assert(words.size() == 2 && words[0] == "path" && words[1] == "effect/indoor_small/DIS M(2).ogg");
    audio_words("path effect//name.ogg \"// quoted comment\"", words);
    assert(words.size() == 2 && words[1] == "effect//name.ogg");
    assert(audio_scalar(" +1520.25suffix") == 1520.25f);
    assert(audio_byte(" -129suffix") == 127 && audio_byte("511") == 255);
    assert(audio_name_equal("Door_Open", "door_open") && !audio_name_equal("door", "door_open"));
}
} // namespace pusu
