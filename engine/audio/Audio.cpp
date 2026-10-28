#include "engine/audio/Audio.h"
#include "engine/core/Log.h"
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <stdexcept>

namespace fe {
struct Audio::Impl {
    struct Sound {
        ma_sound handle{};
        bool initialized = false;
        double retiring = -1;
        ~Sound() { if (initialized) ma_sound_uninit(&handle); }
    };
    ma_engine engine{};
    ma_sound_group music{}, effects{};
    bool initialized=false,musicInitialized=false,effectsInitialized=false;
    std::array<float,3> volumes{1,0.65f,0.8f};
    std::vector<std::unique_ptr<Sound>> musicSounds, effectSounds;
    std::filesystem::path current;
    BgmOptions currentOptions;
    explicit Impl(bool disabled) {
        if (disabled) return;
        auto config=ma_engine_config_init();
        if (ma_engine_init(&config,&engine)!=MA_SUCCESS) { log(LogLevel::Warning,"Audio device unavailable; continuing silently"); return; }
        initialized=true;
        if (ma_sound_group_init(&engine,0,nullptr,&music)!=MA_SUCCESS) return;
        musicInitialized=true;
        if (ma_sound_group_init(&engine,0,nullptr,&effects)!=MA_SUCCESS) return;
        effectsInitialized=true;
        ma_sound_group_set_volume(&music,volumes[1]); ma_sound_group_set_volume(&effects,volumes[2]);
        log(LogLevel::Info,"Audio initialized at "+std::to_string(ma_engine_get_sample_rate(&engine))+" Hz");
    }
    ~Impl() {
        effectSounds.clear(); musicSounds.clear();
        if (effectsInitialized) ma_sound_group_uninit(&effects);
        if (musicInitialized) ma_sound_group_uninit(&music);
        if (initialized) ma_engine_uninit(&engine);
    }
    [[nodiscard]] bool ready() const { return effectsInitialized; }
    std::unique_ptr<Sound> load(const std::filesystem::path& file,bool streaming) {
        auto result=std::make_unique<Sound>();
        const auto flags=MA_SOUND_FLAG_NO_SPATIALIZATION|(streaming?MA_SOUND_FLAG_STREAM:MA_SOUND_FLAG_DECODE);
        if (ma_sound_init_from_file(&engine,file.string().c_str(),flags,streaming?&music:&effects,nullptr,&result->handle)!=MA_SUCCESS)
            throw std::runtime_error("Cannot load audio " + file.string());
        result->initialized=true; return result;
    }
};
Audio::Audio(bool disabled) : impl_(std::make_unique<Impl>(disabled)) {}
Audio::~Audio()=default;
void Audio::reload(const std::filesystem::path& file) {
    auto& p=*impl_;std::error_code error;
    if (p.current.empty() || !std::filesystem::equivalent(p.current,file,error)) return;
    const auto previous=p.current;const auto options=p.currentOptions;p.current.clear();
    try {playBgm(file,options);}catch (...) {p.current=previous;throw;}
}
bool Audio::available() const { return impl_->ready(); }
void Audio::playBgm(const std::filesystem::path& file,BgmOptions options) {
    auto& p=*impl_;
    if (!p.ready() || (p.current==file && p.currentOptions==options)) return;
    if (!std::isfinite(options.fadeSeconds) || !std::isfinite(options.loopStart) || !std::isfinite(options.loopEnd) || options.fadeSeconds<0 || options.loopStart<0 ||
        (options.loopEnd>0 && options.loopEnd<=options.loopStart)) throw std::runtime_error("Invalid BGM fade or loop points");
    auto sound=p.load(file,true);
    ma_sound_set_looping(&sound->handle,options.loop);
    if (options.loopStart>0 || options.loopEnd>0) {
        ma_uint32 rate=0;
        if (ma_sound_get_data_format(&sound->handle,nullptr,nullptr,&rate,nullptr,0)!=MA_SUCCESS || rate==0) throw std::runtime_error("Cannot read BGM sample rate");
        const auto begin=static_cast<ma_uint64>(options.loopStart*rate);
        const auto end=options.loopEnd>0?static_cast<ma_uint64>(options.loopEnd*rate):~ma_uint64(0);
        if (ma_data_source_set_loop_point_in_pcm_frames(ma_sound_get_data_source(&sound->handle),begin,end)!=MA_SUCCESS)
            throw std::runtime_error("Cannot set BGM loop points");
        log(LogLevel::Info,"BGM loop frames "+std::to_string(begin)+".."+std::to_string(end));
    }
    stopBgm(options.fadeSeconds);
    if (options.fadeSeconds>0) ma_sound_set_fade_in_milliseconds(&sound->handle,0,1,static_cast<ma_uint64>(options.fadeSeconds*1000));
    ma_sound_start(&sound->handle);p.musicSounds.push_back(std::move(sound));p.current=file;p.currentOptions=options;
}
void Audio::stopBgm(double seconds) {
    if (!std::isfinite(seconds) || seconds<0) throw std::runtime_error("Invalid BGM fade duration");
    auto& p=*impl_; p.current.clear();
    for (auto& sound : p.musicSounds) {
        sound->retiring=std::max(0.0,seconds);
        ma_sound_set_fade_in_milliseconds(&sound->handle,-1,0,static_cast<ma_uint64>(std::max(0.0,seconds)*1000));
    }
}
void Audio::playSfx(const std::filesystem::path& file,float volume,float pitch) {
    auto& p=*impl_; if (!p.ready()) return;
    if (!std::isfinite(volume) || !std::isfinite(pitch)) throw std::runtime_error("Invalid SFX volume or pitch");
    auto sound=p.load(file,false);
    ma_sound_set_volume(&sound->handle,std::clamp(volume,0.0f,4.0f)); ma_sound_set_pitch(&sound->handle,std::clamp(pitch,0.1f,4.0f));
    ma_sound_start(&sound->handle); p.effectSounds.push_back(std::move(sound));
}
void Audio::setVolume(AudioBus bus,float volume) {
    if (!std::isfinite(volume)) throw std::runtime_error("Volume must be finite");
    auto& p=*impl_; const auto value=std::clamp(volume,0.0f,1.0f); p.volumes.at(static_cast<std::size_t>(bus))=value;
    if (!p.ready()) return;
    switch (bus) {
    case AudioBus::Master: ma_engine_set_volume(&p.engine,value); break;
    case AudioBus::Music: ma_sound_group_set_volume(&p.music,value); break;
    case AudioBus::Effects: ma_sound_group_set_volume(&p.effects,value); break;
    }
}
float Audio::volume(AudioBus bus) const { return impl_->volumes.at(static_cast<std::size_t>(bus)); }
void Audio::update(double dt) {
    auto& p=*impl_;
    if (!std::isfinite(dt) || dt<0) return;
    for (auto& sound : p.musicSounds) if (sound->retiring>=0) sound->retiring=std::max(0.0,sound->retiring-dt);
    std::erase_if(p.musicSounds,[](const auto& sound) { return sound->retiring==0 || ma_sound_at_end(&sound->handle); });
    if (p.musicSounds.empty()) p.current.clear();
    std::erase_if(p.effectSounds,[](const auto& sound) { return ma_sound_at_end(&sound->handle); });
}
}
