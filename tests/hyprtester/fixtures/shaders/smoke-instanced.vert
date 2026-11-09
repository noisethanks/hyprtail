#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology instanced copies
// Smoke test fixture: `copies` small squares on a circle around every
// visible node. K is a param so the test can change it live. Only has to
// build and draw.

#pragma hyprtail param int copies 4 1 64
#pragma hyprtail param float size 3 0.5 64
#pragma hyprtail param float spread 8 0 256
#pragma hyprtail padding spread + size

void main() {
    ht_initVaryings();

    HtNode n     = ht_node();
    float  angle = 6.2831853 * (float(ht_instance()) + n.seed) / float(ht_K);
    vec2   c     = ht_corner();

    ht_vLocal    = c;
    ht_vAge      = n.age;
    ht_vLife     = clamp(1.0 - n.age / fade_ms, 0.0, 1.0);
    ht_vSeed     = n.seed;
    gl_Position  = ht_toClip(n.pos + vec2(cos(angle), sin(angle)) * spread + c * size);
}
