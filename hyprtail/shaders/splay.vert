#version 300 es
#pragma hyprtail contract 2
#pragma hyprtail topology path
// hyprtail geometry "splay": convex.vert's thin strip, pointed at both ends,
// whose centerline moves sideways with the speed of the node it passes.
// Several layers with opposite spread_px signs make rails that open up
// while the pointer moves and close to one line at rest: a cord under
// tension. For spring-source presets (source = spring).
//
//   - Sideways shift of a node = offset + spread_px * k, in px, positive to
//     the right of travel. k = clamp(speed / speed_ref, 0, 1), optionally
//     eased (spread_ease). k is 0 at rest, so spread_px adds nothing then and
//     the strip sits at offset; offset is the static shift convex.vert has,
//     and it still applies in full whatever the speed.
//   - Speed is the node's own ht_vSpeed (px/ms). On a spring-chain source it
//     is the point's spring velocity, current every tick, so it is smooth in
//     time and along the chain. On the pointer source it is the raw step
//     between two samples, frozen at the node's birth, and is noisy: the
//     rails would flutter. Use this shader with source = spring.
//   - Every node moves by the amount its own speed asks, so the strip flares
//     where the chain is fast (near the head) and closes where it is slow
//     (the tail, and turns, where the chain slows down).
//   - Width, head ramp, tail shape and joins are convex.vert's, unchanged.
//
// To judge: at rest the rails lie on top of one another; a flick opens them
// smoothly, never in steps; they close again as the cord settles; nothing
// spikes at a sharp turn.
//
// Not clamped against the strip width or the segment length: spread moves
// the centerline, width is the strip's own thickness, and the two are
// independent. Past a sharp turn the inner edge can fold over itself; the
// miter limit bounds the corner, and the chain is slow at a turn, which
// shrinks the spread there.
//
// speed_ref is declared exactly like the one in gradient.frag (the loader
// requires the same declaration in both stages), so one setting scales both:
// the speed that gives full spread is the speed that gives color_by = 0 its
// full color_b.
//
// Writes all six standard varyings, so any path fragment shader pairs with
// it. ht_vLocal is x 0 at the newer end of a segment, 1 at the older, y -1..1
// across the strip (a fraction of the half-width, whatever the taper).
//
// Includes only the contract's own helpers (helpers/ribbon.glsl: joins and
// the degenerate-segment guard; helpers/fade.glsl: ht_life).

#include "helpers/ribbon.glsl"
#include "helpers/fade.glsl"

// Strip width at its fattest, px.
#pragma hyprtail param float width 3 0 64 // ribbon thickness at its widest, px
// Static sideways shift of the strip, px, positive to the right of travel.
// Added to the speed-driven spread.
#pragma hyprtail param float offset 0 -24 24 // sideways shift, px; positive = right of travel
// Extra sideways shift at full speed, px, positive to the right of travel.
// 0 = this layer does not move sideways. Opposite signs on two layers make
// them splay apart.
#pragma hyprtail param float spread_px 0 -32 32 // extra sideways drift at full speed, px
// Speed that gives the full spread, px/ms. Also the color_by = 0 scale of the
// color shaders.
#pragma hyprtail param float speed_ref 2 0.001 1000 // speed for full drift and full color_b, px/ms; lower = reacts sooner
// How spread follows speed: 0 = linearly, 1 = smoothstep (no change right at
// rest or at speed_ref).
#pragma hyprtail param float spread_ease 1 0 1 // drift response: 0 = linear, 1 = gentle at both ends
// Time over which the head grows from a point to full width, ms; 0 = blunt.
#pragma hyprtail param float head_ms 140 0 2000 // how gradually the head swells, ms; 0 = blunt head
// Tail shape: 1 = width follows life linearly; above 1 the tail thins sooner.
#pragma hyprtail param float tail_curve 1 0.25 4 // tail thinning: 1 = even, higher = thins sooner
#pragma hyprtail param float miter_limit 2 1 16 // how far sharp corners reach, half-widths; lower = blunter corners
// Furthest a corner gets from a node: the largest shifts (offset 24, spread
// 32) plus half the width, all scaled by the widest miter. Padding cannot
// take an absolute value, so these are the tops of the parameters' ranges.
#pragma hyprtail padding (width * 0.5 + 24 + 32) * miter_limit + 1

float halfWidthOf(HtNode n) {
    float life = ht_life(n.age, fade_ms);
    float tail = life > 0.0 ? pow(life, tail_curve) : 0.0;
    float head = head_ms > 0.0 ? smoothstep(0.0, head_ms, n.age) : 1.0;
    return 0.5 * width * head * tail;
}

// Speed-driven part of the sideways shift, px. Zero when the node is at rest.
float spreadOf(HtNode n) {
    float k = clamp(length(n.vel) / speed_ref, 0.0, 1.0);
    return spread_px * mix(k, smoothstep(0.0, 1.0, k), spread_ease);
}

void main() {
    ht_initVaryings();

    HtNode prev = ht_prev();
    HtNode p0   = ht_p0();
    HtNode p1   = ht_p1();
    HtNode next = ht_next();

    // Not connected, or both ends fully faded: nothing to draw.
    if (p1.segmentStart || (ht_faded(p0.age, fade_ms) && ht_faded(p1.age, fade_ms))) {
        gl_Position = ht_collapsedPosition();
        return;
    }

    // Coincident endpoints: no direction, nothing to draw. The neighbors fall
    // back to their own direction at this joint (ht_dirBetween). A settled
    // spring chain is all of these, and its nodes have no speed, so no
    // spread is lost by skipping them.
    if (length(p1.pos - p0.pos) < HT_EPS) {
        gl_Position = ht_collapsedPosition();
        return;
    }

    vec2 dir     = ht_dirBetween(p0.pos, p1.pos, vec2(1.0, 0.0));
    vec2 dirPrev = p0.segmentStart ? dir : ht_dirBetween(prev.pos, p0.pos, dir);
    vec2 dirNext = next.segmentStart ? dir : ht_dirBetween(p1.pos, next.pos, dir);

    bool  atEnd = ht_atEnd();
    float side  = ht_side();

    HtNode n  = atEnd ? p1 : p0;
    float  hw = halfWidthOf(n);

    // ht_jointOffset is linear in its width argument, so the shifts and the
    // half-width share one call: the strip's centerline is the path shifted
    // by offset + spread, its edges +-hw from that. Both segments at a joint
    // read the same node, so they agree on its spread and the strip stays
    // joined.
    float reach = offset + spreadOf(n) + side * hw;
    vec2  shift = atEnd ? ht_jointOffset(dir, dirNext, reach, miter_limit) : ht_jointOffset(dirPrev, dir, reach, miter_limit);

    ht_vLocal = vec2(atEnd ? 0.0 : 1.0, side);
    ht_vAge   = n.age;
    ht_vLife  = ht_life(n.age, fade_ms);
    ht_vSpeed = length(n.vel);
    ht_vDist  = n.dist;
    ht_vSeed  = n.seed;

    gl_Position = ht_toClip(n.pos + shift);
}
