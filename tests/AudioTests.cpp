#include "engine/audio/MiniAudio.h"
#include "engine/audio/PitchShift.h"
#include "engine/core/Pack.h"
#include "engine/core/Paths.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace fe;

namespace {
// Test files live in a folder of this process's own, removed at exit.
struct TempFolder {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("fengine-audio-test-" + std::to_string(std::random_device{}()));
    TempFolder() { std::filesystem::create_directories(path); }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// A 16-bit mono sine wave.
std::filesystem::path tone(const char* name, double seconds = 1, double hertz = 382) {
    static const TempFolder folder;
    const auto file = folder.path / (std::string(name) + ".wav");
    constexpr std::uint32_t rate = 48000;
    const auto frames = static_cast<std::uint32_t>(seconds * rate);
    std::ofstream out(file, std::ios::binary);
    auto u32 = [&](std::uint32_t v) {
        out.write(reinterpret_cast<const char*>(&v), 4);
    };
    auto u16 = [&](std::uint16_t v) {
        out.write(reinterpret_cast<const char*>(&v), 2);
    };
    out.write("RIFF", 4);
    u32(36 + frames * 2);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double phase = 2 * 3.14159265358979 * hertz * i / rate;
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::sin(phase) * 12000)));
    }
    return file;
}

std::filesystem::path toneFile() {
    return tone("tone");
}

// Mixes `seconds` of audio in 10 ms ticks, letting the loader thread finish
// asynchronous loads; returns the interleaved stereo output.
std::vector<float> play(MiniAudio& audio, double seconds) {
    std::vector<float> output;
    const auto chunks = static_cast<int>(std::lround(seconds * 100));
    for (int i = 0; i < chunks; ++i) {
        audio.mix(MiniAudio::SampleRate / 100, &output);
        audio.update(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return output;
}

double rms(const std::vector<float>& samples, int channel) {
    double sum = 0;
    std::size_t count = 0;
    for (std::size_t i = static_cast<std::size_t>(channel); i < samples.size(); i += 2) {
        sum += double(samples[i]) * samples[i];
        ++count;
    }
    return count ? std::sqrt(sum / static_cast<double>(count)) : 0;
}

void mixUntil(MiniAudio& audio, const std::function<bool()>& done, double limit = 2) {
    for (double t = 0; t < limit && !done(); t += 0.01) {
        play(audio, 0.01);
    }
}

// The frequency (Hz) with the largest DFT magnitude in the left channel of
// interleaved stereo, over `frames` from `start`, scanning `from`..`to` in
// 4 Hz steps.
double peakFrequency(const std::vector<float>& stereo, std::size_t start, std::size_t frames,
                     double from = 100, double to = 2000) {
    double bestHertz = 0, best = -1;
    for (double hertz = from; hertz <= to; hertz += 4) {
        double re = 0, im = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const double phase = 2 * 3.14159265358979 * hertz * static_cast<double>(i) / 48000.0;
            const double x = stereo[(start + i) * 2];
            re += x * std::cos(phase);
            im -= x * std::sin(phase);
        }
        const double magnitude = re * re + im * im;
        if (magnitude > best) {
            best = magnitude;
            bestHertz = hertz;
        }
    }
    return bestHertz;
}
} // namespace

TEST_CASE("music plays at its volume, not the volume squared") {
    MiniAudio audio(false);
    REQUIRE(audio.available());
    MusicOptions options;
    options.volume = 0.5f;
    options.fade = 0.1;
    audio.playMusic(toneFile(), options);
    play(audio, 0.3);
    CHECK(audio.musicGain("main") == doctest::Approx(0.5f).epsilon(0.01));
    audio.setMusicVolume("main", 0.25f, 0);
    play(audio, 0.05);
    CHECK(audio.musicGain("main") == doctest::Approx(0.25f).epsilon(0.01));
}

