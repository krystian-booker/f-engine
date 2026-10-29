#include "engine/audio/MiniAudio.h"
#include "engine/audio/BusDsp.h"
#include "engine/core/Log.h"
#include "engine/core/Paths.h"
#define MINIAUDIO_IMPLEMENTATION
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <miniaudio.h>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace fe {
namespace {
using Clock = std::chrono::steady_clock;

// How far ahead cued music is scheduled, so every track of a cue (and a stem
// joining a playing main track) starts on the same mixer frame.
constexpr double StartLead = 0.08, StartMargin = 0.02;

ma_uint64 framesFor(double seconds) {
    return static_cast<ma_uint64>(std::max(0.0, seconds) * MiniAudio::SampleRate);
}

void checkFinite(double value, const char* what) {
    if (!std::isfinite(value)) {
        throw std::runtime_error(std::string("Invalid ") + what);
    }
}

struct BusNode {
    ma_node_base base;
    BusDsp* dsp;
};

void processBus(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount) {
    const auto frames = std::min(*inCount, *outCount);
    static_cast<BusNode*>(node)->dsp->process(in[0], out[0], frames);
    *inCount = frames;
    *outCount = frames;
}

ma_node_vtable busVtable{processBus, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING};
} // namespace

struct MiniAudio::Impl {
    struct Sound {
        enum class Cue { None, Group, Joining };
        ma_sound handle{};
        bool initialized = false;
        std::string file;
        std::optional<ma_uint64> retireAt; // mixer frame after which a faded-out sound is released
        double loopStart = 0, loopEnd = 0;
        bool loopPending = false; // loop points wait for an asynchronous load
        bool held = false;        // stopped by pauseSounds
        // Music waits until buffered: a Group cue starts with all other Group
        // cues at `cueAt` seconds; Joining starts in step with the main layer.
        Cue cue = Cue::None;
        double cueAt = 0;
        bool cueSeeked = false, cueAtEnd = false;
        ma_uint64 startTime = 0;
        // Positional sounds: `volume` and `pan` are the script's values,
        // combined with the listener's gain and pan when applied.
        std::optional<glm::vec3> position;
        std::string listener;
        float radius = 8, volume = 1, pan = 0;
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
    struct Bus {
        explicit Bus(unsigned rate) : dsp(rate) {}
        BusDsp dsp;
        BusNode node{};
        ma_sound_group group{};
        bool nodeReady = false, groupReady = false;
        float volume = 1;
    };
    ma_engine engine{};
    bool ready = false;
    ma_device device{};
    bool wantDevice = false, deviceOpen = false, paused = false, soundsPaused = false;
    Clock::time_point lastAttempt{}, lastUpdate = Clock::now();
    std::map<std::string, std::unique_ptr<Bus>> buses;
    std::map<std::string, Layer> layers;
    std::map<int, std::unique_ptr<Sound>> voices;
    std::map<std::string, Listener> listeners;
    std::set<std::string> cached; // effect files held decoded by the resource manager
    std::map<std::string, double> lengths;
    int nextVoice = 1;

    [[nodiscard]] ma_uint64 now() const { return ma_engine_get_time_in_pcm_frames(&engine); }

    bool openDevice() {
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = Channels;
        config.sampleRate = SampleRate;
        config.pUserData = &engine;
        config.dataCallback = [](ma_device* output, void* frames, const void*, ma_uint32 count) {
            ma_engine_read_pcm_frames(static_cast<ma_engine*>(output->pUserData), frames, count, nullptr);
        };
        if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
            return false;
        }
        if (ma_device_start(&device) != MA_SUCCESS) {
            ma_device_uninit(&device);
            return false;
        }
        return true;
    }

    void closeDevice() {
        if (deviceOpen) {
            ma_device_uninit(&device);
            deviceOpen = false;
        }
    }

    // A bus: its group carries the volume, its node the effects. Every bus
    // feeds master, which feeds the device.
    Bus* bus(const std::string& name) {
        if (const auto found = buses.find(name); found != buses.end()) {
            return found->second.get();
        }
        Bus* parent = name == "master" ? nullptr : bus("master");
        if (name != "master" && !parent) {
            return nullptr;
        }
        auto created = std::make_unique<Bus>(SampleRate);
        auto& b = *created;
        const ma_uint32 channels[1] = {Channels};
        auto config = ma_node_config_init();
        config.vtable = &busVtable;
        config.pInputChannels = channels;
        config.pOutputChannels = channels;
        b.node.dsp = &b.dsp;
        if (ma_node_init(ma_engine_get_node_graph(&engine), &config, nullptr, &b.node) != MA_SUCCESS) {
            return nullptr;
        }
        b.nodeReady = true;
        if (ma_sound_group_init(&engine, 0, nullptr, &b.group) != MA_SUCCESS) {
            ma_node_uninit(&b.node, nullptr);
            return nullptr;
        }
        b.groupReady = true;
        ma_node_attach_output_bus(&b.group, 0, &b.node, 0);
        ma_node_attach_output_bus(
            &b.node, 0, parent ? static_cast<ma_node*>(&parent->group) : ma_engine_get_endpoint(&engine), 0);
        return buses.emplace(name, std::move(created)).first->second.get();
    }

    void releaseBuses() {
        // Children first; master is the parent of every other bus.
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& [name, b] : buses) {
                if ((name == "master") != (pass == 1)) {
                    continue;
                }
                if (b->groupReady) {
                    ma_sound_group_uninit(&b->group);
                }
                if (b->nodeReady) {
                    ma_node_uninit(&b->node, nullptr);
                }
            }
        }
        buses.clear();
    }

    std::unique_ptr<Sound> load(const std::filesystem::path& file, ma_uint32 flags,
                                const std::string& busName) {
        if (!isFile(file)) {
            throw std::runtime_error("Cannot load audio " + file.string());
        }
        const auto local = localFile(file).string();
        auto* target = bus(busName);
        auto sound = std::make_unique<Sound>();
        if (!target ||
            ma_sound_init_from_file(&engine, local.c_str(), flags | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                    &target->group, nullptr, &sound->handle) != MA_SUCCESS) {
            throw std::runtime_error("Cannot load audio " + file.string());
        }
        sound->initialized = true;
        sound->file = file.string();
        return sound;
    }

    static ma_resource_manager_data_source* source(Sound& sound) {
        return static_cast<ma_resource_manager_data_source*>(ma_sound_get_data_source(&sound.handle));
    }

    static ma_uint32 rate(Sound& sound) {
        ma_uint32 value = 0;
        ma_sound_get_data_format(&sound.handle, nullptr, nullptr, &value, nullptr, 0);
        return value;
    }

    static bool loaded(Sound& sound) {
        return ma_resource_manager_data_source_result(source(sound)) == MA_SUCCESS;
    }

    // Applies loop points once the file's sample rate is known.
    static void applyLoop(Sound& sound) {
        const auto sampleRate = rate(sound);
        if (sampleRate == 0) {
            sound.loopPending = true;
            return;
        }
        const auto begin = static_cast<ma_uint64>(sound.loopStart * sampleRate);
        const auto end =
            sound.loopEnd > 0 ? static_cast<ma_uint64>(sound.loopEnd * sampleRate) : ~ma_uint64(0);
        ma_data_source_set_loop_point_in_pcm_frames(ma_sound_get_data_source(&sound.handle), begin, end);
        sound.loopPending = false;
    }

    void fadeOut(Sound& sound, double seconds) {
        const auto frames = framesFor(seconds);
        ma_sound_set_fade_in_pcm_frames(&sound.handle, -1, 0, frames);
        sound.retireAt = now() + frames;
    }

    // True when the sound should be released: faded out, finished, or its
    // asynchronous load failed.
    bool expired(Sound& sound, ma_uint64 time) {
        if (sound.retireAt && time >= *sound.retireAt) {
            return true;
        }
        const auto* data = source(sound);
        const auto status = data ? ma_resource_manager_data_source_result(data) : MA_SUCCESS;
        if (status != MA_SUCCESS && status != MA_BUSY) {
            log(LogLevel::Error, "Cannot decode audio " + sound.file);
            return true;
        }
        if (status == MA_SUCCESS && sound.loopPending) {
            applyLoop(sound);
        }
        return status == MA_SUCCESS && ma_sound_at_end(&sound.handle) == MA_TRUE;
    }

    Sound* currentMusic(const std::string& name) const {
        const auto found = layers.find(name);
        if (found == layers.end() || found->second.sounds.empty() || found->second.sounds.back()->retireAt) {
            return nullptr;
        }
        return found->second.sounds.back().get();
    }

    Sound* voice(int id) const {
        const auto found = voices.find(id);
        return found == voices.end() ? nullptr : found->second.get();
    }

    // A new track for a layer, cued (not yet started) at its options.
    std::unique_ptr<Sound> cueMusic(const std::filesystem::path& file, const MusicOptions& options,
                                    double fade) {
        auto sound = load(file, MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC, "music");
        ma_sound_set_looping(&sound->handle, options.loop);
        ma_sound_set_pitch(&sound->handle, options.pitch);
        sound->loopStart = options.loopStart;
        sound->loopEnd = options.loopEnd;
        if (options.loopStart > 0 || options.loopEnd > 0) {
            applyLoop(*sound);
        }
        // The fader carries the volume; the sound's own volume stays at 1.
        ma_sound_set_fade_in_pcm_frames(&sound->handle, fade > 0 ? 0.0f : options.volume, options.volume,
                                        framesFor(fade));
        sound->cue = Sound::Cue::Group;
        return sound;
    }

    // Seeks a cued track's stream to `seconds` (before it ever plays, so the
    // mixer never reads it concurrently); true once that position is buffered.
    // Positions past the end wrap for looping tracks and end the others.
    static bool buffered(Sound& sound, double seconds) {
        if (!loaded(sound)) {
            return false;
        }
        if (!sound.cueSeeked) {
            ma_uint64 length = 0;
            ma_resource_manager_data_source_get_length_in_pcm_frames(source(sound), &length);
            auto frame = static_cast<ma_uint64>(std::max(0.0, seconds) * rate(sound));
            if (length > 0 && frame >= length) {
                frame = ma_sound_is_looping(&sound.handle) ? frame % length : length;
            }
            ma_resource_manager_data_source_seek_to_pcm_frame(source(sound), frame);
            sound.cueSeeked = true;
            sound.cueAtEnd = frame >= length && length > 0;
        }
        return hasBuffer(sound);
    }

    static bool hasBuffer(Sound& sound) {
        ma_uint64 available = 0;
        ma_resource_manager_data_source_get_available_frames(source(sound), &available);
        return available > 0 || sound.cueAtEnd;
    }

    // Where the main track will be at mixer frame `at`, in seconds, wrapped
    // into its loop.
    double mainPositionAt(Sound& main, ma_uint64 at) {
        ma_uint64 cursor = 0, before = 0, after = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            before = now();
            ma_sound_get_cursor_in_pcm_frames(&main.handle, &cursor);
            after = now();
            if (before == after) {
                break;
            }
        }
        const auto origin = std::max(after, main.startTime);
        double seconds = static_cast<double>(cursor) / rate(main) +
                         (static_cast<double>(at) - static_cast<double>(origin)) / SampleRate *
                             ma_sound_get_pitch(&main.handle);
        float length = 0;
        ma_sound_get_length_in_seconds(&main.handle, &length);
        const double end = main.loopEnd > 0 ? main.loopEnd : length;
        if (ma_sound_is_looping(&main.handle) && end > main.loopStart && seconds >= end) {
            seconds = main.loopStart + std::fmod(seconds - main.loopStart, end - main.loopStart);
        }
        return seconds;
    }

    void start(Sound& sound, ma_uint64 at) {
        ma_sound_set_start_time_in_pcm_frames(&sound.handle, at);
        ma_sound_start(&sound.handle);
        sound.startTime = at;
        sound.cue = Sound::Cue::None;
    }

    void startCued() {
        std::vector<Sound*> group;
        bool ready = true;
        for (auto& [name, layer] : layers) {
            for (auto& sound : layer.sounds) {
                if (sound->cue == Sound::Cue::Group && !sound->retireAt) {
                    group.push_back(sound.get());
                    ready = buffered(*sound, sound->cueAt) && ready;
                }
            }
        }
        if (!group.empty() && ready) {
            const auto at = now() + framesFor(StartLead);
            for (auto* sound : group) {
                start(*sound, at);
            }
        }
        for (auto& [name, layer] : layers) {
            auto* sound = currentMusic(name);
            if (!sound || sound->cue != Sound::Cue::Joining) {
                continue;
            }
            auto* main = currentMusic("main");
            if (!main || main->cue != Sound::Cue::None) {
                sound->cue = Sound::Cue::Group;
                sound->cueAt = main ? main->cueAt : 0;
                continue;
            }
            if (main->held || !loaded(*sound)) {
                continue;
            }
            if (!sound->cueSeeked) {
                sound->startTime = std::max(now(), main->startTime) + framesFor(StartLead);
                buffered(*sound, mainPositionAt(*main, sound->startTime));
            } else if (hasBuffer(*sound)) {
                if (now() + framesFor(StartMargin) <= sound->startTime) {
                    start(*sound, sound->startTime);
                } else {
                    sound->cueSeeked = false; // buffering missed the slot; aim again
                }
            }
        }
    }

    void place(Sound& sound) const {
        if (!sound.position) {
            ma_sound_set_volume(&sound.handle, sound.volume);
            ma_sound_set_pan(&sound.handle, sound.pan);
            return;
        }
        const auto listener = listeners.find(sound.listener);
        const auto heard = listener == listeners.end()
                               ? Spatial{0, 0}
                               : spatialize(listener->second, *sound.position, sound.radius);
        ma_sound_set_volume(&sound.handle, sound.volume * heard.gain);
        ma_sound_set_pan(&sound.handle, std::clamp(sound.pan + heard.pan, -1.0f, 1.0f));
    }

    template<class Fn> void eachSound(Fn&& fn) {
        for (auto& [name, layer] : layers) {
            for (auto& sound : layer.sounds) {
                fn(*sound);
            }
        }
        for (auto& [id, sound] : voices) {
            fn(*sound);
        }
    }
};

