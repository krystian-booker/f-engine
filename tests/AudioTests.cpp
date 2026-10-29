#include "engine/audio/MiniAudio.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <fstream>
#include <functional>
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

TEST_CASE("one-shots end by themselves; missing files throw") {
    MiniAudio audio(false);
    audio.playSound(toneFile(), SoundOptions{});
    play(audio, 1.3);
    CHECK(audio.activeSounds() == 0);
    CHECK_THROWS(audio.playSound("missing.wav", SoundOptions{}));
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
