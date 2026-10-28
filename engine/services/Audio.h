#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace fe {
struct MusicOptions {
    std::string layer = "main"; // independent stems play on separate layers
    double fade = 0.75, loopStart = 0, loopEnd = 0;
    float volume = 1;
    bool loop = true;
    bool sync = false; // start at the main layer's position (stems)
};

struct SoundOptions {
    float volume = 1, pitch = 1, pan = 0;
    bool loop = false;
    std::string bus = "sfx";
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
    [[nodiscard]] virtual std::string currentMusic(const std::string& layer) const = 0;
    virtual int playSound(const std::filesystem::path& file, const SoundOptions& options) = 0;
    virtual void stopSound(int voice, double fade) = 0;
    virtual void setSound(int voice, float volume, float pan) = 0;
    virtual void setBusVolume(const std::string& bus, float volume) = 0;
    [[nodiscard]] virtual float busVolume(const std::string& bus) const = 0;
    virtual void update(double dt) = 0;
    virtual void reload(const std::filesystem::path& file) = 0;
};

class NullAudio final : public Audio {
public:
    [[nodiscard]] bool available() const override { return false; }
    void playMusic(const std::filesystem::path& file, const MusicOptions& options) override {
        music_[options.layer] = file.generic_string();
    }
    void stopMusic(const std::string& layer, double) override {
        if (layer.empty()) {
            music_.clear();
        } else {
            music_.erase(layer);
        }
    }
    void setMusicVolume(const std::string&, float, double) override {}
    [[nodiscard]] std::string currentMusic(const std::string& layer) const override {
        const auto found = music_.find(layer);
        return found == music_.end() ? std::string{} : found->second;
    }
    int playSound(const std::filesystem::path& file, const SoundOptions&) override {
        lastSound_ = file.generic_string();
        return ++voices_;
    }
    void stopSound(int, double) override {}
    void setSound(int, float, float) override {}
    void setBusVolume(const std::string& bus, float volume) override { buses_[bus] = volume; }
    [[nodiscard]] float busVolume(const std::string& bus) const override {
        const auto found = buses_.find(bus);
        return found == buses_.end() ? 1.0f : found->second;
    }
    void update(double) override {}
    void reload(const std::filesystem::path&) override {}
    [[nodiscard]] const std::string& lastSound() const { return lastSound_; }

private:
    std::map<std::string, std::string> music_;
    std::map<std::string, float> buses_;
    std::string lastSound_;
    int voices_ = 0;
};
} // namespace fe
