#pragma once
#include "engine/services/Audio.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fe {
// miniaudio implementation of the Audio service.
//
// The mixer runs at a fixed 48 kHz stereo and is pulled by a playback device
// on the system's default output. When the default output changes (headphones
// plugged in), CoreAudio and WASAPI follow it inside miniaudio. If the device
// stops or disappears the mixer keeps running on the game thread (so fades and
// one-shots still finish) and the default device is reopened once a second
// until one is available.
//
// Files load on miniaudio's job thread: sound effects are decoded once and
// cached, music streams. Music tracks start once their stream is buffered,
// together with every other track cued at the same time, so stems started in
// the same tick begin on the same frame. A stem with `sync` joining a main
// track that is already playing is buffered at the main track's position a
// moment ahead and scheduled to start exactly then. Seeking main re-cues its
// synced stems with it.
//
// Signal flow: sound [-> pitch shifter] -> bus group (volume) -> bus effects
// (low-pass, reverb, ducking) -> master group -> master effects -> device. The
// buses master, music, sfx and voice always exist; others are created on first
// use. A sound asked to keep its speed under a pitch change gets a PitchShift
// node of its own between it and its bus; otherwise pitch is miniaudio's
// resampler (varispeed).
class MiniAudio final : public Audio {
public:
    static constexpr unsigned SampleRate = 48000, Channels = 2;
    // `openDevice` false mixes only when mix() is called (tests).
    explicit MiniAudio(bool openDevice = true);
    ~MiniAudio() override;
    MiniAudio(const MiniAudio&) = delete;
    MiniAudio& operator=(const MiniAudio&) = delete;
    [[nodiscard]] bool available() const override;
    void playMusic(const std::filesystem::path& file, const MusicOptions& options) override;
    void stopMusic(const std::string& layer, double fade) override;
    void setMusicVolume(const std::string& layer, float volume, double fade) override;
    void setMusicPitch(const std::string& layer, float pitch, bool preserveSpeed = false) override;
    [[nodiscard]] float musicVolume(const std::string& layer) const override;
    [[nodiscard]] float musicPitch(const std::string& layer) const override;
    [[nodiscard]] std::string currentMusic(const std::string& layer) const override;
    [[nodiscard]] double musicPosition(const std::string& layer) const override;
    void seekMusic(const std::string& layer, double seconds) override;
    int playSound(const std::filesystem::path& file, const SoundOptions& options) override;
    void stopSound(int voice, double fade) override;
    void setSound(int voice, const VoiceChange& change) override;
    [[nodiscard]] bool playing(int voice) const override;
    [[nodiscard]] double position(int voice) const override;
    [[nodiscard]] double length(const std::filesystem::path& file) override;
    void setBusVolume(const std::string& bus, float volume) override;
    [[nodiscard]] float busVolume(const std::string& bus) const override;
    void setBusEffect(const std::string& bus, const BusEffect& effect) override;
    [[nodiscard]] BusEffect busEffect(const std::string& bus) const override;
    void setDucking(const std::string& bus, const Ducking& ducking) override;
    void pauseSounds(bool paused) override;
    [[nodiscard]] bool soundsPaused() const override;
    void setListeners(std::map<std::string, Listener> listeners) override;
    // Starts cued music, releases finished and faded-out sounds (timed by the
    // mixer, so game time scaling and pauses don't delay them) and recovers a
    // lost device.
    void update(double dt) override;
    void reload(const std::filesystem::path& file) override;

    // Stops the device while the game is suspended (window in background).
    void setPaused(bool paused);
    // Decoded sound effects are kept up to this many bytes (default 64 MiB;
    // decoded audio is f32 stereo at 48 kHz, 384 KB per second). Over the
    // budget the least recently played files are dropped first; a file a
    // voice is still playing is never dropped. Files longer than the stream
    // threshold (default 10 s) stream from disk instead of being decoded.
    void setSoundCacheBudget(std::size_t bytes);
    [[nodiscard]] std::size_t soundCacheBudget() const;
    [[nodiscard]] std::size_t soundCacheBytes() const; // decoded bytes held now
    [[nodiscard]] bool soundCached(const std::filesystem::path& file) const;
    void setSoundStreamThreshold(double seconds);
    [[nodiscard]] double soundStreamThreshold() const;
    // At most this many voices (one-shots and looping effects, fading ones
    // included) play at once, 64 by default. A sound started when the cap is
    // reached steals a voice: one already fading out if any, else the oldest
    // non-looping voice on the lowest-priority bus (sfx, then game-defined
    // buses, voice, music); only when every voice loops is a looping one
    // stolen. A stolen voice stops at once and no longer reports `playing`.
    void setMaxVoices(std::size_t count);
    [[nodiscard]] std::size_t maxVoices() const;
    // The soft limiter on the master bus (BusDsp), on by default.
    void setMasterLimiter(bool on);
    [[nodiscard]] bool masterLimiter() const;
    // Mixes frames, advancing the mixer clock; the interleaved stereo output
    // is appended to `output` when given.
    void mix(std::uint32_t frames, std::vector<float>* output = nullptr);
    // Audible gain of a music layer's current track (volume and fade), the
    // number of sounds still held (music and effects), the volume and pan a
    // voice is mixed at (after positioning), and a bus's ducking gain.
    [[nodiscard]] float musicGain(const std::string& layer) const;
    [[nodiscard]] std::size_t activeSounds() const;
    [[nodiscard]] Spatial voiceMix(int voice) const;
    [[nodiscard]] float duckGain(const std::string& bus) const;
    // Whether a voice plays through a pitch shifter (preserveSpeed within range).
    [[nodiscard]] bool voiceShifted(int voice) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fe
