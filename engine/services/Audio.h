#pragma once
#include <algorithm>
#include <filesystem>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <string>

namespace fe {
struct MusicOptions {
    std::string layer = "main"; // independent stems play on separate layers
    double fade = 0.75, loopStart = 0, loopEnd = 0;
    float volume = 1, pitch = 1;
    bool loop = true;
    bool sync = false; // start at the main layer's position (stems)
};

// Where positional sounds are heard from: a world's camera.
struct Listener {
    glm::vec3 position{0}, right{1, 0, 0};
};

struct Spatial {
    float gain = 1, pan = 0;
};

// Gain falls linearly to zero at `radius`; pan follows the listener's right
// vector. Shared by looping entity emitters and positional one-shots.
inline Spatial spatialize(const Listener& listener, glm::vec3 position, float radius) {
    const auto offset = position - listener.position;
    const float distance = glm::length(offset);
    return {std::max(0.0f, 1 - distance / std::max(0.1f, radius)),
            distance > 0.01f ? std::clamp(glm::dot(offset / distance, listener.right), -1.0f, 1.0f) : 0.0f};
}

struct SoundOptions {
    float volume = 1, pitch = 1, pan = 0;
    bool loop = false;
    std::string bus = "sfx";
    // Positional sounds are attenuated and panned from the named listener;
    // with no such listener they are silent.
    std::optional<glm::vec3> position;
    std::string listener;
    float radius = 8;
};

// Fields left empty keep their current value.
struct VoiceChange {
    // Default initializers, so designated initializers may leave fields out
    // (GCC's -Wmissing-field-initializers).
    std::optional<float> volume{}, pan{}, pitch{};
    std::optional<glm::vec3> position{};
};

struct BusEffect {
    float lowpass = 0; // cutoff in Hz; 0 = off
    float reverb = 0;  // wet mix 0..1
    float room = 0.7f, damping = 0.5f;
    double fade = 0; // seconds to glide from the previous settings
};

// Sidechain ducking: a bus's gain drops by `amount` while the key bus is
// audible and recovers after it falls silent.
struct Ducking {
    float amount = 0;
    std::string key = "voice";
    double attack = 0.08, release = 0.5;
};

// Audio playback service. The runtime implements it with miniaudio;
// headless runs use NullAudio, which records what was requested.
class Audio {
public:
    virtual ~Audio() = default;
    [[nodiscard]] virtual bool available() const = 0;
    virtual void playMusic(const std::filesystem::path& file, const MusicOptions& options) = 0;
    virtual void stopMusic(const std::string& layer, double fade) = 0; // empty layer = all
    virtual void setMusicVolume(const std::string& layer, float volume, double fade) = 0;
    virtual void setMusicPitch(const std::string& layer, float pitch) = 0;
    // The volume and pitch a layer was last given (1 when it has none).
    [[nodiscard]] virtual float musicVolume(const std::string& layer) const = 0;
    [[nodiscard]] virtual float musicPitch(const std::string& layer) const = 0;
    [[nodiscard]] virtual std::string currentMusic(const std::string& layer) const = 0;
    [[nodiscard]] virtual double musicPosition(const std::string& layer) const = 0; // seconds
    virtual void seekMusic(const std::string& layer, double seconds) = 0;
    virtual int playSound(const std::filesystem::path& file, const SoundOptions& options) = 0;
    virtual void stopSound(int voice, double fade) = 0;
    virtual void setSound(int voice, const VoiceChange& change) = 0;
    // A voice plays until it ends or is stopped; paused voices still count.
    [[nodiscard]] virtual bool playing(int voice) const = 0;
    [[nodiscard]] virtual double position(int voice) const = 0; // seconds
    [[nodiscard]] virtual double length(const std::filesystem::path& file) = 0;
    virtual void setBusVolume(const std::string& bus, float volume) = 0;
    [[nodiscard]] virtual float busVolume(const std::string& bus) const = 0;
    virtual void setBusEffect(const std::string& bus, const BusEffect& effect) = 0;
    [[nodiscard]] virtual BusEffect busEffect(const std::string& bus) const = 0;
    virtual void setDucking(const std::string& bus, const Ducking& ducking) = 0;
    // Pauses everything playing now; sounds started while paused play (menu
    // sounds). Separate from the runtime's suspension on focus loss.
    virtual void pauseSounds(bool paused) = 0;
    [[nodiscard]] virtual bool soundsPaused() const = 0;
    // Replaces all listeners; called once per tick with each drawn world.
    virtual void setListeners(std::map<std::string, Listener> listeners) = 0;
    virtual void update(double dt) = 0;
    virtual void reload(const std::filesystem::path& file) = 0;
};

class NullAudio : public Audio {
public:
    [[nodiscard]] bool available() const override { return false; }
    void playMusic(const std::filesystem::path& file, const MusicOptions& options) override {
        music_[options.layer] = file.generic_string();
        volumes_[options.layer] = options.volume;
        pitches_[options.layer] = options.pitch;
    }
    void stopMusic(const std::string& layer, double) override {
        if (layer.empty()) {
            music_.clear();
        } else {
            music_.erase(layer);
        }
    }
    void setMusicVolume(const std::string& layer, float volume, double) override { volumes_[layer] = volume; }
    void setMusicPitch(const std::string& layer, float pitch) override { pitches_[layer] = pitch; }
    [[nodiscard]] float musicVolume(const std::string& layer) const override {
        const auto found = volumes_.find(layer);
        return found == volumes_.end() ? 1.0f : found->second;
    }
    [[nodiscard]] float musicPitch(const std::string& layer) const override {
        const auto found = pitches_.find(layer);
        return found == pitches_.end() ? 1.0f : found->second;
    }
    [[nodiscard]] std::string currentMusic(const std::string& layer) const override {
        const auto found = music_.find(layer);
        return found == music_.end() ? std::string{} : found->second;
    }
    [[nodiscard]] double musicPosition(const std::string&) const override { return 0; }
    void seekMusic(const std::string&, double) override {}
    int playSound(const std::filesystem::path& file, const SoundOptions&) override {
        lastSound_ = file.generic_string();
        return ++voices_;
    }
    void stopSound(int, double) override {}
    void setSound(int, const VoiceChange&) override {}
    [[nodiscard]] bool playing(int) const override { return false; }
    [[nodiscard]] double position(int) const override { return 0; }
    [[nodiscard]] double length(const std::filesystem::path&) override { return 0; }
    void setBusVolume(const std::string& bus, float volume) override { buses_[bus] = volume; }
    [[nodiscard]] float busVolume(const std::string& bus) const override {
        const auto found = buses_.find(bus);
        return found == buses_.end() ? 1.0f : found->second;
    }
    void setBusEffect(const std::string& bus, const BusEffect& effect) override { effects_[bus] = effect; }
    [[nodiscard]] BusEffect busEffect(const std::string& bus) const override {
        const auto found = effects_.find(bus);
        return found == effects_.end() ? BusEffect{} : found->second;
    }
    void setDucking(const std::string&, const Ducking&) override {}
    void pauseSounds(bool paused) override { paused_ = paused; }
    [[nodiscard]] bool soundsPaused() const override { return paused_; }
    void setListeners(std::map<std::string, Listener>) override {}
    void update(double) override {}
    void reload(const std::filesystem::path&) override {}
    [[nodiscard]] const std::string& lastSound() const { return lastSound_; }

private:
    std::map<std::string, std::string> music_;
    std::map<std::string, float> buses_, volumes_, pitches_;
    std::map<std::string, BusEffect> effects_;
    std::string lastSound_;
    int voices_ = 0;
    bool paused_ = false;
};
} // namespace fe
