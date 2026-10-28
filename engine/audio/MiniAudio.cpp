#include "engine/audio/MiniAudio.h"
#include "engine/core/Log.h"
#define MINIAUDIO_IMPLEMENTATION
#include <algorithm>
#include <cmath>
#include <map>
#include <miniaudio.h>
#include <stdexcept>
#include <vector>

namespace fe {
struct MiniAudio::Impl {
    struct Sound {
        ma_sound handle{};
        bool initialized = false;
        double retiring = -1; // seconds left of a fade-out before release
        ~Sound() {
            if (initialized) {
                ma_sound_uninit(&handle);
            }
        }
    };
    struct Layer {
        std::vector<std::unique_ptr<Sound>> sounds; // the newest is current; older ones fade out
        std::filesystem::path file;
        MusicOptions options;
    };
    ma_engine engine{};
    bool ready = false;
    std::map<std::string, std::unique_ptr<ma_sound_group>> groups;
    std::map<std::string, float> volumes;
    std::map<std::string, Layer> layers;
    std::map<int, std::unique_ptr<Sound>> voices;
    int nextVoice = 1;

    ma_sound_group* group(const std::string& name) {
        if (!ready) {
            return nullptr;
        }
        auto& slot = groups[name];
        if (!slot) {
            slot = std::make_unique<ma_sound_group>();
            if (ma_sound_group_init(&engine, 0, nullptr, slot.get()) != MA_SUCCESS) {
                groups.erase(name);
                return nullptr;
            }
            const auto found = volumes.find(name);
            ma_sound_group_set_volume(slot.get(), found == volumes.end() ? 1.0f : found->second);
        }
        return slot.get();
    }

    std::unique_ptr<Sound> load(const std::filesystem::path& file, bool stream, const std::string& bus) {
        auto sound = std::make_unique<Sound>();
        const auto flags =
            MA_SOUND_FLAG_NO_SPATIALIZATION | (stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE);
        if (ma_sound_init_from_file(&engine, file.string().c_str(), flags, group(bus), nullptr,
                                    &sound->handle) != MA_SUCCESS) {
            throw std::runtime_error("Cannot load audio " + file.string());
        }
        sound->initialized = true;
        return sound;
    }

