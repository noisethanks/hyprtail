#version 300 es
#pragma hyprtail contract 2
// Smoke test fixture: one color, faded by ht_vLife. Pairs with every
// fixture geometry (no `expects`). Output is premultiplied alpha.

#pragma hyprtail param color color_a rgba(ffffffff)

void main() {
    float a      = color_a.a * ht_vLife;
    ht_fragColor = vec4(color_a.rgb * a, a);
}