MiniAudio::MiniAudio(bool openDevice) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    auto config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = Channels;
    config.sampleRate = SampleRate;
    config.defaultVolumeSmoothTimeInPCMFrames = SampleRate / 200; // per-tick volume changes glide over 5 ms
    if (ma_engine_init(&config, &p.engine) != MA_SUCCESS) {
        log(LogLevel::Warning, "Audio mixer unavailable; continuing silently");
        return;
    }
    p.ready = true;
    for (const char* name : {"master", "music", "sfx", "voice"}) {
        if (!p.bus(name)) {
            log(LogLevel::Warning, std::string("Audio bus '") + name + "' unavailable");
        }
    }
    p.wantDevice = openDevice;
    if (openDevice) {
        p.deviceOpen = p.openDevice();
        p.lastAttempt = Clock::now();
        if (!p.deviceOpen) {
            log(LogLevel::Warning, "No audio device; sound resumes when one is connected");
        }
    }
}

MiniAudio::~MiniAudio() {
    auto& p = *impl_;
    p.closeDevice();
    p.voices.clear();
    p.layers.clear();
    p.releaseBuses();
    if (p.ready) {
        for (const auto& file : p.cached) {
            ma_resource_manager_unregister_file(ma_engine_get_resource_manager(&p.engine), file.c_str());
        }
        ma_engine_uninit(&p.engine);
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
    if (layer.file == file && p.currentMusic(options.layer)) {
        return; // already playing
    }
    if (!std::isfinite(options.fade) || options.fade < 0 || options.loopStart < 0 ||
        (options.loopEnd > 0 && options.loopEnd <= options.loopStart) || !std::isfinite(options.pitch)) {
        throw std::runtime_error("Invalid music fade, loop points or pitch");
    }
    auto settings = options;
    settings.volume = std::clamp(options.volume, 0.0f, 4.0f);
    settings.pitch = std::clamp(options.pitch, 0.25f, 4.0f);
    auto sound = p.cueMusic(file, settings, options.fade);
    if (const auto* main = p.currentMusic("main"); options.sync && options.layer != "main" && main) {
        if (main->cue == Impl::Sound::Cue::None) {
            sound->cue = Impl::Sound::Cue::Joining;
        } else {
            sound->cueAt = main->cueAt;
        }
    }
    for (auto& old : layer.sounds) {
        if (!old->retireAt) {
            p.fadeOut(*old, options.fade);
        }
    }
    layer.sounds.push_back(std::move(sound));
    layer.file = file;
    layer.options = settings;
}

void MiniAudio::stopMusic(const std::string& layerName, double fade) {
    for (auto& [name, layer] : impl_->layers) {
        if (!layerName.empty() && name != layerName) {
            continue;
        }
        for (auto& sound : layer.sounds) {
            if (!sound->retireAt) {
                impl_->fadeOut(*sound, fade);
            }
        }
        layer.file.clear();
    }
}

void MiniAudio::setMusicVolume(const std::string& layerName, float volume, double fade) {
    // Tracks already fading out keep fading; only the current one changes.
    auto* sound = impl_->currentMusic(layerName);
    if (!sound) {
        return;
    }
    volume = std::clamp(volume, 0.0f, 4.0f);
    impl_->layers[layerName].options.volume = volume;
    ma_sound_set_fade_in_pcm_frames(&sound->handle, -1, volume, framesFor(fade));
}

void MiniAudio::setMusicPitch(const std::string& layerName, float pitch) {
    checkFinite(pitch, "music pitch");
    pitch = std::clamp(pitch, 0.25f, 4.0f);
    // Synced stems follow the main layer so they stay in step.
    for (auto& [name, layer] : impl_->layers) {
        if (name != layerName && !(layerName == "main" && layer.options.sync)) {
            continue;
        }
        layer.options.pitch = pitch;
        for (auto& sound : layer.sounds) {
            ma_sound_set_pitch(&sound->handle, pitch);
        }
    }
}

float MiniAudio::musicVolume(const std::string& layer) const {
    const auto found = impl_->layers.find(layer);
    return found == impl_->layers.end() ? 1.0f : found->second.options.volume;
}

float MiniAudio::musicPitch(const std::string& layer) const {
    const auto found = impl_->layers.find(layer);
    return found == impl_->layers.end() ? 1.0f : found->second.options.pitch;
}

std::string MiniAudio::currentMusic(const std::string& layer) const {
    const auto found = impl_->layers.find(layer);
    return found == impl_->layers.end() ? std::string{} : found->second.file.string();
}

double MiniAudio::musicPosition(const std::string& layer) const {
    auto* sound = impl_->currentMusic(layer);
    if (!sound) {
        return 0;
    }
    if (sound->cue == Impl::Sound::Cue::Group) {
        return sound->cueAt;
    }
    float seconds = 0;
    ma_sound_get_cursor_in_seconds(&sound->handle, &seconds);
    return seconds;
}

void MiniAudio::seekMusic(const std::string& layerName, double seconds) {
    auto& p = *impl_;
    checkFinite(seconds, "music position");
    // A fresh stream is cued at the new position: seeking a playing stream
    // stalls it while the job thread refills, which would put stems out of step.
    for (auto& [name, layer] : p.layers) {
        if (name != layerName && !(layerName == "main" && layer.options.sync)) {
            continue;
        }
        auto* current = p.currentMusic(name);
        if (!current) {
            continue;
        }
        auto sound = p.cueMusic(layer.file, layer.options, 0);
        sound->cueAt = std::max(0.0, seconds);
        p.fadeOut(*current, current->cue == Impl::Sound::Cue::None ? 0.01 : 0);
        layer.sounds.push_back(std::move(sound));
    }
}

int MiniAudio::playSound(const std::filesystem::path& file, const SoundOptions& options) {
    auto& p = *impl_;
    if (!p.ready) {
        return -1;
    }
    const auto path = localFile(file).string();
    if (!p.cached.contains(path) && isFile(file) &&
        ma_resource_manager_register_file(ma_engine_get_resource_manager(&p.engine), path.c_str(),
                                          MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE |
                                              MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_ASYNC) == MA_SUCCESS) {
        p.cached.insert(path);
    }
    auto sound = p.load(file, MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC, options.bus);
    sound->volume = std::clamp(options.volume, 0.0f, 4.0f);
    sound->pan = std::clamp(options.pan, -1.0f, 1.0f);
    sound->position = options.position;
    sound->listener = options.listener;
    sound->radius = options.radius;
    p.place(*sound);
    ma_sound_set_pitch(&sound->handle, std::clamp(options.pitch, 0.1f, 4.0f));
    ma_sound_set_looping(&sound->handle, options.loop);
    ma_sound_start(&sound->handle);
    const int voice = p.nextVoice++;
    p.voices[voice] = std::move(sound);
    return voice;
}

void MiniAudio::stopSound(int voice, double fade) {
    auto* sound = impl_->voice(voice);
    if (sound && !sound->retireAt) {
        impl_->fadeOut(*sound, fade);
    }
}

void MiniAudio::setSound(int voice, const VoiceChange& change) {
    auto* sound = impl_->voice(voice);
    if (!sound) {
        return;
    }
    if (change.volume) {
        sound->volume = std::clamp(*change.volume, 0.0f, 4.0f);
    }
    if (change.pan) {
        sound->pan = std::clamp(*change.pan, -1.0f, 1.0f);
    }
    if (change.position) {
        sound->position = change.position;
    }
    if (change.pitch) {
        ma_sound_set_pitch(&sound->handle, std::clamp(*change.pitch, 0.1f, 4.0f));
    }
    impl_->place(*sound);
}

bool MiniAudio::playing(int voice) const {
    auto* sound = impl_->voice(voice);
    if (!sound || sound->retireAt) {
        return false;
    }
    const auto status = ma_resource_manager_data_source_result(Impl::source(*sound));
    return status == MA_BUSY || (status == MA_SUCCESS && ma_sound_at_end(&sound->handle) == MA_FALSE);
}

double MiniAudio::position(int voice) const {
    auto* sound = impl_->voice(voice);
    float seconds = 0;
    if (sound && Impl::loaded(*sound)) {
        ma_sound_get_cursor_in_seconds(&sound->handle, &seconds);
    }
    return seconds;
}

double MiniAudio::length(const std::filesystem::path& file) {
    auto& p = *impl_;
    const auto path = file.string();
    if (const auto found = p.lengths.find(path); found != p.lengths.end()) {
        return found->second;
    }
    ma_decoder decoder;
    const auto config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (!isFile(file) ||
        ma_decoder_init_file(localFile(file).string().c_str(), &config, &decoder) != MA_SUCCESS) {
        throw std::runtime_error("Cannot load audio " + path);
    }
    ma_uint64 frames = 0;
    const auto result = ma_decoder_get_length_in_pcm_frames(&decoder, &frames);
    const auto rate = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);
    if (result != MA_SUCCESS || rate == 0) {
        throw std::runtime_error("Cannot measure audio " + path);
    }
    return p.lengths[path] = static_cast<double>(frames) / rate;
}