TEST_CASE("changing the volume of a fading-out track keeps the fade-out") {
    MiniAudio audio(false);
    audio.playMusic(toneFile(), MusicOptions{});
    play(audio, 0.1);
    audio.stopMusic("main", 0.2);
    audio.setMusicVolume("main", 1, 0);
    CHECK(audio.musicGain("main") == doctest::Approx(0));
    play(audio, 0.4);
    CHECK(audio.activeSounds() == 0);
    CHECK(audio.currentMusic("main").empty());
}

TEST_CASE("stopped looping voices are released on the mixer clock, without game ticks") {
    MiniAudio audio(false);
    SoundOptions options;
    options.loop = true;
    const int voice = audio.playSound(toneFile(), options);
    const int other = audio.playSound(toneFile(), options);
    play(audio, 0.1);
    CHECK(audio.activeSounds() == 2);
    audio.stopSound(voice, 0.25);
    audio.stopSound(other, 0);
    audio.update(0);
    CHECK(audio.activeSounds() == 1);
    audio.update(10); // a long game tick does not end the fade early
    CHECK(audio.activeSounds() == 1);
    play(audio, 0.4);
    CHECK(audio.activeSounds() == 0);
}

TEST_CASE("bus volumes range 0 to 4 like music and voice volumes") {
    MiniAudio audio(false);
    audio.setBusVolume("sfx", 2.5f);
    CHECK(audio.busVolume("sfx") == doctest::Approx(2.5f));
    audio.setBusVolume("sfx", 9.0f);
    CHECK(audio.busVolume("sfx") == doctest::Approx(4.0f));
    audio.setBusVolume("sfx", -1.0f);
    CHECK(audio.busVolume("sfx") == doctest::Approx(0.0f));
    audio.setBusVolume("sfx", 1.0f);
    // The group carries the gain: a doubled bus doubles the output.
    audio.playSound(toneFile(), SoundOptions{});
    const double unity = rms(play(audio, 0.3), 0);
    audio.setBusVolume("sfx", 2.0f);
    const double doubled = rms(play(audio, 0.3), 0);
    CHECK(doubled == doctest::Approx(unity * 2).epsilon(0.1));
}

TEST_CASE("the voice cap steals the oldest non-looping voice on the lowest-priority bus") {
    MiniAudio audio(false);
    CHECK(audio.maxVoices() == 64);
    audio.setMaxVoices(3);
    SoundOptions sfx, speech, loop;
    speech.bus = "voice";
    loop.loop = true;
    const int firstSfx = audio.playSound(toneFile(), sfx);
    const int line = audio.playSound(toneFile(), speech);
    const int secondSfx = audio.playSound(toneFile(), sfx);
    play(audio, 0.05);
    CHECK(audio.activeSounds() == 3);
    // Full: the oldest sfx goes, not the older voice line.
    const int third = audio.playSound(toneFile(), sfx);
    CHECK(audio.activeSounds() == 3);
    CHECK_FALSE(audio.playing(firstSfx));
    CHECK(audio.playing(line));
    CHECK(audio.playing(secondSfx));
    CHECK(audio.playing(third));
    // Looping voices are kept while a non-looping one exists, even on a higher bus.
    audio.stopSound(secondSfx, 0);
    audio.stopSound(third, 0);
    audio.update(0);
    const int ambience = audio.playSound(toneFile(), loop);
    const int otherLoop = audio.playSound(toneFile(), loop);
    CHECK(audio.activeSounds() == 3);
    const int fourth = audio.playSound(toneFile(), sfx);
    CHECK_FALSE(audio.playing(line));
    CHECK(audio.playing(ambience));
    CHECK(audio.playing(otherLoop));
    CHECK(audio.playing(fourth));
    // A voice already fading out is the first to go.
    audio.stopSound(otherLoop, 1.0);
    audio.playSound(toneFile(), sfx);
    CHECK(audio.playing(ambience));
    CHECK(audio.playing(fourth));
    CHECK(audio.activeSounds() == 3);
    // Lowering the cap drops voices at once.
    audio.setMaxVoices(1);
    CHECK(audio.activeSounds() == 1);
    CHECK(audio.playing(ambience)); // the one looping voice outlives the one-shots
}

