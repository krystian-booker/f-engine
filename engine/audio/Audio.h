#pragma once
#include <filesystem>
#include <memory>

namespace fe {
enum class AudioBus { Master, Music, Effects };
struct BgmOptions {
    double fadeSeconds = 0.75, loopStart = 0, loopEnd = 0;
    bool loop = true;
    bool operator==(const BgmOptions&) const=default;
};
class Audio {
public:
    explicit Audio(bool disabled = false);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;
    [[nodiscard]] bool available() const;
    void playBgm(const std::filesystem::path& file, BgmOptions options = {});
    void stopBgm(double fadeSeconds = 0.5);
    void playSfx(const std::filesystem::path& file, float volume = 1, float pitch = 1);
    void setVolume(AudioBus bus, float volume);
    [[nodiscard]] float volume(AudioBus bus) const;
    void update(double dt);
    void reload(const std::filesystem::path& file);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
