#pragma once
#include "engine/services/Audio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace fe {
// Per-bus effects run on the mixer thread: a resonance-free low-pass
// (Simper's state-variable filter), a Freeverb-style reverb, and sidechain
// ducking. Settings come from the game thread and glide sample by sample, so
// changing them every tick never clicks.
class BusDsp {
public:
    explicit BusDsp(unsigned sampleRate) : rate_(static_cast<float>(sampleRate)), reverb_(sampleRate) {
        cutoff_.value = cutoff_.target = std::log2(MaxCutoff);
        room_.value = room_.target = 0.7f;
        damping_.value = damping_.target = 0.5f;
    }

    void configure(const BusEffect& effect) {
        std::lock_guard lock(mutex_);
        settings_ = effect;
        ++generation_;
    }
    [[nodiscard]] BusEffect settings() const {
        std::lock_guard lock(mutex_);
        return settings_;
    }
    void duck(const BusDsp* key, float amount, double attack, double release) {
        duckAmount_ = std::clamp(amount, 0.0f, 1.0f);
        attackFrames_ = std::max(1.0f, static_cast<float>(attack) * rate_);
        releaseFrames_ = std::max(1.0f, static_cast<float>(release) * rate_);
        key_ = key;
    }
    [[nodiscard]] float duckGain() const { return gain_.load(); }
    // The bus carried sound within the last HoldSeconds (it keys ducking).
    [[nodiscard]] bool audible() const { return silentFrames_.load() < HoldSeconds * rate_; }

    // Interleaved stereo in, interleaved stereo out (may alias).
    void process(const float* in, float* out, std::uint32_t frames) {
        pullSettings();
        float peak = 0;
        for (std::uint32_t i = 0; i < frames * 2; ++i) {
            peak = std::max(peak, std::abs(in[i]));
        }
        const auto silent = silentFrames_.load();
        silentFrames_ = peak > SilenceThreshold ? 0u : std::min<std::uint32_t>(silent + frames, 1u << 30);

        const auto* key = key_.load();
        const float amount = duckAmount_.load();
        const float target = key && key->audible() ? 1 - amount : 1;
        float gain = gain_.load();
        const float depth = std::max(amount, 1 - gain);
        const float down = depth / attackFrames_.load(), up = depth / releaseFrames_.load();

        for (std::uint32_t offset = 0; offset < frames;) {
            const auto count = std::min<std::uint32_t>(Chunk, frames - offset);
            cutoff_.advance(count);
            mix_.advance(count);
            room_.advance(count);
            damping_.advance(count);
            const bool filter = lowpassOn_ || cutoff_.value < std::log2(MaxCutoff) - 0.01f;
            if (!filter && filtering_) {
                svf_ = {};
            }
            filtering_ = filter;
            float a1 = 1, a2 = 0, a3 = 0;
            if (filter) {
                const float g = std::tan(3.14159265f * std::exp2(cutoff_.value) / rate_);
                const float k = 1.41421356f; // Butterworth Q
                a1 = 1 / (1 + g * (g + k));
                a2 = g * a1;
                a3 = g * a2;
            }
            const float wet = mix_.value;
            const bool reverb = wet > 0 || mix_.remaining > 0;
            if (reverb && !reverbActive_) {
                reverb_.clear();
            }
            reverbActive_ = reverb;
            reverb_.set(room_.value, damping_.value);
            for (std::uint32_t i = offset; i < offset + count; ++i) {
                float l = in[i * 2], r = in[i * 2 + 1];
                if (filter) {
                    l = svf_[0].run(l, a1, a2, a3);
                    r = svf_[1].run(r, a1, a2, a3);
                }
                if (reverb) {
                    const auto [wl, wr] = reverb_.run(l, r);
                    l += (wl - l) * wet;
                    r += (wr - r) * wet;
                }
                gain = gain > target ? std::max(target, gain - down) : std::min(target, gain + up);
                out[i * 2] = l * gain;
                out[i * 2 + 1] = r * gain;
            }
            offset += count;
        }
        gain_ = gain;
    }

private:
    static constexpr float MaxCutoff = 20000, MinCutoff = 20, SilenceThreshold = 1e-4f, HoldSeconds = 0.05f;
    static constexpr std::uint32_t Chunk = 32;
    static constexpr double MinGlide = 0.005;

    struct Glide {
        float value = 0, target = 0, step = 0;
        std::uint32_t remaining = 0;
        void to(float goal, std::uint32_t frames) {
            target = goal;
            remaining = std::max(1u, frames);
            step = (goal - value) / static_cast<float>(remaining);
        }
        void advance(std::uint32_t frames) {
            if (frames >= remaining) {
                value = target;
                remaining = 0;
            } else {
                value += step * static_cast<float>(frames);
                remaining -= frames;
            }
        }
    };

