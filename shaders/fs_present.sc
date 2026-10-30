$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_frame, 0);
SAMPLER2D(s_freeze, 1);
#include "chromatic.sh"
// [0] transition kind (1 swirl, 2 mosaic, 3 wipe, 4 iris, 5 fade, 6 shatter), progress, centre x, centre y
// [1] transition colour
// [2] fade rgb, amount
// [3] flash rgb, amount
// [4] letterbox bar height (fraction of the screen), aspect ratio, texture origin bottom-left (0/1), frozen (0/1)
// [5] radial blur, radial centre x, y, chromatic aberration
// [6] output encoding (0 sRGB as is, 1 scRGB linear, 2 HDR10 PQ), linear scale (paper white over
//     the reference white, or the paper white in nits for PQ)
uniform vec4 u_present[7];

// Positions use screen coordinates: 0..1 with y down, like the canvas.
vec2 texel(vec2 p) { return u_present[4].z > 0.5 ? vec2(p.x, 1.0 - p.y) : p; }

vec3 fetch(vec2 p, bool frozen) {
    vec2 uv = texel(p);
    return frozen ? texture2D(s_freeze, uv).rgb : texture2D(s_frame, uv).rgb;
}

// The frame (or the held one) with radial blur and chromatic aberration.
vec3 look(vec2 p, bool frozen) {
    float strength = u_present[5].x;
    float chromatic = u_present[5].w;
    if (strength <= 0.0 && chromatic <= 0.0) return fetch(p, frozen);
    if (strength <= 0.0) {
        // Chromatic aberration alone: one split tap, not the 12 of the blur.
        vec2 d = chromaticShift(p, chromatic);
        return vec3(fetch(p + d, frozen).r, fetch(p, frozen).g, fetch(p - d, frozen).b);
    }
    vec2 c = u_present[5].yz;
    vec3 sum = vec3_splat(0.0);
    for (int i = 0; i < 12; ++i) {
        vec2 q = c + (p - c) * (1.0 - strength * 0.3 * float(i) / 11.0);
        vec2 d = chromaticShift(q, chromatic);
        sum += vec3(fetch(q + d, frozen).r, fetch(q, frozen).g, fetch(q - d, frozen).b);
    }
    return sum / 12.0;
}

vec2 transitionSource(vec2 p) {
    float kind = u_present[0].x;
    float t = u_present[0].y;
    vec2 aspect = vec2(u_present[4].y, 1.0);
    if (kind > 0.5 && kind < 1.5) {
        vec2 c = (p - u_present[0].zw) * aspect;
        float angle = t * 9.0 * (1.0 - length(c));
        float s = sin(angle), co = cos(angle);
        return u_present[0].zw + vec2(c.x * co - c.y * s, c.x * s + c.y * co) * (1.0 - t * 0.6) / aspect;
    }
    if (kind > 1.5 && kind < 2.5) {
        vec2 cells = mix(240.0, 8.0, t) * aspect;
        return (floor(p * cells) + 0.5) / cells;
    }
    return p;
}

float transitionCover(vec2 p) {
    float kind = u_present[0].x;
    float t = u_present[0].y;
    if (kind < 0.5) return 0.0;
    if (kind < 2.5) return smoothstep(0.6, 1.0, t);
    if (kind < 3.5) return step(p.x, t * 1.1 - 0.05);
    if (kind < 4.5) {
        vec2 aspect = vec2(u_present[4].y, 1.0);
        vec2 c = u_present[0].zw;
        float furthest = length(max(c, 1.0 - c) * aspect);
        return step((1.0 - t) * furthest * 1.05, length((p - c) * aspect));
    }
    return t;
}

float hash(vec2 v) { return fract(sin(dot(v, vec2(12.9898, 78.233))) * 43758.5453); }

vec3 srgbToLinear(vec3 c) {
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3_splat(2.4));
    return mix(low, high, step(0.04045, c));
}

