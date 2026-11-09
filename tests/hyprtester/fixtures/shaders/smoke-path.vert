#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology path
// Smoke test fixture: a flat ribbon of constant width along each segment,
// no joins. Only has to build and draw; how it looks doesn't matter.

#pragma hyprtail param float width 4 0.5 64
#pragma hyprtail padding width

void main() {
    ht_initVaryings();

    HtNode p0 = ht_p0();
    HtNode p1 = ht_p1();
    HtNode n  = ht_atEnd() ? p1 : p0;

    // A segment start isn't joined to the node before it: zero width.
    vec2  d      = p1.pos - p0.pos;
    float len    = length(d);
    vec2  dir    = len > 0.0 ? d / len : vec2(1.0, 0.0);
    float w      = p1.segmentStart ? 0.0 : width;

    ht_vLocal    = vec2(ht_atEnd() ? 0.0 : 1.0, ht_side());
    ht_vAge      = n.age;
    ht_vLife     = clamp(1.0 - n.age / fade_ms, 0.0, 1.0);
    ht_vSeed     = n.seed;
    gl_Position  = ht_toClip(n.pos + vec2(-dir.y, dir.x) * ht_side() * w * 0.5);
}
