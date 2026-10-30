#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fe {
// Pitch shift that keeps time. A delay line is read by two grains that move
// through it at `ratio` samples per output frame, each windowed by a raised
// cosine and half a grain apart, so their windows sum to one; a grain that
// has run its length restarts near the write head, at the offset (searched
// within a few milliseconds, WSOLA-style) whose waveform best matches what
// the other grain is reading, so the splice stays coherent. Output frames
// equal input frames exactly: a 440 Hz tone at 2 plays at 880 Hz for the
// same duration. Ratios outside 0.5..2 are not shifted (the caller falls back
// to varispeed); 1 copies the input through with no latency.
//
// Interleaved stereo; one instance per sound, on the mixer thread.
// `setRatio` may be called from another thread.
class PitchShift {
public:
    static constexpr float MinRatio = 0.5f, MaxRatio = 2.0f;
    static constexpr unsigned Channels = 2;

    explicit PitchShift(unsigned sampleRate)
        : grain_(std::max(64u, sampleRate / 25)), search_(std::max(8u, sampleRate / 200)) {
        std::uint32_t size = 256;
        while (size < 4 * grain_ + 4 * search_) {
            size *= 2;
        }
        mask_ = size - 1;
        buffer_.assign(static_cast<std::size_t>(size) * Channels, 0.0f);
    }

    [[nodiscard]] static bool supports(float ratio) {
        return std::isfinite(ratio) && ratio >= MinRatio && ratio <= MaxRatio;
    }
    // The pitch multiplier (2 = an octave up). Unsupported values pass the
    // input through, as 1 does.
    void setRatio(float ratio) { ratio_.store(supports(ratio) ? ratio : 1.0f); }
    [[nodiscard]] float ratio() const { return ratio_.load(); }
    // Output frames behind the input at a ratio: the delay a restarted grain
    // needs so it never overtakes the write head while it plays.
    [[nodiscard]] std::uint32_t latency(float ratio) const {
        const float ahead = std::max(0.0f, ratio - 1.0f) * static_cast<float>(grain_);
        return static_cast<std::uint32_t>(std::ceil(ahead)) + search_ + 4;
    }

    // `in` and `out` may be the same buffer.
    void process(const float* in, float* out, std::uint32_t frames) {
        const float ratio = ratio_.load();
        if (ratio == 1.0f) {
            // Keep the delay line current so a later shift has history to read.
            for (std::uint32_t i = 0; i < frames; ++i) {
                push(in + i * Channels);
            }
            if (out != in) {
                std::memcpy(out, in, static_cast<std::size_t>(frames) * Channels * sizeof(float));
            }
            shifting_ = false;
            return;
        }
        if (!shifting_) {
            seat(ratio);
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            push(in + i * Channels);
            std::array<float, Channels> sum{};
            for (auto& head : heads_) {
                if (head.phase >= grain_) {
                    restart(head, other(head), ratio);
                }
                const float gain = window(head.phase);
                read(head.position, gain, sum);
                head.position += ratio;
                ++head.phase;
            }
            for (unsigned c = 0; c < Channels; ++c) {
                out[i * Channels + c] = sum[c];
            }
        }
    }

private:
    struct Head {
        double position = 0; // absolute input frame, fractional
        std::uint32_t phase = 0;
    };

    void push(const float* frame) {
        const auto index = (written_ & mask_) * Channels;
        for (unsigned c = 0; c < Channels; ++c) {
            buffer_[index + c] = frame[c];
        }
        ++written_;
    }

    [[nodiscard]] float window(std::uint32_t phase) const {
        const float s = std::sin(3.14159265f * static_cast<float>(phase) / static_cast<float>(grain_));
        return s * s;
    }

    // Reads at a fractional frame, linearly interpolated, never past the
    // newest written frame nor before the oldest one still in the line;
    // frames before the first one written are silence.
    void read(double position, float gain, std::array<float, Channels>& sum) const {
        if (position < 0 || written_ < 2) {
            return;
        }
        const double newest = static_cast<double>(written_) - 2.0;
        const double oldest = written_ > mask_ ? static_cast<double>(written_ - mask_) : 0.0;
        position = std::clamp(position, oldest, std::max(oldest, newest));
        const auto whole = static_cast<std::uint64_t>(std::floor(position));
        const float fraction = static_cast<float>(position - static_cast<double>(whole));
        const auto a = (whole & mask_) * Channels, b = ((whole + 1) & mask_) * Channels;
        for (unsigned c = 0; c < Channels; ++c) {
            sum[c] += gain * (buffer_[a + c] + (buffer_[b + c] - buffer_[a + c]) * fraction);
        }
    }

    [[nodiscard]] float mono(std::uint64_t frame) const {
        const auto index = (frame & mask_) * Channels;
        float value = 0;
        for (unsigned c = 0; c < Channels; ++c) {
            value += buffer_[index + c];
        }
        return value;
    }

    [[nodiscard]] Head& other(Head& head) { return &head == &heads_[0] ? heads_[1] : heads_[0]; }

    // Entering a shift: one grain starts now, the other is half way through.
    void seat(float ratio) {
        const double start = static_cast<double>(written_) - latency(ratio);
        heads_[0] = {start, 0};
        heads_[1] = {start + (ratio - 1.0) * grain_ / 2.0, grain_ / 2};
        shifting_ = true;
    }

    // Restarts a finished grain behind the write head, where its next
    // samples line up best with what the other grain reads now.
    void restart(Head& head, const Head& reference, float ratio) {
        const double nominal = static_cast<double>(written_) - latency(ratio);
        const auto taps = std::min<std::uint32_t>(96, grain_ / 4);
        const double stride = ratio;
        const auto base = static_cast<std::uint64_t>(std::max(0.0, std::floor(reference.position)));
        double best = -1e30;
        int bestOffset = 0;
        const auto earliest = static_cast<double>(written_ > mask_ ? written_ - mask_ : 0) + 1.0;
        for (int offset = -static_cast<int>(search_); offset <= static_cast<int>(search_); ++offset) {
            const double candidate = nominal + offset;
            if (candidate < earliest) {
                continue;
            }
            const auto from = static_cast<std::uint64_t>(candidate);
            double score = 0;
            for (std::uint32_t tap = 0; tap < taps; ++tap) {
                const auto step = static_cast<std::uint64_t>(std::llround(tap * stride));
                score += static_cast<double>(mono(from + step)) * static_cast<double>(mono(base + step));
            }
            if (score > best) {
                best = score;
                bestOffset = offset;
            }
        }
        head.position = std::max(earliest, nominal + bestOffset);
        head.phase = 0;
    }

    const std::uint32_t grain_, search_;
    std::uint32_t mask_ = 0;
    std::vector<float> buffer_;
    std::uint64_t written_ = 0;
    std::array<Head, 2> heads_{};
    bool shifting_ = false;
    std::atomic<float> ratio_{1.0f};
};
} // namespace fe