TEST_CASE("one-shots end by themselves; missing files throw") {
    MiniAudio audio(false);
    audio.playSound(toneFile(), SoundOptions{});
    play(audio, 1.3);
    CHECK(audio.activeSounds() == 0);
    CHECK_THROWS(audio.playSound("missing.wav", SoundOptions{}));
}

TEST_CASE("Ogg Vorbis files decode like the other formats") {
    MiniAudio audio(false);
    const auto file = std::filesystem::path(FENGINE_TEST_GAME) / "audio" / "tone.ogg";
    CHECK(audio.length(file) == doctest::Approx(0.5).epsilon(0.05));
    const int voice = audio.playSound(file, SoundOptions{});
    CHECK(audio.playing(voice));
    play(audio, 0.8);
    CHECK(audio.activeSounds() == 0); // it played to its end
}

TEST_CASE("voices report whether they play and where they are") {
    MiniAudio audio(false);
    CHECK(audio.length(toneFile()) == doctest::Approx(1.0));
    CHECK_THROWS(static_cast<void>(audio.length("missing.wav")));
    const int voice = audio.playSound(toneFile(), SoundOptions{});
    CHECK(audio.playing(voice));
    mixUntil(audio, [&] { return audio.position(voice) > 0; });
    const double start = audio.position(voice);
    play(audio, 0.3);
    CHECK(audio.playing(voice));
    CHECK(audio.position(voice) - start == doctest::Approx(0.3).epsilon(0.05));
    play(audio, 1);
    CHECK_FALSE(audio.playing(voice));

    SoundOptions looping;
    looping.loop = true;
    const int loop = audio.playSound(toneFile(), looping);
    play(audio, 0.1);
    CHECK(audio.playing(loop));
    audio.stopSound(loop, 0.5);
    CHECK_FALSE(audio.playing(loop)); // stopping counts at once, while the fade-out continues
    CHECK_FALSE(audio.playing(12345));
}

TEST_CASE("stems cued together start on the same frame and follow seeks of main") {
    MiniAudio audio(false);
    const auto file = tone("long", 3);
    audio.playMusic(file, MusicOptions{});
    MusicOptions stem;
    stem.layer = "drums";
    stem.sync = true;
    audio.playMusic(file, stem);
    mixUntil(audio, [&] { return audio.musicPosition("main") > 0.2; });
    CHECK(audio.musicPosition("drums") == doctest::Approx(audio.musicPosition("main")).epsilon(1e-6));

    audio.seekMusic("main", 2.0);
    CHECK(audio.musicPosition("main") == doctest::Approx(2.0));
    mixUntil(audio, [&] { return audio.musicPosition("main") > 2.1; });
    CHECK(audio.musicPosition("main") < 2.2);
    CHECK(audio.musicPosition("drums") == doctest::Approx(audio.musicPosition("main")).epsilon(1e-6));
    CHECK(audio.activeSounds() == 2); // the replaced streams were released

    audio.seekMusic("main", 7.5); // past the end of a looping 3 s track
    mixUntil(audio, [&] { return audio.musicPosition("main") < 2; });
    CHECK(audio.musicPosition("main") == doctest::Approx(1.5).epsilon(0.1));

    MusicOptions once;
    once.layer = "jingle";
    once.loop = false;
    audio.playMusic(file, once);
    audio.seekMusic("jingle", 10);
    mixUntil(audio, [&] { return audio.activeSounds() == 3; });
    CHECK(audio.currentMusic("jingle") == file.string());
    play(audio, 0.2);
    CHECK(audio.activeSounds() == 2); // a track sought past its end finishes
}

TEST_CASE("a synced stem joining playing music starts at main's position") {
    MiniAudio audio(false);
    const auto file = tone("long", 3);
    audio.playMusic(file, MusicOptions{});
    mixUntil(audio, [&] { return audio.musicPosition("main") > 0.4; });
    MusicOptions stem;
    stem.layer = "strings";
    stem.sync = true;
    audio.playMusic(file, stem);
    mixUntil(audio, [&] { return audio.musicPosition("strings") > 0; });
    play(audio, 0.1);
    CHECK(audio.musicPosition("strings") > 0.5);
    const double offset = audio.musicPosition("strings") - audio.musicPosition("main");
    CHECK(std::abs(offset) < 2.0 / MiniAudio::SampleRate);
}

