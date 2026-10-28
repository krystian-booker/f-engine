#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fe {
// Bounds catch-up work after a debugger pause without changing the tick size.
class FixedStep {
public:
    explicit FixedStep(double hz = 60.0, int maxSteps = 8) : step_(1.0 / hz), maxSteps_(maxSteps) {
        if (!std::isfinite(hz) || hz <= 0 || maxSteps <= 0)
            throw std::invalid_argument("Invalid fixed-step settings");
    }
    template<class Update> int advance(double elapsed, Update&& update) {
        if (!std::isfinite(elapsed) || elapsed < 0)
            elapsed = 0;
        accumulator_ += std::min(elapsed, step_ * maxSteps_);
        int ticks = 0;
        while (accumulator_ + 1e-12 >= step_ && ticks < maxSteps_) {
            update(step_);
            accumulator_ = std::max(0.0, accumulator_ - step_);
            ++ticks;
        }
        return ticks;
    }
    [[nodiscard]] double alpha() const { return std::clamp(accumulator_ / step_, 0.0, 1.0); }
    [[nodiscard]] double interval() const { return step_; }

private:
    double step_;
    double accumulator_ = 0;
    int maxSteps_;
};
} // namespace fe