// SMPTE ST 2084 of luminance in nits; FrameMath::pqEncode mirrors it.
vec3 pqEncode(vec3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0, c1 = 3424.0 / 4096.0,
                c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3_splat(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3_splat(m2));
}

vec3 rec709ToRec2020(vec3 c) {
    return vec3(dot(vec3(0.6274, 0.3293, 0.0433), c), dot(vec3(0.0691, 0.9195, 0.0114), c),
                dot(vec3(0.0164, 0.0880, 0.8956), c));
}

// The frame (worlds and canvas, composed in sRGB) for the back buffer:
// untouched for an 8-bit one; decoded to linear light and scaled to the paper
// white for an HDR one (FrameMath::outputColor).
vec3 encodeOutput(vec3 color) {
    float mode = u_present[6].x;
    if (mode < 0.5) return color;
    vec3 light = srgbToLinear(clamp(color, 0.0, 1.0)); // `linear` is an HLSL keyword
    if (mode < 1.5) return light * u_present[6].y;
    return pqEncode(rec709ToRec2020(light) * u_present[6].y);
}

// Shatter: the screen splits into triangular shards (two per cell of an 8x5
// grid) that spin, shrink and fall at staggered times. Returns whether `p`
// shows a shard and, in `source`, the point of the original image it shows.
bool shard(vec2 p, out vec2 source) {
    vec2 cells = vec2(8.0, 5.0);
    vec2 aspect = vec2(u_present[4].y, 1.0);
    float t = u_present[0].y;
    source = p;
    for (int column = -1; column <= 1; ++column) {
        for (int row = 0; row < 5; ++row) {
            vec2 cell = vec2(floor(p.x * cells.x) + float(column), float(row));
            if (cell.x < 0.0 || cell.x >= cells.x) continue;
            bool diagonal = hash(cell + 0.37) > 0.5;
            for (int piece = 0; piece < 2; ++piece) {
                float h = hash(cell * 1.7 + float(piece) * 9.1);
                float local = max(0.0, t - h * 0.35 - (4.0 - cell.y) * 0.04) / 0.5;
                vec2 centre = (cell + 0.5) / cells;
                vec2 offset = vec2((h - 0.5) * 0.3 * local, 1.6 * local * local);
                float angle = (hash(cell + float(piece) * 3.3) - 0.5) * 3.0 * local;
                float scale = max(0.05, 1.0 - 0.4 * local);
                vec2 d = (p - centre - offset) * aspect / scale;
                float s = sin(-angle), c = cos(-angle);
                vec2 q = centre + vec2(d.x * c - d.y * s, d.x * s + d.y * c) / aspect;
                vec2 f = q * cells - cell;
                if (f.x < 0.0 || f.y < 0.0 || f.x > 1.0 || f.y > 1.0) continue;
                bool first = diagonal ? f.x > f.y : f.x + f.y < 1.0;
                if (first == (piece == 0)) {
                    source = q;
                    return true;
                }
            }
        }
    }
    return false;
}

void main() {
    vec2 p = texel(v_texcoord0);
    bool frozen = u_present[4].w > 0.5;
    float kind = u_present[0].x;
    vec3 color;
    if (kind > 5.5) {
        vec2 source;
        bool hit = shard(p, source);
        color = hit ? look(source, frozen) : (frozen ? look(p, false) : u_present[1].rgb);
    } else if (frozen) {
        color = mix(look(transitionSource(p), true), look(p, false), transitionCover(p));
    } else {
        color = mix(look(transitionSource(p), false), u_present[1].rgb, transitionCover(p));
    }
    float bar = u_present[4].x;
    color *= step(bar, p.y) * step(p.y, 1.0 - bar);
    color = mix(color, u_present[2].rgb, u_present[2].a);
    color += u_present[3].rgb * u_present[3].a;
    gl_FragColor = vec4(encodeOutput(color), 1.0);
}