void MiniAudio::setBusVolume(const std::string& name, float volume) {
    auto& p = *impl_;
    if (!p.ready) {
        return;
    }
    if (auto* bus = p.bus(name)) {
        bus->volume = std::clamp(volume, 0.0f, 1.0f);
        ma_sound_group_set_volume(&bus->group, bus->volume);
    }
}

float MiniAudio::busVolume(const std::string& name) const {
    const auto found = impl_->buses.find(name);
    return found == impl_->buses.end() ? 1.0f : found->second->volume;
}

void MiniAudio::setBusEffect(const std::string& name, const BusEffect& effect) {
    checkFinite(effect.lowpass, "low-pass cutoff");
    checkFinite(effect.reverb, "reverb mix");
    checkFinite(effect.fade, "effect fade");
    if (auto* bus = impl_->ready ? impl_->bus(name) : nullptr) {
        bus->dsp.configure(effect);
    }
}

BusEffect MiniAudio::busEffect(const std::string& name) const {
    const auto found = impl_->buses.find(name);
    return found == impl_->buses.end() ? BusEffect{} : found->second->dsp.settings();
}

void MiniAudio::setDucking(const std::string& name, const Ducking& ducking) {
    auto& p = *impl_;
    if (ducking.key == name) {
        throw std::runtime_error("A bus cannot duck itself");
    }
    checkFinite(ducking.attack, "ducking attack");
    checkFinite(ducking.release, "ducking release");
    auto* bus = p.ready ? p.bus(name) : nullptr;
    auto* key = bus && ducking.amount > 0 ? p.bus(ducking.key) : nullptr;
    if (bus) {
        bus->dsp.duck(key ? &key->dsp : nullptr, ducking.amount, ducking.attack, ducking.release);
    }
}

