// Shared by the post-processing passes.
// u_dof[0] near, far, focus distance, aperture
// u_dof[1] tilt-shift line, tilt-shift range, mode (0 depth, 1 tilt, 2 both),
//          depth flags: homogeneous depth (0/1) plus 2 for an orthographic camera
// u_dof[2] texel w, h of the pass input, widest blur (pixels of the pass), texture origin bottom-left (0/1)
uniform vec4 u_dof[3];

float linearDepth(float device) {
    float n = u_dof[0].x;
    float f = u_dof[0].y;
    float flags = u_dof[1].w;
    if (flags > 1.5) {
        // Orthographic: window depth is already linear between the planes.
        return n + device * (f - n);
    }
    if (flags > 0.5) {
        float ndc = device * 2.0 - 1.0;
        return 2.0 * n * f / max(f + n - ndc * (f - n), 0.0001);
    }
    return n * f / max(f - device * (f - n), 0.0001);
}

// Signed circle of confusion, -1 (near) .. 1 (far); see FrameMath.h.
float circleOfConfusion(float viewDepth, vec2 uv) {
    float screenY = u_dof[2].w > 0.5 ? 1.0 - uv.y : uv.y;
    float depth = clamp(u_dof[0].w * (viewDepth - u_dof[0].z) / max(viewDepth, 0.1), -1.0, 1.0);
    float band = clamp((abs(screenY - u_dof[1].x) - u_dof[1].y) * 5.0, 0.0, 1.0);
    float tilt = screenY < u_dof[1].x ? band : -band;
    if (u_dof[1].z < 0.5) return depth;
    if (u_dof[1].z < 1.5) return tilt;
    return abs(depth) >= abs(tilt) ? depth : tilt;
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
