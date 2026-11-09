#include "WarpPath.hpp"

#include <algorithm>
#include <cmath>

namespace hyprtail::warp {
    namespace {
        // How far eased progress may leave 0..1, in path lengths. Keeps every
        // node finite and the damage box sane whatever the curve returns.
        // Never reached by a curve from hl.curve: its control points are
        // limited to -1..2 (LuaBindingsConfigRules.cpp:322), which keeps y
        // within about -0.66..1.66. The legacy `bezier` keyword has no limit
        // (legacy ConfigManager.cpp:1389-1421).
        constexpr float MAX_OVERSHOOT = 4.F;

        // Nodes one warp generates at most: the pointer history's size, more
        // would only overwrite the warp's own start. Spacing stays within
        // max_spacing up to 4096 * max_spacing px (32768 at the default 8).
        constexpr auto MAX_NODES = static_cast<double>(RING_CEILING);

        // Progress s along the straight path. Outside 0..1 it carries on along
        // the same line: past the target (s > 1) or behind the start (s < 0).
        SVec2f lineAt(const SVec2f& p0, const SVec2f& p2, float s) {
            return {p0.x + (p2.x - p0.x) * s, p0.y + (p2.y - p0.y) * s};
        }

        // Progress s along the quadratic Bezier (p0, p1, p2), s being its
        // parameter, not arc length. Outside 0..1 it carries on along the
        // tangent at the nearer end (B'(1) = 2 (p2 - p1), B'(0) = 2 (p1 -
        // p0)), not along the parabola, which would bend an overshoot back
        // around instead of past the target in the direction of travel.
        SVec2f curveAt(const SVec2f& p0, const SVec2f& p1, const SVec2f& p2, float s) {
            if (s > 1.F) {
                const float k = 2.F * (s - 1.F);
                return {p2.x + (p2.x - p1.x) * k, p2.y + (p2.y - p1.y) * k};
            }
            if (s < 0.F) {
                const float k = 2.F * s;
                return {p0.x + (p1.x - p0.x) * k, p0.y + (p1.y - p0.y) * k};
            }
            // The expression `warp = "curve"` used before easing existed,
            // kept as is so the linear case reproduces it bit for bit.
            const float u = 1.F - s;
            return {u * u * p0.x + 2.F * u * s * p1.x + s * s * p2.x, u * u * p0.y + 2.F * u * s * p1.y + s * s * p2.y};
        }
    }

    double durationMs(float chordPx, double speedPxPerMs, double maxMs) {
        const double cap = std::max(0.0, maxMs);
        return speedPxPerMs > 0.0 ? std::min(static_cast<double>(chordPx) / speedPxPerMs, cap) : cap;
    }

    std::vector<SNode> nodes(eShape shape, const SCursorNode& from, const SVec2f& to, double nowMs, double speedPxPerMs, double maxMs, float maxSpacingPx, const FEase& ease) {
        const SVec2f p0    = from.posPx;
        const SVec2f p2    = to;
        const float  chord = std::hypot(p2.x - p0.x, p2.y - p0.y);

        // Control point along the incoming velocity, for tangent continuity
        // at p0. Zero velocity (a fresh segment) falls back to the chord's
        // midpoint, which makes the quadratic Bezier degenerate to a straight
        // line -- no special-casing needed. Unused by LINE.
        SVec2f p1 = {(p0.x + p2.x) / 2.F, (p0.y + p2.y) / 2.F};
        if (const float speed = std::hypot(from.velocity.x, from.velocity.y); speed > 1e-6F)
            p1 = {p0.x + from.velocity.x / speed * chord * 0.5F, p0.y + from.velocity.y / speed * chord * 0.5F};

        // Length / max_spacing, rounded up. Clamped as a double so a huge
        // chord can't overflow the int.
        const int n = static_cast<int>(std::clamp(std::ceil(static_cast<double>(chord) / std::max(static_cast<double>(maxSpacingPx), 0.01)), 1.0, MAX_NODES));

        // The birth window: from now on, or from the newest node's birth if
        // the clock went backwards, so births stay monotonic.
        const double       start = std::max(from.birthTimeMs, nowMs);
        const double       span  = durationMs(chord, speedPxPerMs, maxMs);
        const double       last  = start + span;

        std::vector<SNode> out;
        out.reserve(n);
        for (int i = 1; i <= n; ++i) {
            const float t   = static_cast<float>(i) / static_cast<float>(n);
            SVec2f      pos = p2; // the last node lands on the target exactly
            if (i < n) {
                float s = t;
                if (ease) {
                    if (const float e = ease(t); std::isfinite(e))
                        s = std::clamp(e, -MAX_OVERSHOOT, 1.F + MAX_OVERSHOOT);
                }
                pos = shape == eShape::LINE ? lineAt(p0, p2, s) : curveAt(p0, p1, p2, s);
            }
            // Births spread evenly over the window, so the warp draws, and
            // later fades, along the path in real time.
            out.push_back({pos, i == n ? last : std::min(start + span * t, last)});
        }
        return out;
    }

