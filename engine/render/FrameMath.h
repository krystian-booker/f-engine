#pragma once
#include "engine/world/Camera.h"
#include "engine/world/MapData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// CPU-side rules of the frame renderer that need no GPU: bloom chain sizes,
// particle near fade, circle of confusion, chromatic aberration, uniform
// packing, target pooling, draw camera interpolation, freeze-frame lifetime,
// custom shader parameters, instance grouping, glyph effects and thumbnail
// scaling. The shaders mirror the formulas here.
namespace fe {
// Bloom mip chain: level i is the scene halved i + 1 times; the chain stops
// before a side would drop below 2 pixels.
inline std::vector<glm::ivec2> bloomMipSizes(glm::ivec2 scene, int maxLevels) {
    std::vector<glm::ivec2> sizes;
    glm::ivec2 size = scene;
    for (int level = 0; level < maxLevels; ++level) {
        size /= 2;
        if (size.x < 2 || size.y < 2) {
            break;
        }
        sizes.push_back(size);
    }
    if (sizes.empty()) {
        sizes.push_back(glm::max(glm::ivec2(1), scene / 2));
    }
    return sizes;
}

// The upsample pass adds each coarser level scaled by `radius`, so level k
// ends up weighted radius^k; dividing by the sum keeps bloom energy constant
// when the radius or level count changes.
inline float bloomNormalisation(int levels, float radius) {
    float sum = 0, weight = 1;
    for (int level = 0; level < levels; ++level) {
        sum += weight;
        weight *= radius;
    }
    return std::max(sum, 1e-4f);
}

// Soft-knee bloom threshold: the fraction of a colour with brightest channel
// `brightness` that passes. Below threshold - knee nothing passes; a
// quadratic curve joins it to the linear part above threshold + knee.
inline float bloomPass(float brightness, float threshold, float knee) {
    const float soft = std::clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    const float curve = soft * soft / (4.0f * knee + 1e-4f);
    return std::max(curve, brightness - threshold) / std::max(brightness, 1e-4f);
}

// Opacity of a particle `depth` units in front of a camera that is
// `focusDistance` from its focus: particles fade out between 30% and 10% of
// that distance, so weather drifting past the lens doesn't fill the screen
// with a blurred blob.
inline float particleNearFade(float depth, float focusDistance) {
    const float gone = focusDistance * 0.1f, full = focusDistance * 0.3f;
    return std::clamp((depth - gone) / std::max(full - gone, 1e-4f), 0.0f, 1.0f);
}

// View-space distance of a depth-buffer value. `homogeneous` backends
// (OpenGL) store window depth for an NDC range of -1..1; both branches are the
// same function of window depth (a shader without the flag, like fs_water.sc,
// is right on every backend).
inline float linearDepth(float device, float nearPlane, float farPlane, bool homogeneous) {
    if (homogeneous) {
        const float ndc = device * 2.0f - 1.0f;
        return 2.0f * nearPlane * farPlane /
               std::max(farPlane + nearPlane - ndc * (farPlane - nearPlane), 1e-4f);
    }
    return nearPlane * farPlane / std::max(farPlane - device * (farPlane - nearPlane), 1e-4f);
}

// Chromatic aberration, one rule for the post pass (`post.chromatic_aberration`,
// one world's scene) and the screen pass (`fe.screen` `chromatic`, worlds and
// UI): with amount a, red is read a * 3% of the way further from the screen
// centre and blue as much nearer. chromatic.sh mirrors it.
inline glm::vec2 chromaticShift(glm::vec2 uv, float amount) {
    return (uv - 0.5f) * (amount * 0.03f);
}

// What the composite pass needs besides the world's post settings.
struct PostFrame {
    float bloomStrength = 0; // normalised; 0 = no bloom
    bool depthOfField = false, distorted = false;
    float blurRadius = 0;       // widest depth-of-field blur, scene pixels
    glm::vec4 tint{1, 1, 1, 0}; // fe.screen tint: rgb, amount
};

// u_post of fs_post.sc:
// [0] bloom strength, exposure, chromatic aberration, distortion scale (0 = none drawn)
// [1] depth of field, vignette, colour grade, ambient occlusion (0/1)
// [2] vignette strength, LUT blend, ACES tone mapping (0/1), widest blur
// [3] tint rgb, amount
inline std::array<glm::vec4, 4> packPost(const PostSettings& s, const PostFrame& frame) {
    const auto flag = [](bool on) {
        return on ? 1.0f : 0.0f;
    };
    return {glm::vec4(frame.bloomStrength, s.exposure, s.chromaticAberration,
                      frame.distorted ? s.distortion : 0.0f),
            glm::vec4(flag(frame.depthOfField), flag(s.vignette), flag(s.colorGrade), flag(s.ssao)),
            glm::vec4(s.vignetteStrength, s.lutBlendTarget.empty() ? 0.0f : s.lutBlend,
                      flag(s.tonemap == "aces"), frame.blurRadius),
            frame.tint};
}

// What water needs besides the environment's water settings.
struct WaterFrame {
    float time = 0, nearPlane = 0.1f, farPlane = 100;
    glm::vec3 horizon{0};          // the sky's horizon colour
    bool sceneCopy = false;        // the opaque scene was copied for refraction and reflection
    bool originBottomLeft = false; // bgfx caps
};

// u_water of fs_water.sc: time, wave scale, wave speed, fresnel; deep colour,
// depth falloff; horizon, specular; near, far, depth tint (0/1), unused;
// reflection, refraction, scene copy (0/1), origin bottom-left (0/1).
inline std::array<glm::vec4, 5> packWater(const WaterSettings& w, const WaterFrame& frame) {
    const auto flag = [](bool on) {
        return on ? 1.0f : 0.0f;
    };
    return {glm::vec4(frame.time, w.waveScale, w.waveSpeed, w.fresnel),
            glm::vec4(w.deepColor, w.depthFalloff), glm::vec4(frame.horizon, w.specular),
            glm::vec4(frame.nearPlane, frame.farPlane, flag(frame.sceneCopy && w.depthFalloff > 0), 0),
            glm::vec4(w.reflection, w.refraction, flag(frame.sceneCopy), flag(frame.originBottomLeft))};
}

enum class FocusMode { Depth, Tilt, Both };

struct FocusSettings {
    FocusMode mode = FocusMode::Both;
    float distance = 17; // view depth in focus
    float aperture = 1.5f;
    float line = 0.55f, range = 0.25f; // tilt-shift: sharp band centre and half height (0..1 down the screen)
};

// Signed circle of confusion, -1..1: negative in front of the focus, positive
// behind it, 0 sharp. Depth mode grows with relative distance from the focus;
// tilt-shift mode with distance from the sharp band (above it counts as far,
// below as near). Both takes the stronger one with its own sign.
inline float circleOfConfusion(float viewDepth, float screenY, const FocusSettings& focus) {
    const float depth =
        std::clamp(focus.aperture * (viewDepth - focus.distance) / std::max(viewDepth, 0.1f), -1.0f, 1.0f);
    const float band = std::clamp((std::abs(screenY - focus.line) - focus.range) * 5.0f, 0.0f, 1.0f);
    const float tilt = screenY < focus.line ? band : -band;
    switch (focus.mode) {
    case FocusMode::Depth:
        return depth;
    case FocusMode::Tilt:
        return tilt;
    case FocusMode::Both:
        break;
    }
    return std::abs(depth) >= std::abs(tilt) ? depth : tilt;
}

// Off-screen resources reused across frames: whatever is acquired during a
// frame survives endFrame(); the rest is released. `keep` lets a caller
// rebuild an entry whose shape changed (a named target resized).
template<class Key, class Value> class FramePool {
public:
    template<class Make> Value& acquire(const Key& key, Make&& make) {
        return acquire(key, std::forward<Make>(make), [](const Value&) { return true; });
    }
    template<class Make, class Keep> Value& acquire(const Key& key, Make&& make, Keep&& keep) {
        auto found = entries_.find(key);
        if (found == entries_.end() || !keep(*found->second.value)) {
            Entry entry{std::make_unique<Value>(make()), false};
            found = entries_.insert_or_assign(key, std::move(entry)).first;
        }
        found->second.used = true;
        return *found->second.value;
    }
    [[nodiscard]] Value* find(const Key& key) const {
        const auto found = entries_.find(key);
        return found == entries_.end() ? nullptr : found->second.value.get();
    }
    void endFrame() {
        std::erase_if(entries_, [](const auto& entry) { return !entry.second.used; });
        for (auto& entry : entries_) {
            entry.second.used = false;
        }
    }
    void clear() { entries_.clear(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::unique_ptr<Value> value;
        bool used = false;
    };
    std::map<Key, Entry> entries_;
};

// Interpolation of world:draw camera overrides. A draw's camera is matched
// with the previous frame's draw of the same world into the same target (the
// n-th such draw of the frame); when a tick has passed since, the values it
// had become the start of the interpolation, as the world camera's
// beginTick() does. A draw not made on the previous frame starts snapped.
class DrawCameras {
public:
    const Camera& update(const void* world, const std::string& target, const Camera& next,
                         std::uint64_t tick) {
        const Key key{world, target, ordinals_[{world, target}]++};
        auto [found, added] = states_.try_emplace(key);
        auto& state = found->second;
        if (added) {
            state.camera = next;
            state.camera.snap();
        } else {
            if (state.tick != tick) {
                state.camera.beginTick();
            }
            state.camera.target = next.target;
            state.camera.yaw = next.yaw;
            state.camera.pitch = next.pitch;
            state.camera.distance = next.distance;
            state.camera.fov = next.fov;
            state.camera.nearPlane = next.nearPlane;
            state.camera.farPlane = next.farPlane;
            state.camera.shake = next.shake;
        }
        state.tick = tick;
        state.used = true;
        return state.camera;
    }
    void endFrame() {
        std::erase_if(states_, [](const auto& entry) { return !entry.second.used; });
        for (auto& entry : states_) {
            entry.second.used = false;
        }
        ordinals_.clear();
    }
    [[nodiscard]] std::size_t size() const { return states_.size(); }

private:
    using Key = std::tuple<const void*, std::string, int>;
    struct State {
        Camera camera;
        std::uint64_t tick = 0;
        bool used = false;
    };
    std::map<Key, State> states_;
    std::map<std::pair<const void*, std::string>, int> ordinals_;
};

// Freeze-frame lifetime. fe.screen.freeze() sets `frozen` and bumps the
// serial; the renderer captures once per serial and releases the image when
// the screen is unfrozen.
class FreezeFrame {
public:
    enum class Action { None, Capture, Release };
    Action update(bool frozen, std::uint32_t serial) {
        if (!frozen) {
            const bool release = held_;
            held_ = false;
            return release ? Action::Release : Action::None;
        }
        if (held_ && serial == serial_) {
            return Action::None;
        }
        held_ = true;
        serial_ = serial;
        return Action::Capture;
    }
    [[nodiscard]] bool held() const { return held_; }

private:
    bool held_ = false;
    std::uint32_t serial_ = 0;
};

// Game post shaders take up to 16 numbers, packed in order into four vec4s.
constexpr std::size_t CustomParamCount = 16;
inline std::array<glm::vec4, 4> packCustomParams(const std::vector<float>& params) {
    if (params.size() > CustomParamCount) {
        throw std::runtime_error("A custom post shader takes at most 16 params");
    }
    std::array<glm::vec4, 4> packed{};
    for (std::size_t i = 0; i < params.size(); ++i) {
        packed[i / 4][static_cast<glm::length_t>(i % 4)] = params[i];
    }
    return packed;
}

// Instanced draws: `order` becomes the indices of `keys` with equal keys
// adjacent, groups in the order of their first key and each group in its
// original order, so one instanced draw per group keeps the first-use order.
template<class Key> void groupByFirstUse(const std::vector<Key>& keys, std::vector<int>& order) {
    std::map<Key, int> first;
    std::vector<int> rank(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        rank[i] = first.try_emplace(keys[i], static_cast<int>(i)).first->second;
    }
    order.resize(keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return rank[static_cast<std::size_t>(a)] < rank[static_cast<std::size_t>(b)];
    });
}

// The normal an instanced mesh vertex gets (vs_world_instanced.sc): the model
// matrix's cofactor over its determinant, which is its inverse transpose, so
// it agrees with the normal matrix of single draws, mirrored scales included.
inline glm::vec3 instanceNormal(const glm::mat4& model, glm::vec3 normal) {
    const glm::vec3 c0(model[0]), c1(model[1]), c2(model[2]);
    float determinant = glm::dot(c0, glm::cross(c1, c2));
    determinant = std::abs(determinant) > 1e-12f ? determinant : 1.0f;
    return (glm::cross(c1, c2) * normal.x + glm::cross(c2, c0) * normal.y + glm::cross(c0, c1) * normal.z) /
           determinant;
}

// A 0xRRGGBBAA colour with red, green and blue multiplied by its alpha (rounded).
inline std::uint32_t premultiplyRgba(std::uint32_t rgba) {
    const std::uint32_t a = rgba & 0xffu;
    const auto channel = [&](unsigned shift) {
        return (((rgba >> shift) & 0xffu) * a + 127u) / 255u << shift;
    };
    return channel(24) | channel(16) | channel(8) | a;
}

// Per-glyph text animation recorded on canvas quads (CanvasQuad::effect).
enum GlyphEffect : std::uint8_t { GlyphEffectNone = 0, GlyphEffectShake = 1, GlyphEffectWave = 2 };

// Offset in whole canvas pixels of a glyph at `time`. Shake jumps to a new
// spot 15 times a second, differently per glyph; wave runs a sine along the
// text, `phase` being the glyph's position in it.
inline glm::vec2 glyphEffectOffset(std::uint8_t effect, float phase, double time) {
    if (effect == GlyphEffectShake) {
        const auto step = static_cast<std::uint32_t>(std::floor(time * 15.0));
        auto hash = [](std::uint32_t x) {
            x ^= x >> 16u;
            x *= 0x7feb352du;
            x ^= x >> 15u;
            x *= 0x846ca68bu;
            x ^= x >> 16u;
            return x;
        };
        const std::uint32_t seed = hash(step * 0x9e3779b9u ^ static_cast<std::uint32_t>(phase * 131.0f));
        return {static_cast<float>(seed % 3u) - 1.0f, static_cast<float>((seed >> 8u) % 3u) - 1.0f};
    }
    if (effect == GlyphEffectWave) {
        return {0, std::round(2.0f * static_cast<float>(std::sin(time * 8.0 - phase * 0.7)))};
    }
    return {0, 0};
}

// Save thumbnails keep at most `maxWidth` pixels across: the integer factor
// the captured frame is box-filtered down by.
inline int thumbnailDivisor(glm::ivec2 size, int maxWidth) {
    return std::max(1, (size.x + maxWidth - 1) / maxWidth);
}

// Box-filters an RGBA8 image by `divisor`, dropping partial blocks at the edges.
inline std::vector<std::uint8_t> downscaleRgba(const std::vector<std::uint8_t>& rgba, glm::ivec2 size,
                                               int divisor, glm::ivec2& outSize) {
    outSize = glm::max(glm::ivec2(1), size / divisor);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(outSize.x) * static_cast<std::size_t>(outSize.y) *
                                  4);
    for (int y = 0; y < outSize.y; ++y) {
        for (int x = 0; x < outSize.x; ++x) {
            for (int c = 0; c < 4; ++c) {
                unsigned sum = 0, count = 0;
                for (int dy = 0; dy < divisor && y * divisor + dy < size.y; ++dy) {
                    for (int dx = 0; dx < divisor && x * divisor + dx < size.x; ++dx) {
                        const auto index =
                            (static_cast<std::size_t>(y * divisor + dy) * static_cast<std::size_t>(size.x) +
                             static_cast<std::size_t>(x * divisor + dx)) *
                                4 +
                            static_cast<std::size_t>(c);
                        sum += rgba[index];
                        ++count;
                    }
                }
                out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(outSize.x) +
                     static_cast<std::size_t>(x)) *
                        4 +
                    static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>((sum + count / 2) / std::max(1u, count));
            }
        }
    }
    return out;
}
} // namespace fe
