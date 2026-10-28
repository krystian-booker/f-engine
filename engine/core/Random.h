#pragma once
#include <cstdint>

namespace fe {
// PCG32: small, fast and identical on every platform, so seeded streams
// replay the same way everywhere.
class Random {
public:
    explicit Random(std::uint64_t seed = 0x853c49e6748fea9bULL,
                    std::uint64_t stream = 0xda3e39cb94b95bdbULL) {
        reseed(seed, stream);
    }
    void reseed(std::uint64_t seed, std::uint64_t stream = 0xda3e39cb94b95bdbULL) {
        state_ = 0;
        increment_ = (stream << 1u) | 1u;
        next();
        state_ += seed;
        next();
    }
    std::uint32_t next() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + increment_;
        const auto shifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rotation = static_cast<std::uint32_t>(old >> 59u);
        return (shifted >> rotation) | (shifted << ((~rotation + 1u) & 31u));
    }
    // Uniform in [0, 1).
    double unit() { return next() * (1.0 / 4294967296.0); }
    // Uniform integer in [low, high].
    std::int64_t range(std::int64_t low, std::int64_t high) {
        if (high <= low) {
            return low;
        }
        const auto span = static_cast<std::uint64_t>(high - low) + 1;
        return low + static_cast<std::int64_t>(unit() * static_cast<double>(span));
    }
    [[nodiscard]] std::uint64_t state() const { return state_; }

private:
    std::uint64_t state_ = 0;
    std::uint64_t increment_ = 0;
};
} // namespace fe