    void CPlayback::start(std::vector<SNode> nodes, const SVec2f& from, double startMs, const SVec2f& anchorOffset) {
        m_queue.assign(nodes.begin(), nodes.end());
        m_head         = from;
        m_prevMs       = startMs;
        m_anchorOffset = anchorOffset;
    }

    void CPlayback::clear() {
        m_queue.clear();
    }

    bool CPlayback::empty() const {
        return m_queue.empty();
    }

    size_t CPlayback::pending() const {
        return m_queue.size();
    }

    void CPlayback::feed(ISource& source, const SNode& n, double birthMs, float minSpacingPx, float maxSpacingPx) {
        emitTo(source, n.posPx, m_prevMs, birthMs, minSpacingPx, maxSpacingPx, false, m_queue.empty());
        m_prevMs = birthMs;
        m_head   = n.posPx;
    }

    void CPlayback::drain(ISource& source, double nowMs, float minSpacingPx, float maxSpacingPx) {
        while (!m_queue.empty() && m_queue.front().birthMs <= nowMs) {
            const SNode n = m_queue.front();
            m_queue.pop_front();
            feed(source, n, n.birthMs, minSpacingPx, maxSpacingPx);
        }
    }

    void CPlayback::flush(ISource& source, double nowMs, float minSpacingPx, float maxSpacingPx) {
        while (!m_queue.empty()) {
            const SNode n = m_queue.front();
            m_queue.pop_front();
            feed(source, n, std::min(n.birthMs, nowMs), minSpacingPx, maxSpacingPx);
        }
    }

    double CPlayback::endMs() const {
        return m_queue.back().birthMs;
    }

    const SVec2f& CPlayback::head() const {
        return m_head;
    }

    const SVec2f& CPlayback::target() const {
        return m_queue.back().posPx;
    }

    std::optional<SVec2f> CPlayback::anchor() const {
        if (m_queue.empty())
            return std::nullopt;
        return SVec2f{.x = m_head.x + m_anchorOffset.x, .y = m_head.y + m_anchorOffset.y};
    }

    std::optional<STrailBounds> CPlayback::bounds() const {
        if (m_queue.empty())
            return std::nullopt;
        STrailBounds b{m_head.x, m_head.y, m_head.x, m_head.y};
        for (const auto& n : m_queue) {
            b.x1 = std::min(b.x1, n.posPx.x);
            b.y1 = std::min(b.y1, n.posPx.y);
            b.x2 = std::max(b.x2, n.posPx.x);
            b.y2 = std::max(b.y2, n.posPx.y);
        }
        return b;
    }

    void sample(ISource& source, CPlayback& playback, const SVec2f& pos, double fromMs, double nowMs, float minSpacingPx, float maxSpacingPx, bool& pendingBreak) {
        if (pendingBreak)
            playback.clear();
        else {
            playback.drain(source, nowMs, minSpacingPx, maxSpacingPx);
            if (!playback.empty()) {
                const SVec2f& t = playback.target();
                if (pos == t || std::hypot(pos.x - t.x, pos.y - t.y) < minSpacingPx)
                    return;
                playback.flush(source, nowMs, minSpacingPx, maxSpacingPx);
            }
        }
        if (emitTo(source, pos, fromMs, nowMs, minSpacingPx, maxSpacingPx, pendingBreak))
            pendingBreak = false;
    }

    void start(ISource& source, CPlayback& playback, eShape shape, const SVec2f& to, const SVec2f& anchorOffset, double nowMs, double speedPxPerMs, double maxMs,
               float minSpacingPx, float maxSpacingPx, const FEase& ease) {
        playback.flush(source, nowMs, minSpacingPx, maxSpacingPx);
        if (source.empty() || source.warp(to, nowMs))
            return;

        // A copy: the source's newest node changes as the queue drains.
        const SCursorNode from = source.newest();
        playback.start(nodes(shape, from, to, nowMs, speedPxPerMs, maxMs, maxSpacingPx, ease), from.posPx, std::max(nowMs, from.birthTimeMs), anchorOffset);
    }
}
