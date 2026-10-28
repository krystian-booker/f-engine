#pragma once
#include "engine/services/Audio.h"

namespace fe {
// miniaudio implementation of the Audio service.
class MiniAudio final : public Audio {
public:
    MiniAudio();
    ~MiniAudio() override;
    MiniAudio(const MiniAudio&) = delete;
    MiniAudio& operator=(const MiniAudio&) = delete;
    [[nodiscard]] bool available() const override;
    void playMusic(const std::filesystem::path& file, const MusicOptions& options) override;
    void stopMusic(const std::string& layer, double fade) override;
    void setMusicVolume(const std::string& layer, float volume, double fade) override;
    [[nodiscard]] std::string currentMusic(const std::string& layer) const override;
    int playSound(const std::filesystem::path& file, const SoundOptions& options) override;
    void stopSound(int voice, double fade) override;
    void setSound(int voice, float volume, float pan) override;
    void setBusVolume(const std::string& bus, float volume) override;
    [[nodiscard]] float busVolume(const std::string& bus) const override;
    void update(double dt) override;
    void reload(const std::filesystem::path& file) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fe
