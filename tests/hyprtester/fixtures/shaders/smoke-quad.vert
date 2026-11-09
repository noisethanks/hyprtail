#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology quad
// Smoke test fixture: the square around the pointer, ht_anchor +-
// ht_extentPx. Only has to build and draw.

#pragma hyprtail param float radius 12 1 256
#pragma hyprtail padding radius

void main() {
    ht_initVaryings();

    vec2 c      = ht_corner();
    ht_vLocal   = c;
    ht_vAge     = ht_stillMs;
    ht_vLife    = 1.0;
    gl_Position = ht_toClip(ht_anchor + c * ht_extentPx);
}
