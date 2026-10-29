// Chromatic aberration shared by the post pass (one world) and the screen
// pass (everything); mirrors chromaticShift() in FrameMath.h. With amount a,
// red is read a * 3% of the way further from the screen centre and blue as
// much nearer: sample red at uv + shift and blue at uv - shift.
vec2 chromaticShift(vec2 uv, float amount) { return (uv - 0.5) * (amount * 0.03); }