void MiniAudio::pauseSounds(bool paused) {
    auto& p = *impl_;
    if (p.soundsPaused == paused) {
        return;
    }
    p.soundsPaused = paused;
    p.eachSound([&](Impl::Sound& sound) {
        if (paused && ma_node_get_state(&sound.handle) == ma_node_state_started) {
            ma_sound_stop(&sound.handle);
            sound.held = true;
        } else if (!paused && sound.held) {
            ma_sound_start(&sound.handle);
            sound.held = false;
        }
    });
}

bool MiniAudio::soundsPaused() const {
    return impl_->soundsPaused;
}

void MiniAudio::setListeners(std::map<std::string, Listener> listeners) {
    auto& p = *impl_;
    p.listeners = std::move(listeners);
    for (auto& [id, sound] : p.voices) {
        if (sound->position) {
            p.place(*sound);
        }
    }
}

void MiniAudio::update(double) {
    auto& p = *impl_;
    if (!p.ready) {
        return;
    }
    const auto wall = Clock::now();
    const double elapsed = std::chrono::duration<double>(wall - p.lastUpdate).count();
    p.lastUpdate = wall;
    if (p.wantDevice && !p.paused) {
        if (p.deviceOpen && ma_device_get_state(&p.device) != ma_device_state_started) {
            log(LogLevel::Warning, "Audio device lost");
            p.closeDevice();
        }
        if (!p.deviceOpen && wall - p.lastAttempt > std::chrono::seconds(1)) {
            p.lastAttempt = wall;
            p.deviceOpen = p.openDevice();
            if (p.deviceOpen) {
                log(LogLevel::Info, "Audio device opened");
            }
        }
        if (!p.deviceOpen) {
            // Keep time moving so fades and one-shots still finish.
            mix(static_cast<std::uint32_t>(framesFor(std::min(elapsed, 0.25))));
        }
    }
    const auto time = p.now();
    for (auto& entry : p.layers) {
        std::erase_if(entry.second.sounds, [&](auto& sound) { return p.expired(*sound, time); });
    }
    std::erase_if(p.voices, [&](auto& entry) { return p.expired(*entry.second, time); });
    p.startCued();
}

