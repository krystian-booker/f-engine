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
// Signal flow: sound -> bus group (volume) -> bus effects (low-pass, reverb,
// ducking) -> master group -> master effects -> device. The buses master,
// music, sfx and voice always exist; others are created on first use.
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
    void setMusicPitch(const std::string& layer, float pitch) override;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fe