    static void fadeOut(Sound& sound, double seconds) {
        sound.retiring = std::max(0.0, seconds);
        ma_sound_set_fade_in_milliseconds(&sound.handle, -1, 0,
                                          static_cast<ma_uint64>(sound.retiring * 1000));
    }
};

MiniAudio::MiniAudio() : impl_(std::make_unique<Impl>()) {
    auto config = ma_engine_config_init();
    if (ma_engine_init(&config, &impl_->engine) != MA_SUCCESS) {
        log(LogLevel::Warning, "No audio device; continuing silently");
        return;
    }
    impl_->ready = true;
    log(LogLevel::Info, "Audio at " + std::to_string(ma_engine_get_sample_rate(&impl_->engine)) + " Hz");
}

MiniAudio::~MiniAudio() {
    impl_->voices.clear();
    impl_->layers.clear();
    for (auto& [name, group] : impl_->groups) {
        (void)name;
        ma_sound_group_uninit(group.get());
    }
    impl_->groups.clear();
    if (impl_->ready) {
        ma_engine_uninit(&impl_->engine);
    }
}

bool MiniAudio::available() const {
    return impl_->ready;
}

void MiniAudio::playMusic(const std::filesystem::path& file, const MusicOptions& options) {
    auto& p = *impl_;
    if (!p.ready) {
        return;
    }
    auto& layer = p.layers[options.layer];
    if (layer.file == file && !layer.sounds.empty() && layer.sounds.back()->retiring < 0) {
        return; // already playing
    }
    if (!std::isfinite(options.fade) || options.fade < 0 || options.loopStart < 0 ||
        (options.loopEnd > 0 && options.loopEnd <= options.loopStart)) {
        throw std::runtime_error("Invalid music fade or loop points");
    }
    auto sound = p.load(file, true, "music");
    ma_sound_set_looping(&sound->handle, options.loop);
    if (options.loopStart > 0 || options.loopEnd > 0) {
        ma_uint32 rate = 0;
        if (ma_sound_get_data_format(&sound->handle, nullptr, nullptr, &rate, nullptr, 0) != MA_SUCCESS ||
            rate == 0) {
            throw std::runtime_error("Cannot read the music sample rate: " + file.string());
        }
        const auto begin = static_cast<ma_uint64>(options.loopStart * rate);
        const auto end = options.loopEnd > 0 ? static_cast<ma_uint64>(options.loopEnd * rate) : ~ma_uint64(0);
        ma_data_source_set_loop_point_in_pcm_frames(ma_sound_get_data_source(&sound->handle), begin, end);
    }
    if (options.sync && options.layer != "main") {
        // Stems start in step with the main track.
        const auto main = p.layers.find("main");
        if (main != p.layers.end() && !main->second.sounds.empty()) {
            ma_uint64 cursor = 0;
            ma_sound_get_cursor_in_pcm_frames(&main->second.sounds.back()->handle, &cursor);
            ma_sound_seek_to_pcm_frame(&sound->handle, cursor);
        }
    }
    for (auto& old : layer.sounds) {
        if (old->retiring < 0) {
            Impl::fadeOut(*old, options.fade);
        }
    }
    ma_sound_set_volume(&sound->handle, options.volume);
    if (options.fade > 0) {
        ma_sound_set_fade_in_milliseconds(&sound->handle, 0, options.volume,
                                          static_cast<ma_uint64>(options.fade * 1000));
    }
    ma_sound_start(&sound->handle);
    layer.sounds.push_back(std::move(sound));
    layer.file = file;
    layer.options = options;
}

void MiniAudio::stopMusic(const std::string& layerName, double fade) {
    for (auto& [name, layer] : impl_->layers) {
        if (!layerName.empty() && name != layerName) {
            continue;
        }
        for (auto& sound : layer.sounds) {
            if (sound->retiring < 0) {
                Impl::fadeOut(*sound, fade);
            }
        }
        layer.file.clear();
    }
}

void MiniAudio::setMusicVolume(const std::string& layerName, float volume, double fade) {
    const auto found = impl_->layers.find(layerName);
    if (found == impl_->layers.end() || found->second.sounds.empty()) {
        return;
    }
    auto& sound = *found->second.sounds.back();
    ma_sound_set_fade_in_milliseconds(&sound.handle, -1, std::clamp(volume, 0.0f, 4.0f),
                                      static_cast<ma_uint64>(fade * 1000));
}

std::string MiniAudio::currentMusic(const std::string& layer) const {
    const auto found = impl_->layers.find(layer);
    return found == impl_->layers.end() ? std::string{} : found->second.file.string();
}

int MiniAudio::playSound(const std::filesystem::path& file, const SoundOptions& options) {
    auto& p = *impl_;
    if (!p.ready) {
        return -1;
    }
    auto sound = p.load(file, false, options.bus);
    ma_sound_set_volume(&sound->handle, std::clamp(options.volume, 0.0f, 4.0f));
    ma_sound_set_pitch(&sound->handle, std::clamp(options.pitch, 0.1f, 4.0f));
    ma_sound_set_pan(&sound->handle, std::clamp(options.pan, -1.0f, 1.0f));
    ma_sound_set_looping(&sound->handle, options.loop);
    ma_sound_start(&sound->handle);
    const int voice = p.nextVoice++;
    p.voices[voice] = std::move(sound);
    return voice;
}

void MiniAudio::stopSound(int voice, double fade) {
    const auto found = impl_->voices.find(voice);
    if (found != impl_->voices.end()) {
        Impl::fadeOut(*found->second, fade);
    }
}

void MiniAudio::setSound(int voice, float volume, float pan) {
    const auto found = impl_->voices.find(voice);
    if (found != impl_->voices.end()) {
        ma_sound_set_volume(&found->second->handle, std::clamp(volume, 0.0f, 4.0f));
        ma_sound_set_pan(&found->second->handle, std::clamp(pan, -1.0f, 1.0f));
    }
}

void MiniAudio::setBusVolume(const std::string& bus, float volume) {
    auto& p = *impl_;
    volume = std::clamp(volume, 0.0f, 1.0f);
    p.volumes[bus] = volume;
    if (!p.ready) {
        return;
    }
    if (bus == "master") {
        ma_engine_set_volume(&p.engine, volume);
    } else if (auto* group = p.group(bus)) {
        ma_sound_group_set_volume(group, volume);
    }
}

float MiniAudio::busVolume(const std::string& bus) const {
    const auto found = impl_->volumes.find(bus);
    return found == impl_->volumes.end() ? 1.0f : found->second;
}

void MiniAudio::update(double dt) {
    auto& p = *impl_;
    auto expired = [dt](std::unique_ptr<Impl::Sound>& sound) {
        if (sound->retiring >= 0) {
            sound->retiring -= dt;
            if (sound->retiring <= 0) {
                return true;
            }
        }
        return ma_sound_at_end(&sound->handle) == MA_TRUE;
    };
    for (auto& [name, layer] : p.layers) {
        (void)name;
        std::erase_if(layer.sounds, expired);
    }
    std::erase_if(p.voices, [&](auto& entry) { return expired(entry.second); });
}

void MiniAudio::reload(const std::filesystem::path& file) {
    for (auto& [name, layer] : impl_->layers) {
        (void)name;
        std::error_code ec;
        if (!layer.file.empty() && std::filesystem::equivalent(layer.file, file, ec)) {
            const auto options = layer.options;
            layer.file.clear();
            playMusic(file, options);
        }
    }
}
} // namespace fe