TEST_CASE("pitch changes how fast music and voices play") {
    MiniAudio audio(false);
    const auto file = tone("long", 3);
    audio.playMusic(file, MusicOptions{});
    mixUntil(audio, [&] { return audio.musicPosition("main") > 0.1; });
    double before = audio.musicPosition("main");
    audio.setMusicPitch("main", 2);
    play(audio, 0.3);
    CHECK(audio.musicPosition("main") - before == doctest::Approx(0.6).epsilon(0.05));

    SoundOptions looping;
    looping.loop = true;
    const int voice = audio.playSound(file, looping);
    mixUntil(audio, [&] { return audio.position(voice) > 0; });
    audio.setSound(voice, {.pitch = 0.5f});
    before = audio.position(voice);
    play(audio, 0.4);
    CHECK(audio.position(voice) - before == doctest::Approx(0.2).epsilon(0.05));
}

TEST_CASE("pausing holds what is playing while new sounds still play") {
    MiniAudio audio(false);
    const auto file = tone("long", 3);
    audio.playMusic(file, MusicOptions{});
    SoundOptions looping;
    looping.loop = true;
    const int held = audio.playSound(file, looping);
    mixUntil(audio, [&] { return audio.musicPosition("main") > 0.1 && audio.position(held) > 0.1; });
    audio.pauseSounds(true);
    CHECK(audio.soundsPaused());
    const double music = audio.musicPosition("main"), voice = audio.position(held);
    const int menu = audio.playSound(file, SoundOptions{});
    play(audio, 0.3);
    CHECK(audio.musicPosition("main") == doctest::Approx(music));
    CHECK(audio.position(held) == doctest::Approx(voice));
    CHECK(audio.playing(held));
    CHECK(audio.position(menu) > 0.2);
    audio.pauseSounds(false);
    play(audio, 0.2);
    CHECK(audio.musicPosition("main") - music == doctest::Approx(0.2).epsilon(0.1));
    CHECK(audio.position(held) - voice == doctest::Approx(0.2).epsilon(0.1));
}

TEST_CASE("a bus low-pass removes high frequencies and keeps low ones") {
    BusEffect muffled;
    muffled.lowpass = 1000;
    auto level = [&](double hertz, bool filtered) {
        MiniAudio audio(false);
        SoundOptions looping;
        looping.loop = true;
        looping.bus = "music";
        audio.playSound(tone(hertz > 1000 ? "high" : "low", 1, hertz), looping);
        play(audio, 0.2);
        if (filtered) {
            audio.setBusEffect("music", muffled);
            CHECK(audio.busEffect("music").lowpass == doctest::Approx(1000));
            play(audio, 0.05);
        }
        return rms(play(audio, 0.2), 0);
    };
    CHECK(level(8000, true) < level(8000, false) * 0.05);
    CHECK(level(200, true) > level(200, false) * 0.9);
}

TEST_CASE("bus reverb leaves a tail after the sound ends") {
    const auto blip = tone("blip", 0.1, 600);
    auto tail = [&](float mix) {
        MiniAudio audio(false);
        BusEffect cave;
        cave.reverb = mix;
        cave.room = 0.8f;
        audio.setBusEffect("sfx", cave);
        const int voice = audio.playSound(blip, SoundOptions{});
        mixUntil(audio, [&] { return !audio.playing(voice); });
        return rms(play(audio, 0.3), 0);
    };
    CHECK(tail(0) == 0);
    CHECK(tail(0.5f) > 1e-3);
}

