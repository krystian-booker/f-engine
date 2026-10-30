#pragma once
#include <cmath>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace fe {
enum class FullscreenMode { Window, Borderless, Exclusive };
enum class VsyncMode { Off, On, Adaptive };

struct DisplayMode {
    int width = 0, height = 0;
    float refresh = 0; // Hz, 0 when unknown
};

struct Monitor {
    std::string name;
    DisplayMode desktop;
    std::vector<DisplayMode> modes; // exclusive fullscreen modes, largest first
};

// The window and the monitors, as the runtime last reported them (headless
// runs report one monitor the size of the configured window).
struct DisplayState {
    FullscreenMode fullscreen = FullscreenMode::Window;
    int width = 1280, height = 720; // window size in screen units, or the exclusive mode
    VsyncMode vsync = VsyncMode::On;
    int monitor = 1; // 1-based index into monitors
    std::vector<Monitor> monitors;
    // The back buffer is a high dynamic range format (game.json hdr_output or
    // --hdr, and the backend provided one); false headless.
    bool hdr = false;
};

// A change scripts asked for; unset fields keep their value.
struct DisplayRequest {
    std::optional<FullscreenMode> fullscreen;
    std::optional<int> width, height, monitor;
    std::optional<VsyncMode> vsync;

    void merge(const DisplayRequest& later) {
        fullscreen = later.fullscreen ? later.fullscreen : fullscreen;
        width = later.width ? later.width : width;
        height = later.height ? later.height : height;
        monitor = later.monitor ? later.monitor : monitor;
        vsync = later.vsync ? later.vsync : vsync;
    }
    void applyTo(DisplayState& state) const {
        state.fullscreen = fullscreen.value_or(state.fullscreen);
        state.width = width.value_or(state.width);
        state.height = height.value_or(state.height);
        state.monitor = monitor.value_or(state.monitor);
        state.vsync = vsync.value_or(state.vsync);
    }
};

// Adaptive vsync: synchronised while frames keep up with the display, torn
// rather than halved when they don't. Fed the duration of every presented
// frame, it says whether the next frames should wait for vertical blank.
class AdaptiveVsync {
public:
    // `synced`: whether the measured frame waited for vertical blank.
    bool update(double frameSeconds, double refreshHz, bool synced) {
        const double interval = 1.0 / (refreshHz > 0 ? refreshHz : 60.0);
        frames_.push_back(frameSeconds);
        if (frames_.size() > Window) {
            frames_.pop_front();
        }
        if (frames_.size() < Window) {
            return synced;
        }
        double total = 0;
        for (const double frame : frames_) {
            total += frame;
        }
        const double average = total / static_cast<double>(frames_.size());
        // Synced frames that miss a blank wait for the next one, so the average
        // climbs towards two intervals; unsynced frames show the real cost.
        const bool next = synced ? average < interval * 1.4 : average < interval * 0.9;
        if (next != synced) {
            frames_.clear();
        }
        return next;
    }
    void reset() { frames_.clear(); }

private:
    static constexpr std::size_t Window = 30;
    std::deque<double> frames_;
};

// Frame cap for when vsync is off (game.json "max_fps"): says how long to
// wait before starting each frame so that frames start one interval apart.
// Deadlines advance by whole intervals, so timing jitter does not accumulate;
// a frame that arrives more than an interval late starts at once and the
// schedule restarts from it.
class FrameCap {
public:
    explicit FrameCap(int maxFps = 0) { setMaxFps(maxFps); }
    void setMaxFps(int maxFps) {
        const double interval = maxFps > 0 ? 1.0 / maxFps : 0.0;
        if (interval != interval_) {
            interval_ = interval;
            next_ = -1;
        }
    }
    [[nodiscard]] int maxFps() const {
        return interval_ > 0 ? static_cast<int>(std::lround(1 / interval_)) : 0;
    }
    [[nodiscard]] double interval() const { return interval_; }
    // `now` in seconds on any monotonic clock; returns the seconds to wait
    // before this frame may start (0 when uncapped or already due).
    [[nodiscard]] double wait(double now) {
        if (interval_ <= 0) {
            return 0;
        }
        if (next_ < 0 || now - next_ > interval_) {
            next_ = now + interval_;
            return 0;
        }
        const double wait = next_ - now;
        next_ += interval_;
        return wait > 0 ? wait : 0;
    }
    void reset() { next_ = -1; }

private:
    double interval_ = 0, next_ = -1;
};
} // namespace fe
