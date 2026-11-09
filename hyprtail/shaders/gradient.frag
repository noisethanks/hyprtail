#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail expects path
// hyprtail look "gradient": a two-color ribbon, from a crisp line to a soft
// glow. The base look of every ribbon trail, and the reference for
// fragment shaders (see the contract summary in taper.vert).
//
// Reads only standard varyings, so it pairs with any path geometry shader
// that writes them. Output is PREMULTIPLIED alpha. Only color parameters are
// color-managed; colors computed or hardcoded here are written as-is
// (correct on SDR sRGB outputs, wrong on HDR / wide-gamut ones).
//
// Same layer, two jobs: softness 0 is a hard-edged core, softness near 1 with
// a wide width and low alpha colors is a glow to put under another ribbon.
// alpha scales the whole layer, and fade_curve shapes how it falls with life.

#include "helpers/palette.glsl"

#pragma hyprtail param color color_a rgba(1a66ffff) // starting color: slow, newborn, or start of a cycle
#pragma hyprtail param color color_b rgba(ff1a1aff) // ending color: fast, fading, or middle of a cycle
// 0 speed, 1 life, 2 distance, 3 seed, 4 cycle: see helpers/palette.glsl.
#pragma hyprtail param int color_by 0 0 4 // what shifts the color: 0 speed, 1 age, 2 distance, 3 random, 4 time cycle
#pragma hyprtail param float speed_ref 2 0.001 1000 // speed for full color_b, px/ms; lower = flares sooner
#pragma hyprtail param float color_period 200 1 100000 // length of one color cycle, px or ms; lower = tighter bands
// 0 = solid to the edge, 1 = fades out from the centerline.
#pragma hyprtail param float softness 0 0 1 // edge softness: 0 = crisp, 1 = glow fading to the edges
// Overall opacity on top of the colors' own alpha.
#pragma hyprtail param float alpha 1 0 1 // opacity, 0..1
// How the opacity falls with life: 1 = linearly, above 1 sooner, below 1 later.
#pragma hyprtail param float fade_curve 1 0.25 4 // how the tail fades: 1 = evenly, higher = vanishes sooner

void main() {
    // Across the width: 0 at the centerline, 1 at the edge. The ~1px
    // antialiased edge stays inside the geometry so the damage box covers it.
    float d   = abs(ht_vLocal.y);
    float aa  = fwidth(ht_vLocal.y);
    float cov = 1.0 - smoothstep(1.0 - max(softness, aa), 1.0, d);
    if (cov <= 0.0)
        discard;

    float t = ht_paletteT(color_by, ht_vSpeed, ht_vLife, ht_vDist, ht_vSeed, ht_vAge, speed_ref, color_period);
    vec4  c = mix(color_a, color_b, t);

    float a      = c.a * alpha * pow(max(ht_vLife, 0.0), fade_curve) * cov;
    ht_fragColor = vec4(c.rgb * a, a);
}