TEST_CASE("ducking glides a bus down while the voice bus is audible and back after") {
    MiniAudio audio(false);
    SoundOptions looping;
    looping.loop = true;
    looping.bus = "music";
    audio.playSound(toneFile(), looping);
    Ducking ducking;
    ducking.amount = 0.6f;
    ducking.attack = 0.2;
    ducking.release = 0.3;
    audio.setDucking("music", ducking);
    play(audio, 0.1);
    CHECK(audio.duckGain("music") == doctest::Approx(1));
    SoundOptions line;
    line.bus = "voice";
    const int voice = audio.playSound(tone("line", 0.5), line);
    std::vector<float> gains;
    while (audio.playing(voice)) {
        play(audio, 0.01);
        gains.push_back(audio.duckGain("music"));
    }
    CHECK(gains.back() == doctest::Approx(0.4));
    CHECK(std::is_sorted(gains.rbegin(), gains.rend()));
    CHECK(std::count_if(gains.begin(), gains.end(), [](float g) { return g > 0.45f && g < 0.95f; }) >= 10);
    play(audio, 0.1);
    CHECK(audio.duckGain("music") > 0.4f);
    CHECK(audio.duckGain("music") < 1);
    play(audio, 0.4);
    CHECK(audio.duckGain("music") == doctest::Approx(1));
    CHECK_THROWS(audio.setDucking("voice", ducking));
}

TEST_CASE("positional sounds are attenuated and panned from their listener") {
    MiniAudio audio(false);
    audio.setListeners({{"field", Listener{{0, 0, 0}, {1, 0, 0}}}});
    SoundOptions options;
    options.loop = true;
    options.position = glm::vec3(4, 0, 0);
    options.listener = "field";
    options.radius = 8;
    const int voice = audio.playSound(toneFile(), options);
    CHECK(audio.voiceMix(voice).gain == doctest::Approx(0.5));
    CHECK(audio.voiceMix(voice).pan == doctest::Approx(1));
    play(audio, 0.2);
    const auto right = play(audio, 0.1);
    CHECK(rms(right, 0) < rms(right, 1) * 0.01);

    audio.setSound(voice, {.volume = 0.5f, .position = glm::vec3(0, 0, -2)});
    CHECK(audio.voiceMix(voice).gain == doctest::Approx(0.375));
    CHECK(audio.voiceMix(voice).pan == doctest::Approx(0));
    audio.setListeners({{"field", Listener{{0, 0, -4}, {-1, 0, 0}}}});
    CHECK(audio.voiceMix(voice).gain == doctest::Approx(0.375));
    audio.setListeners({{"field", Listener{{-2, 0, -2}, {0, 0, 1}}}});
    CHECK(audio.voiceMix(voice).pan == doctest::Approx(0));
    audio.setListeners({{"field", Listener{{0, 0, 0}, {0, 0, 1}}}});
    CHECK(audio.voiceMix(voice).pan == doctest::Approx(-1));
    audio.setListeners({});
    CHECK(audio.voiceMix(voice).gain == 0);
}

TEST_CASE("decoded effects stay under the cache budget; the least recently played go first") {
    MiniAudio audio(false);
    REQUIRE(audio.soundCacheBudget() == std::size_t{64} << 20);
    const auto a = tone("cache-a", 0.5), b = tone("cache-b", 0.5), c = tone("cache-c", 0.5);
    const std::size_t one = 48000 / 2 * 2 * sizeof(float); // half a second of f32 stereo
    audio.setSoundCacheBudget(one * 2 + 1024);
    audio.playSound(a, SoundOptions{});
    play(audio, 0.7);
    CHECK(audio.soundCached(a));
    CHECK(audio.soundCacheBytes() == one);
    audio.playSound(b, SoundOptions{});
    play(audio, 0.7);
    CHECK(audio.soundCached(a));
    CHECK(audio.soundCached(b));
    audio.playSound(a, SoundOptions{}); // a is now the most recently played
    play(audio, 0.7);
    audio.playSound(c, SoundOptions{});
    play(audio, 0.7);
    CHECK_FALSE(audio.soundCached(b)); // over budget: b was played longest ago
    CHECK(audio.soundCached(a));
    CHECK(audio.soundCached(c));
    CHECK(audio.soundCacheBytes() <= audio.soundCacheBudget());
    // A playing sound is never dropped, however small the budget.
    SoundOptions looping;
    looping.loop = true;
    const int voice = audio.playSound(c, looping);
    audio.setSoundCacheBudget(1);
    play(audio, 0.2);
    CHECK(audio.soundCached(c));
    CHECK_FALSE(audio.soundCached(a));
    CHECK(audio.playing(voice));
    audio.stopSound(voice, 0);
    play(audio, 0.1);
    CHECK_FALSE(audio.soundCached(c));
    CHECK(audio.soundCacheBytes() == 0);
}

