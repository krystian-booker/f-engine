#pragma once
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
} // namespace fe
