#pragma once
#include "engine/core/Json.h"
#include "engine/services/InputRecorder.h"
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fe {
class Engine;
struct Image;

// Plays a scripted session: tick-stamped commands that feed input, run Lua,
// assert Lua expressions, wait for conditions, resize, capture and compare
// screenshots. Used for end-to-end tests of whole games. A replay file is an
// array of commands, or {"seed": n, "commands": [...]} (what --record writes).
//
//   [{"tick": 10, "input": [["key:Right", 1]]},
//    {"tick": 40, "input": {"key:Right": 0}},
//    {"tick": 41, "wait_for": "require('game.field').world ~= nil", "timeout": 600},
//    {"tick": 42, "assert": "require('game.state').flags.met_elder"},
//    {"tick": 50, "mouse": [120, 80]}, {"tick": 51, "wheel": -1},
//    {"tick": 55, "text": "Aria"}, {"tick": 56, "text": "かな", "cursor": 2},
//    {"tick": 58, "pads": [{"connected": true, "pad": 1, "name": "Virtual", "type": "xbox",
//                           "rumble": true, "trigger_rumble": false}]},
//    {"tick": 60, "capture": "out/town.png", "compare": "reference/{backend}/town.png", "tolerance": 0.02}]
//
// `input` is an ordered list of [name, value] pairs, fed in that order (the
// recorder writes this form, so a tick's keyboard and gamepad events replay in
// the order they happened); an object {name: value} is also accepted and fed
// in key order. `text` is committed text, or with `cursor` the IME's pre-edit
// string (as `fe.input.feed_text`). `pads` connects or disconnects gamepads
// (as `fe.input.feed_pad`), so fe.input.pads(), device_type(pad) and
// pad_events() replay; a disconnect needs only `pad`. Ticks after a wait_for
// are relative to when its condition became true; a wait_for that times out
// fails, and the command's other fields still run.
// `compare` is relative to the replay file, and "{backend}" in it stands for
// the renderer backend's folder (see backendFolder), so each backend keeps its
// own references. Every capture must look like a rendered frame (see
// blankFrameProblem); one with a reference for the running backend must also
// match it (see compareCapture). A missing reference skips only the
// comparison. Captures and comparisons need the windowed runtime and are
// skipped by `fengine_headless --replay`.
class ReplayHarness {
public:
    struct Effects {
        std::optional<std::filesystem::path> capture, reference;
        double tolerance = 0.02;
        std::optional<std::pair<int, int>> resize;
    };
    explicit ReplayHarness(const std::filesystem::path& file);
    // Runs commands due at this tick (before the engine ticks).
    Effects apply(std::uint64_t tick, Engine& engine);
    [[nodiscard]] bool finished() const { return next_ >= commands_.size(); }
    // Every command ran and the game settled for 30 more ticks.
    [[nodiscard]] bool done(std::uint64_t tick) const { return finished() && tick > lastTick() + 30; }
    [[nodiscard]] const std::vector<std::string>& failures() const { return failures_; }
    [[nodiscard]] std::uint64_t lastTick() const;
    [[nodiscard]] std::optional<std::uint64_t> seed() const { return seed_; }
    void fail(std::string message) { failures_.push_back(std::move(message)); }
    // Prints failures, unfinished commands and script errors to stderr;
    // returns the process exit code.
    int report(Engine& engine) const;

private:
    bool evaluate(Engine& engine, const std::string& expression, std::string& detail);
    static void feedInputs(Engine& engine, const Json& inputs);
    Json commands_;
    std::filesystem::path folder_;
    std::optional<std::uint64_t> seed_;
    std::size_t next_ = 0;
    std::uint64_t offset_ = 0, waitStarted_ = 0;
    bool waiting_ = false;
    std::vector<std::string> failures_;
};

// Mean and largest per-channel difference (0..1) between two RGBA images;
// images of different sizes differ by 1.
struct ImageDifference {
    double mean = 0, max = 0;
};
[[nodiscard]] ImageDifference compareImages(const Image& actual, const Image& reference);

// Reference images are stored this many pixels wide, the height following
// the capture's aspect ratio. Comparing at this size makes references
// independent of the window's pixel density and averages out small
// rasterisation differences between GPUs and drivers of one backend.
inline constexpr int ReferenceWidth = 320;
// The image box-filtered to `width` pixels across (height by aspect ratio).
[[nodiscard]] Image resizedTo(const Image& image, int width);
// compareImages of a capture, resized to the reference's width, and the
// reference; a capture of another aspect ratio differs by 1.
[[nodiscard]] ImageDifference compareCapture(const Image& capture, const Image& reference);
// Why a capture cannot be a rendered frame, or an empty string. Its
// brightness histogram (64 levels of luma) must not have one level covering
// more than 95% of the pixels, and at least 4 levels must each hold 0.5% of
// them. A black, cleared or single-colour frame fails; a dark or sepia scene
// passes.
[[nodiscard]] std::string blankFrameProblem(const Image& image);
// The folder name of a renderer backend for "{backend}" in replay compare
// paths: bgfx's renderer name in lower case, letters and digits only
// ("Metal" -> "metal", "Vulkan" -> "vulkan", "Direct3D 11" -> "direct3d11",
// "OpenGL 2.1" -> "opengl21").
[[nodiscard]] std::string backendFolder(std::string_view rendererName);
[[nodiscard]] std::filesystem::path referenceFor(const std::filesystem::path& compare,
                                                 std::string_view rendererName);

// Records the raw input fed to the engine as a replay, so a played session
// can be replayed (`fengine <game> --record out.json`, then `--replay`).
// Attach it with InputState::setRecorder. A tick's inputs become one command
// holding an ordered list of [name, value] pairs; its gamepad connections and
// disconnections one `pads` command, written before the inputs so a pad is
// known when its buttons replay.
class ReplayRecorder final : public InputRecorder {
public:
    explicit ReplayRecorder(std::uint64_t seed) : seed_(seed) {}
    void input(const std::string& name, float value) override;
    void mouse(glm::vec2 canvasPosition) override;
    void wheel(float delta) override;
    void text(std::string_view utf8) override;
    void composition(const std::string& text, int cursor) override;
    void pad(const InputState::PadEvent& event) override;
    // Stamps everything recorded since the last call with the tick about to run.
    void stamp(std::uint64_t tick);
    // The replay, ending with an empty command at `lastTick` so the replay
    // runs as long as the session did.
    [[nodiscard]] Json toJson(std::uint64_t lastTick) const;

private:
    std::uint64_t seed_;
    Json commands_ = Json::array();
    Json pendingInput_ = Json::array(); // [name, value] pairs in feed order
    Json pendingPads_ = Json::array();  // pad events in arrival order
    std::optional<glm::vec2> pendingMouse_;
    float pendingWheel_ = 0;
    std::string pendingText_;
    std::optional<std::pair<std::string, int>> pendingComposition_;
};

// `fengine_headless <game> --replay <file>`: runs a replay with no window,
// GPU or audio device. Returns the process exit code.
int runHeadlessReplay(const std::filesystem::path& game, const std::filesystem::path& builtin,
                      const std::filesystem::path& replay, std::filesystem::path userData,
                      std::optional<std::uint64_t> seed);
} // namespace fe