TEST_CASE("effects longer than the stream threshold stream instead of being decoded") {
    MiniAudio audio(false);
    CHECK(audio.soundStreamThreshold() == doctest::Approx(10));
    audio.setSoundStreamThreshold(0.5);
    const auto file = tone("long", 3);
    const int voice = audio.playSound(file, SoundOptions{});
    mixUntil(audio, [&] { return audio.position(voice) > 0.1; });
    CHECK(audio.playing(voice));
    CHECK(audio.position(voice) > 0.1);
    CHECK_FALSE(audio.soundCached(file));
    CHECK(audio.soundCacheBytes() == 0);
    const int cached = audio.playSound(tone("short", 0.2), SoundOptions{});
    play(audio, 0.1);
    CHECK(audio.playing(cached));
    CHECK(audio.soundCached(tone("short", 0.2)));
}

TEST_CASE("the master limiter keeps a loud mix under full scale and leaves quiet sound alone") {
    auto peakOf = [](bool limiter, float volume, int voices) {
        MiniAudio audio(false);
        audio.setMasterLimiter(limiter);
        SoundOptions options;
        options.loop = true;
        options.volume = volume;
        for (int i = 0; i < voices; ++i) {
            audio.playSound(toneFile(), options);
        }
        play(audio, 0.2);
        const auto out = play(audio, 0.3);
        float peak = 0;
        for (const float s : out) {
            peak = std::max(peak, std::abs(s));
        }
        return std::make_pair(peak, rms(out, 0));
    };
    MiniAudio defaults(false);
    CHECK(defaults.masterLimiter());
    const auto [loudPeak, loudRms] = peakOf(true, 4, 3);
    const auto [rawPeak, rawRms] = peakOf(false, 4, 3);
    CHECK(rawPeak > 1.5f);
    CHECK(loudPeak <= 1.0f);
    CHECK(loudPeak > 0.9f);
    const auto [quietPeak, quietRms] = peakOf(true, 0.5f, 1);
    const auto [quietRawPeak, quietRawRms] = peakOf(false, 0.5f, 1);
    CHECK(quietRms == doctest::Approx(quietRawRms).epsilon(0.01));
    CHECK(quietPeak == doctest::Approx(quietRawPeak).epsilon(0.01));
}

TEST_CASE("night dynamics compress loud peaks and lift quiet sound") {
    auto measure = [](bool night, float volume) {
        MiniAudio audio(false);
        audio.setMasterLimiter(false);
        Dynamics d;
        d.compress = night;
        audio.setDynamics(d);
        SoundOptions options;
        options.loop = true;
        options.volume = volume;
        audio.playSound(toneFile(), options);
        play(audio, 0.3);
        return rms(play(audio, 0.3), 0);
    };
    const float loud = measure(false, 2), loudNight = measure(true, 2);
    const float quiet = measure(false, 0.05f), quietNight = measure(true, 0.05f);
    CHECK(loudNight < loud);
    CHECK(quietNight > quiet);
    CHECK(loudNight / quietNight < loud / quiet); // the range between them shrinks
    MiniAudio audio(false);
    CHECK_FALSE(audio.dynamics().compress);
}

TEST_CASE("music cued in one tick starts together; a later tick's cue is its own group") {
    MiniAudio audio(false);
    const auto file = tone("long", 3);
    MusicOptions first;
    first.layer = "pads";
    audio.playMusic(file, first);
    mixUntil(audio,
             [&] { return audio.musicPosition("pads") > 0; }); // ticks pass: later cues form another group
    MusicOptions drums, bass;
    drums.layer = "drums";
    bass.layer = "bass";
    audio.playMusic(file, drums);
    audio.playMusic(file, bass);
    mixUntil(audio, [&] { return audio.musicPosition("bass") > 0.2; });
    CHECK(audio.musicPosition("drums") == doctest::Approx(audio.musicPosition("bass")).epsilon(1e-6));
    CHECK(audio.musicPosition("pads") > audio.musicPosition("bass")); // cued and started a tick earlier
}

