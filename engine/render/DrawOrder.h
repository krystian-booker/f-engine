#pragma once
#include "engine/world/MapData.h"

namespace fe {
// Whether a blend mode draws after the opaque scene without writing depth.
constexpr bool blended(BlendMode mode) {
    return mode != BlendMode::Opaque;
}

// How a sprite draws: opaque sprites with a tint alpha below 1 draw as alpha.
constexpr BlendMode spriteBlend(BlendMode mode, float tintAlpha) {
    return mode == BlendMode::Opaque && tintAlpha < 0.999f ? BlendMode::Alpha : mode;
}

// Shadows come from depth-writing draws only: a blended sprite or mesh part
// would cast as a full alpha cutout, so it casts nothing.
constexpr bool castsShadow(bool requested, BlendMode mode) {
    return requested && !blended(mode);
}

// Transparent draws go in passes: alpha and premultiplied (which must be
// sorted against each other), then multiply, then additive. Multiply and
// additive results don't depend on order within their pass.
constexpr int blendPass(BlendMode mode) {
    switch (mode) {
    case BlendMode::Multiply:
        return 1;
    case BlendMode::Additive:
        return 2;
    case BlendMode::Opaque:
    case BlendMode::Alpha:
    case BlendMode::Premultiplied:
        break;
    }
    return 0;
}

struct TransparentKey {
    BlendMode blend = BlendMode::Alpha;
    float distance = 0; // from the camera
};

// Strict weak order: by pass, then back to front.
constexpr bool drawsBefore(const TransparentKey& a, const TransparentKey& b) {
    const int pa = blendPass(a.blend), pb = blendPass(b.blend);
    return pa != pb ? pa < pb : a.distance > b.distance;
}
} // namespace fe
