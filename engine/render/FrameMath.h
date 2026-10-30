#pragma once
#include "engine/world/Camera.h"
#include "engine/world/MapData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// CPU-side rules of the frame renderer that need no GPU: bloom chain sizes,
// particle near fade, circle of confusion, chromatic aberration, the HDR
// output transform, uniform
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
// `brightness` that passes. `knee` is absolute (in brightness units): below
// threshold - knee nothing passes; a quadratic curve joins it to the linear
// part above threshold + knee. fs_bloom_down.sc mirrors it.
inline float bloomPass(float brightness, float threshold, float knee) {
    const float soft = std::clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
    const float curve = soft * soft / (4.0f * knee + 1e-4f);
    return std::max(curve, brightness - threshold) / std::max(brightness, 1e-4f);
}

// The absolute knee of a world's post settings: `bloom_knee` is authored as
// a fraction of `bloom_threshold`. The one place the conversion happens, so
// the shader (u_bloom[1].x) and bloomPass() always agree.
inline float bloomKnee(const PostSettings& post) {
    return post.bloomThreshold * post.bloomKnee;
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

// How the screen pass encodes the frame for the back buffer. The frame is
// composed in sRGB (worlds graded and encoded by the post pass, the canvas on
// top); an HDR back buffer takes it decoded to linear light and scaled.
enum class OutputEncoding {
    Srgb,  // 8-bit back buffer: the frame as it is
    ScRgb, // RGBA16F, extended linear sRGB: 1.0 is `referenceWhite` nits
    Hdr10, // RGB10A2, Rec. 2020 with the ST 2084 (PQ) curve, absolute nits
};

struct OutputColor {
    OutputEncoding encoding = OutputEncoding::Srgb;
    // The brightness of the frame's white (game.json hdr_paper_white) and
    // what the back buffer's 1.0 means: 80 nits for scRGB on Windows and
    // Vulkan; on macOS extended-range Metal layers put the display's own SDR
    // white at 1.0, so the runtime passes the paper white itself (scale 1).
    float paperWhite = 200, referenceWhite = 80;
};

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// The SMPTE ST 2084 perceptual quantizer of a luminance in nits (0..10000).
inline float pqEncode(float nits) {
    constexpr float m1 = 2610.0f / 16384, m2 = 2523.0f / 4096 * 128, c1 = 3424.0f / 4096,
                    c2 = 2413.0f / 4096 * 32, c3 = 2392.0f / 4096 * 32;
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

// Rec. 709 primaries to Rec. 2020, linear light.
inline glm::vec3 rec709ToRec2020(glm::vec3 c) {
    return {0.6274f * c.x + 0.3293f * c.y + 0.0433f * c.z, 0.0691f * c.x + 0.9195f * c.y + 0.0114f * c.z,
            0.0164f * c.x + 0.0880f * c.y + 0.8956f * c.z};
}

// What the screen pass writes for a composed (sRGB-encoded) frame colour.
// fs_present.sc mirrors it; with Srgb the colour passes through untouched,
// so an SDR frame stays bit-identical to one rendered before HDR existed.
inline glm::vec3 outputColor(glm::vec3 srgb, const OutputColor& output) {
    if (output.encoding == OutputEncoding::Srgb) {
        return srgb;
    }
    const glm::vec3 linear(srgbToLinear(srgb.x), srgbToLinear(srgb.y), srgbToLinear(srgb.z));
    if (output.encoding == OutputEncoding::ScRgb) {
        return linear * (output.paperWhite / std::max(output.referenceWhite, 1.0f));
    }
    const glm::vec3 nits = rec709ToRec2020(linear) * output.paperWhite;
    return {pqEncode(nits.x), pqEncode(nits.y), pqEncode(nits.z)};
}

// u_present[6] of fs_present.sc: encoding (0 sRGB, 1 scRGB, 2 HDR10) and the
// scale the decoded linear colour is multiplied by (paper white over the
// reference white, or the paper white in nits for PQ).
inline glm::vec4 packOutput(const OutputColor& output) {
    switch (output.encoding) {
    case OutputEncoding::Srgb:
        return {0, 1, 0, 0};
    case OutputEncoding::ScRgb:
        return {1, output.paperWhite / std::max(output.referenceWhite, 1.0f), 0, 0};
    case OutputEncoding::Hdr10:
        break;
    }
    return {2, output.paperWhite, 0, 0};
}

// An IEEE half-precision value as a float (screenshots of a 16F back buffer).
inline float halfToFloat(std::uint16_t half) {
    const std::uint32_t sign = (half >> 15u) & 1u, exponent = (half >> 10u) & 0x1fu, mantissa = half & 0x3ffu;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 0x1f) {
        value = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
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
    bool orthographic = false;     // the scene's depth is linear between the planes
};

// u_water of fs_water.sc: time, wave scale, wave speed, fresnel; deep colour,
// depth falloff; horizon, specular; near, far, depth tint (0/1), orthographic (0/1);
// reflection, refraction, scene copy (0/1), origin bottom-left (0/1).
inline std::array<glm::vec4, 5> packWater(const WaterSettings& w, const WaterFrame& frame) {
    const auto flag = [](bool on) {
        return on ? 1.0f : 0.0f;
    };
    return {glm::vec4(frame.time, w.waveScale, w.waveSpeed, w.fresnel),
            glm::vec4(w.deepColor, w.depthFalloff), glm::vec4(frame.horizon, w.specular),
            glm::vec4(frame.nearPlane, frame.farPlane, flag(frame.sceneCopy && w.depthFalloff > 0),
                      flag(frame.orthographic)),
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
    // Looks an entry up without acquiring it; `K` may be a view of the key
    // (a std::string_view for a std::string key), so no key is built.
    template<class K> [[nodiscard]] Value* find(const K& key) const {
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
    std::map<Key, Entry, std::less<>> entries_;
};

// The scene target size for an output of `outSize` pixels at `pixelScale`
// (PostSettings::pixelScale, 1..8): a whole scale divides exactly (each scene
// pixel becomes a block of scale x scale output pixels), a fraction rounds.
inline glm::ivec2 sceneSizeFor(glm::ivec2 outSize, float pixelScale) {
    const float scale = std::max(1.0f, pixelScale);
    if (scale == std::floor(scale)) {
        return glm::max(glm::ivec2(1), outSize / static_cast<int>(scale));
    }
    return glm::max(glm::ivec2(1), glm::ivec2(glm::round(glm::vec2(outSize) / scale)));
}

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
            state.camera.orthographic = next.orthographic;
            state.camera.orthoHeight = next.orthoHeight;
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
// `rank` is scratch the caller keeps: with it and `order` sized from an
// earlier call, a frame allocates nothing here (both sorts are in place).
template<class Key>
void groupByFirstUse(const std::vector<Key>& keys, std::vector<int>& order, std::vector<int>& rank) {
    const auto count = keys.size();
    order.resize(count);
    rank.resize(count);
    std::iota(order.begin(), order.end(), 0);
    // Equal keys adjacent, each group in index order; the first of a group is
    // the group's first use.
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& ka = keys[static_cast<std::size_t>(a)];
        const auto& kb = keys[static_cast<std::size_t>(b)];
        return ka != kb ? ka < kb : a < b;
    });
    for (std::size_t start = 0; start < count;) {
        const auto& key = keys[static_cast<std::size_t>(order[start])];
        std::size_t end = start + 1;
        while (end < count && keys[static_cast<std::size_t>(order[end])] == key) {
            ++end;
        }
        for (std::size_t k = start; k < end; ++k) {
            rank[static_cast<std::size_t>(order[k])] = order[start];
        }
        start = end;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const int ra = rank[static_cast<std::size_t>(a)], rb = rank[static_cast<std::size_t>(b)];
        return ra != rb ? ra < rb : a < b;
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

// A wrapping map (Grid::wraps()) is drawn once per visible mirror: shifted by
// 0, +-width and +-depth (and the four corners), whichever of those put the
// map's bounds `min`..`max` where `sees(min, max)` says they matter. Appends
// the shifts not yet in `out`, the zero shift first; a map that does not
// wrap (`width` or `depth` 0) gets only that one.
template<class Sees>
void appendWrapOffsets(glm::vec3 min, glm::vec3 max, float width, float depth, Sees&& sees,
                       std::vector<glm::vec3>& out) {
    if (out.empty()) {
        out.emplace_back(0);
    }
    if (width <= 0 || depth <= 0) {
        return;
    }
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const glm::vec3 offset(static_cast<float>(dx) * width, 0, static_cast<float>(dz) * depth);
            if ((dx == 0 && dz == 0) || std::find(out.begin(), out.end(), offset) != out.end()) {
                continue;
            }
            if (sees(min + offset, max + offset)) {
                out.push_back(offset);
            }
        }
    }
}
} // namespace fe
