#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "TrailBuffer.hpp"

// Warp interpolation: the nodes a hooked warp inserts between
// the trail's newest node and the warp target, as positions and birth times,
// and their playback: births lie ahead of the warp, and each node goes into
// the source once its birth time has come (CPlayback, sample()), so the warp
// draws from origin to target like motion at warp_speed, and fades in the
// same order. No Hyprland headers, so the easing, the timing, the landing and the
// playback are unit tested with plain functions standing in for Hyprland's
// bezier curves.
namespace hyprtail::warp {
    enum class eShape : uint8_t {
        LINE,  // straight from the newest node to the target
        CURVE, // quadratic Bezier, control point along the incoming velocity
    };

    // Normalized elapsed time (0..1) -> normalized progress along the path.
    // Empty = linear, even spacing. Results outside 0..1 are overshoot and
    // are kept, up to 4 path lengths either side; a non-finite result counts
    // as linear for that node.
    using FEase = std::function<float(float)>;

    struct SNode {
        SVec2f posPx;
        double birthMs;
    };

    // How long a warp of `chordPx` takes to draw: chord / speedPxPerMs, at
    // most maxMs (the whole of maxMs for a speed that isn't positive), never
    // negative.
    double durationMs(float chordPx, double speedPxPerMs, double maxMs);

    // Nodes for a warp from `from` (the newest node before the warp, a copy)
    // to `to` at nowMs, timed as motion: the warp takes durationMs(chord,
    // speedPxPerMs, maxMs) to draw, so a long warp is as fast as a short one.
    // Count: chord / maxSpacingPx rounded up, 1..RING_CEILING: evenly spaced along
    // a linear path, maxSpacingPx at most apart. Eased gaps can be wider or
    // narrower; the emission rule (emitTo) fills or skips them on insert, as
    // for real motion.
    //
    // Birth times: node i of n is born at start + duration * i / n, start =
    // max(nowMs, from's birth): the warp draws from now on however recently
    // the pointer last moved, and births never decrease along the source,
    // which its visibility walk relies on (TrailBuffer.hpp, visibleCountOf);
    // the max only matters for a clock that went backwards. The last one is
    // exactly start + duration. Real time, never eased: the easing moves each
    // node along the path instead.
    //
    // The last node is `to` exactly, whatever the easing.
    std::vector<SNode> nodes(eShape shape, const SCursorNode& from, const SVec2f& to, double nowMs, double speedPxPerMs, double maxMs, float maxSpacingPx, const FEase& ease);

    // A warp's nodes waiting for their birth time, oldest first. Positions
    // and times only, nothing source-specific: the pointer history inserts
    // them as they come due; a spring chain doesn't queue today (it moves by
    // the jump, ISource::warp) but could follow head() along the path.
    //
    // Pointer space: nodes are emit points, while quad layers and ht_anchor
    // follow the raw pointer. `anchorOffset` (raw pointer minus emit point,
    // at the target) carries the playback head over, so anchor() ends on the
    // pointer exactly.
    class CPlayback {
      public:
        // Replaces whatever was queued. `from` is where the drawn part ends
        // before the first node comes due (the source's newest node), and
        // startMs when it was reached: the first node's step interpolates
        // births from there.
        void   start(std::vector<SNode> nodes, const SVec2f& from, double startMs, const SVec2f& anchorOffset);
        void   clear();

        bool   empty() const;
        size_t pending() const;

        // Inserts the queued nodes born at or before nowMs, oldest first,
        // through the emission rule (emitTo: fills a gap wider than
        // maxSpacingPx, skips one under minSpacingPx), the last one landing
        // exactly. They leave the queue.
        void drain(ISource& source, double nowMs, float minSpacingPx, float maxSpacingPx);

        // Inserts every queued node the same way, births capped at nowMs: the
        // rest of the warp draws at once.
        void flush(ISource& source, double nowMs, float minSpacingPx, float maxSpacingPx);

        // Birth of the last queued node: when the warp has drawn. Requires
        // !empty().
        double endMs() const;

        // Where the drawn part ends: the last node handed out, else `from`.
        const SVec2f& head() const;

        // The warp's target, the last queued node. Requires !empty().
        const SVec2f& target() const;

        // While nodes are queued, head() in pointer space (see above):
        // what connects the trail to the pointer uses it instead of the
        // pointer, so nothing spans the part not drawn yet. nullopt when
        // empty.
        std::optional<SVec2f> anchor() const;

        // Extent of head() and every queued node, where nodes are still to
        // appear. nullopt when empty.
        std::optional<STrailBounds> bounds() const;

      private:
        // One node through the emission rule, then it is the head.
        void              feed(ISource& source, const SNode& n, double birthMs, float minSpacingPx, float maxSpacingPx);

        std::deque<SNode> m_queue;
        SVec2f            m_head;
        SVec2f            m_anchorOffset;
        double            m_prevMs = 0.0; // birth of the last node fed
    };

    // One render's sample of the emit point `pos` into `source`, with a warp
    // possibly playing. A pending break drops the queue (the rest of the
    // warp never draws). Otherwise the nodes that came due go in first; if
    // some are still queued, a pointer that moved at least minSpacingPx from
    // the target (real motion) flushes the rest, born now, and is sampled as
    // usual, while one still at the target inserts nothing: a node there now
    // would draw a straight segment across the part of the warp not drawn
    // yet. Then the motion itself through the emission rule (emitTo), its
    // births interpolated from fromMs, the pending break starting the new
    // segment.
    void sample(ISource& source, CPlayback& playback, const SVec2f& pos, double fromMs, double nowMs, float minSpacingPx, float maxSpacingPx, bool& pendingBreak);

    // A line or curve warp to the emit point `to` at nowMs. A warp still
    // playing is flushed first (its rest drawn at once, born now), so the new
    // one starts where it landed. Then the source either carries itself
    // across (ISource::warp: a spring chain), or the warp's nodes are queued
    // from its newest node (nodes()). Nothing to start from if the source is
    // empty: the next sample connects plainly.
    void start(ISource& source, CPlayback& playback, eShape shape, const SVec2f& to, const SVec2f& anchorOffset, double nowMs, double speedPxPerMs, double maxMs,
               float minSpacingPx, float maxSpacingPx, const FEase& ease);
}