TEST_CASE("the pitch shifter moves a tone an octave without changing how long it plays") {
    constexpr std::size_t frames = 48000;
    std::vector<float> in(frames * 2), out;
    for (std::size_t i = 0; i < frames; ++i) {
        in[i * 2] = in[i * 2 + 1] =
            0.5f * static_cast<float>(std::sin(2 * 3.14159265358979 * 440 * static_cast<double>(i) / 48000));
    }
    CHECK(peakFrequency(in, 9600, 16384) == doctest::Approx(440).epsilon(0.02));
    const auto render = [&](float ratio) {
        PitchShift shift(48000);
        shift.setRatio(ratio);
        std::vector<float> result;
        std::size_t produced = 0;
        for (std::size_t block = 0; block < frames; block += 480) { // the mixer's 10 ms pulls
            std::vector<float> chunk(480 * 2);
            shift.process(in.data() + block * 2, chunk.data(), 480);
            result.insert(result.end(), chunk.begin(), chunk.end());
            produced += 480;
        }
        CHECK(produced == frames); // frame for frame: the duration is kept (within 2%)
        CHECK(static_cast<double>(result.size() / 2) == doctest::Approx(frames).epsilon(0.02));
        return result;
    };
    out = render(2.0f);
    CHECK(peakFrequency(out, 9600, 16384) == doctest::Approx(880).epsilon(0.02));
    CHECK(rms(out, 0) == doctest::Approx(rms(in, 0)).epsilon(0.3)); // level roughly kept
    out = render(0.5f);
    CHECK(peakFrequency(out, 9600, 16384) == doctest::Approx(220).epsilon(0.02));
    out = render(1.0f); // 1 copies the input straight through
    CHECK(out == in);
    CHECK(PitchShift::supports(0.5f));
    CHECK(PitchShift::supports(2.0f));
    CHECK_FALSE(PitchShift::supports(0.25f));
    CHECK_FALSE(PitchShift::supports(4.0f));
    PitchShift shift(48000);
    shift.setRatio(4.0f); // unsupported: passes through
    CHECK(shift.ratio() == 1.0f);
    CHECK(shift.latency(2.0f) > shift.latency(1.0f));
}

TEST_CASE("preserve_speed shifts the pitch and keeps the playback time; outside its range it is varispeed") {
    MiniAudio audio(false);
    const auto file = tone("long", 3); // 382 Hz
    MusicOptions options;
    options.pitch = 2;
    options.preserveSpeed = true;
    options.fade = 0;
    audio.playMusic(file, options);
    mixUntil(audio, [&] { return audio.musicPosition("main") > 0.1; });
    double before = audio.musicPosition("main");
    auto output = play(audio, 0.5);
    CHECK(audio.musicPosition("main") - before == doctest::Approx(0.5).epsilon(0.05)); // real time
    CHECK(peakFrequency(output, 4800, 16384) == doctest::Approx(764).epsilon(0.03));   // an octave up
    CHECK(audio.musicPitch("main") == doctest::Approx(2));
    audio.setMusicPitch("main", 2); // varispeed again: twice as fast
    before = audio.musicPosition("main");
    play(audio, 0.3);
    CHECK(audio.musicPosition("main") - before == doctest::Approx(0.6).epsilon(0.05));
    audio.setMusicPitch("main", 0.5f, true);
    before = audio.musicPosition("main");
    output = play(audio, 0.5);
    CHECK(audio.musicPosition("main") - before == doctest::Approx(0.5).epsilon(0.05));
    CHECK(peakFrequency(output, 4800, 16384) == doctest::Approx(191).epsilon(0.04));

    SoundOptions looping;
    looping.loop = true;
    looping.pitch = 1.5f;
    looping.preserveSpeed = true;
    const int voice = audio.playSound(file, looping);
    mixUntil(audio, [&] { return audio.position(voice) > 0; });
    CHECK(audio.voiceShifted(voice));
    before = audio.position(voice);
    play(audio, 0.4);
    CHECK(audio.position(voice) - before == doctest::Approx(0.4).epsilon(0.05));
    // Beyond the shifter's range the voice plays as varispeed (logged once).
    audio.setSound(voice, {.pitch = 4.0f, .preserveSpeed = true});
    CHECK_FALSE(audio.voiceShifted(voice));
    before = audio.position(voice);
    play(audio, 0.3);
    CHECK(audio.position(voice) - before == doctest::Approx(1.2).epsilon(0.05));
    // A pitch change that says nothing about speed keeps the last choice.
    audio.setSound(voice, {.pitch = 0.75f});
    CHECK(audio.voiceShifted(voice));
    before = audio.position(voice);
    play(audio, 0.4);
    CHECK(audio.position(voice) - before == doctest::Approx(0.4).epsilon(0.05));
    audio.setSound(voice, {.preserveSpeed = false});
    CHECK_FALSE(audio.voiceShifted(voice));
    before = audio.position(voice);
    play(audio, 0.4);
    CHECK(audio.position(voice) - before == doctest::Approx(0.3).epsilon(0.05));
    // Voices that never asked for it have no shifter.
    const int plain = audio.playSound(file, SoundOptions{});
    CHECK_FALSE(audio.voiceShifted(plain));
}