    // Topology-preserving state-variable low-pass, stable under modulation.
    struct Svf {
        float ic1 = 0, ic2 = 0;
        float run(float v0, float a1, float a2, float a3) {
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            return v2;
        }
    };

    // Freeverb: eight damped combs into four allpasses per channel, with the
    // original 44.1 kHz tunings scaled to the mixer rate.
    class Reverb {
    public:
        explicit Reverb(unsigned sampleRate) {
            constexpr std::array<int, 8> combs{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
            constexpr std::array<int, 4> allpasses{556, 441, 341, 225};
            const double scale = sampleRate / 44100.0;
            for (int channel = 0; channel < 2; ++channel) {
                const int spread = channel * 23;
                const auto size = [&](int tuning) {
                    return static_cast<std::size_t>((tuning + spread) * scale);
                };
                for (std::size_t i = 0; i < combs.size(); ++i) {
                    combs_[channel][i].buffer.assign(size(combs[i]), 0.0f);
                }
                for (std::size_t i = 0; i < allpasses.size(); ++i) {
                    allpasses_[channel][i].buffer.assign(size(allpasses[i]), 0.0f);
                }
            }
        }
        void clear() {
            for (auto& channel : combs_) {
                for (auto& comb : channel) {
                    std::fill(comb.buffer.begin(), comb.buffer.end(), 0.0f);
                    comb.store = 0;
                }
            }
            for (auto& channel : allpasses_) {
                for (auto& allpass : channel) {
                    std::fill(allpass.buffer.begin(), allpass.buffer.end(), 0.0f);
                }
            }
        }
        void set(float room, float damping) {
            feedback_ = std::clamp(room, 0.0f, 1.0f) * 0.28f + 0.7f;
            damp_ = std::clamp(damping, 0.0f, 1.0f) * 0.4f;
        }
        std::array<float, 2> run(float l, float r) {
            const float input = (l + r) * 0.015f;
            std::array<float, 2> out{};
            for (int channel = 0; channel < 2; ++channel) {
                for (auto& comb : combs_[channel]) {
                    out[channel] += comb.run(input, feedback_, damp_);
                }
                for (auto& allpass : allpasses_[channel]) {
                    out[channel] = allpass.run(out[channel]);
                }
                out[channel] *= 3;
            }
            return out;
        }

    private:
        struct Comb {
            std::vector<float> buffer;
            std::size_t index = 0;
            float store = 0;
            float run(float in, float feedback, float damp) {
                const float out = buffer[index];
                store = out * (1 - damp) + store * damp;
                if (std::abs(store) < 1e-20f) {
                    store = 0;
                }
                buffer[index] = in + store * feedback;
                index = index + 1 == buffer.size() ? 0 : index + 1;
                return out;
            }
        };
        struct Allpass {
            std::vector<float> buffer;
            std::size_t index = 0;
            float run(float in) {
                const float stored = buffer[index];
                buffer[index] = in + stored * 0.5f;
                index = index + 1 == buffer.size() ? 0 : index + 1;
                return stored - in;
            }
        };
        std::array<std::array<Comb, 8>, 2> combs_;
        std::array<std::array<Allpass, 4>, 2> allpasses_;
        float feedback_ = 0.896f, damp_ = 0.2f;
    };

    void pullSettings() {
        std::optional<BusEffect> changed;
        if (mutex_.try_lock()) {
            if (generation_ != seen_) {
                seen_ = generation_;
                changed = settings_;
            }
            mutex_.unlock();
        }
        if (!changed) {
            return;
        }
        const auto frames = static_cast<std::uint32_t>(std::max(MinGlide, changed->fade) * rate_);
        lowpassOn_ = changed->lowpass > 0;
        const float cutoff = lowpassOn_ ? std::clamp(changed->lowpass, MinCutoff, MaxCutoff) : MaxCutoff;
        cutoff_.to(std::log2(cutoff), frames);
        mix_.to(std::clamp(changed->reverb, 0.0f, 1.0f), frames);
        room_.to(std::clamp(changed->room, 0.0f, 1.0f), frames);
        damping_.to(std::clamp(changed->damping, 0.0f, 1.0f), frames);
    }

    const float rate_;
    mutable std::mutex mutex_;
    BusEffect settings_;
    std::uint32_t generation_ = 0;
    std::atomic<const BusDsp*> key_{nullptr};
    std::atomic<float> duckAmount_{0}, attackFrames_{1}, releaseFrames_{1}, gain_{1};
    std::atomic<std::uint32_t> silentFrames_{1u << 30};

    // Mixer thread state.
    std::uint32_t seen_ = 0;
    Glide cutoff_, mix_, room_, damping_;
    bool lowpassOn_ = false, filtering_ = false, reverbActive_ = false;
    std::array<Svf, 2> svf_{};
    Reverb reverb_;
};
} // namespace fe