void MiniAudio::reload(const std::filesystem::path& file) {
    auto& p = *impl_;
    if (p.cached.erase(file.string()) > 0) {
        ma_resource_manager_unregister_file(ma_engine_get_resource_manager(&p.engine), file.string().c_str());
    }
    p.lengths.erase(file.string());
    for (auto& entry : p.layers) {
        auto& layer = entry.second;
        std::error_code ec;
        if (!layer.file.empty() && std::filesystem::equivalent(layer.file, file, ec)) {
            const auto options = layer.options;
            layer.file.clear();
            playMusic(file, options);
        }
    }
}

void MiniAudio::setPaused(bool paused) {
    auto& p = *impl_;
    if (p.paused == paused) {
        return;
    }
    p.paused = paused;
    p.lastUpdate = Clock::now();
    if (!p.deviceOpen) {
        return;
    }
    if (paused) {
        ma_device_stop(&p.device);
    } else if (ma_device_start(&p.device) != MA_SUCCESS) {
        p.closeDevice();
    }
}

void MiniAudio::mix(std::uint32_t frames, std::vector<float>* output) {
    if (!impl_->ready) {
        return;
    }
    std::vector<float> scratch(std::size_t(1024) * Channels);
    while (frames > 0) {
        const auto count = std::min<std::uint32_t>(frames, 1024);
        ma_engine_read_pcm_frames(&impl_->engine, scratch.data(), count, nullptr);
        if (output) {
            const auto end = scratch.begin() + std::ptrdiff_t(count) * Channels;
            output->insert(output->end(), scratch.begin(), end);
        }
        frames -= count;
    }
}

float MiniAudio::musicGain(const std::string& layer) const {
    const auto* sound = impl_->currentMusic(layer);
    return sound ? ma_sound_get_volume(&sound->handle) * ma_sound_get_current_fade_volume(&sound->handle)
                 : 0.0f;
}

std::size_t MiniAudio::activeSounds() const {
    std::size_t count = impl_->voices.size();
    for (const auto& entry : impl_->layers) {
        count += entry.second.sounds.size();
    }
    return count;
}

Spatial MiniAudio::voiceMix(int voice) const {
    const auto* sound = impl_->voice(voice);
    return sound ? Spatial{ma_sound_get_volume(&sound->handle), ma_sound_get_pan(&sound->handle)}
                 : Spatial{0, 0};
}

float MiniAudio::duckGain(const std::string& bus) const {
    const auto found = impl_->buses.find(bus);
    return found == impl_->buses.end() ? 1.0f : found->second->dsp.duckGain();
}
} // namespace fe