TEST_CASE("packed audio plays from inside the archive, stored or compressed, without being extracted") {
    TempFolder folder;
    const auto game = folder.path / "game";
    std::filesystem::create_directories(game / "audio");
    const auto read = [](const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const auto ogg = read(std::filesystem::path(FENGINE_TEST_GAME) / "audio" / "tone.ogg");
    const auto wav = read(toneFile());
    {
        PackWriter writer(folder.path / "game.pak");
        writer.add("audio/stored.ogg", ogg); // as the packer stores audio
        writer.add("audio/stored.wav", wav);
        writer.add("audio/squeezed.wav", wav, PackWriter::Compression::Zlib); // an archive made otherwise
        writer.add("audio/squeezed.ogg", ogg, PackWriter::Compression::Zlib);
        writer.finish();
    }
    mountPack(folder.path / "game.pak", false);
    REQUIRE(storedSpan(game / "audio" / "stored.ogg").has_value());
    CHECK(storedSpan(game / "audio" / "stored.ogg")->size == ogg.size());
    CHECK_FALSE(storedSpan(game / "audio" / "squeezed.wav").has_value());
    CHECK(isPacked(game / "audio" / "squeezed.wav"));
    CHECK_FALSE(isPacked(toneFile()));

    MiniAudio audio(false);
    CHECK(audio.length(game / "audio" / "stored.ogg") == doctest::Approx(0.5).epsilon(0.05));
    CHECK(audio.length(game / "audio" / "squeezed.ogg") == doctest::Approx(0.5).epsilon(0.05));
    CHECK(audio.length(game / "audio" / "stored.wav") == doctest::Approx(1.0));
    CHECK(audio.length(game / "audio" / "squeezed.wav") == doctest::Approx(1.0));
    for (const char* name : {"stored.ogg", "stored.wav", "squeezed.wav", "squeezed.ogg"}) {
        CAPTURE(name);
        const int voice = audio.playSound(game / "audio" / name, SoundOptions{});
        CHECK(audio.playing(voice));
        mixUntil(audio, [&] { return audio.position(voice) > 0; });
        CHECK(audio.position(voice) > 0); // it produced sound
        audio.stopSound(voice, 0);
    }
    // Music streams from the archive too.
    audio.playMusic(game / "audio" / "stored.wav", MusicOptions{});
    mixUntil(audio, [&] { return audio.musicPosition("main") > 0; });
    CHECK(audio.musicPosition("main") > 0);
    // Nothing was written next to the archive or anywhere under the game folder.
    CHECK_FALSE(std::filesystem::exists(game / "audio" / "stored.ogg"));
    audio.stopMusic("", 0);
    unmountPacks();
}
