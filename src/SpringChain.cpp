#include "SpringChain.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

#include <hyprutils/animation/Spring.hpp>

namespace {
    // The chain is at rest when every point is this close to the pointer
    // target and this slow, the same rule core applies to its own springs
    // (hyprutils AnimatedVariable.cpp:129-137), at a scale of pixels: a
    // hundredth of a pixel is invisible. It applies to the chain as a whole:
    // snapping single points would cut the leading edge off a wave that is
    // still on its way down the chain.
    constexpr float SETTLE_POS_PX  = 0.05F;
    constexpr float SETTLE_VEL_PXS = 2.F;

    bool            near(const SVec2f& a, const SVec2f& b, float d) {
        return std::abs(a.x - b.x) <= d && std::abs(a.y - b.y) <= d;
    }

    // One axis of one point over a step, relative to the target.
    void advanceAxis(float& pos, float& vel, float target, const auto& link) {
        const float d = pos - target;
        pos           = target + (link.dd * d) + (link.dv * vel);
        vel           = (link.vd * d) + (link.vv * vel);
    }
}

CSpringChainSource::CSpringChainSource(size_t capacity, uint64_t seedBase) :
    m_capacity(std::max<size_t>(capacity, 1)), m_seedBase(seedBase), m_head(linkFor(m_headFreqHz, m_headDampingRatio)), m_link(linkFor(m_freqHz, m_dampingRatio)) {}

// advanceSpring (hyprutils Spring.cpp:9-57) is the closed-form solution of
// x'' = -(k/m) x - (c/m) x' for a constant target, in all three damping
// regimes, and linear in the initial displacement and velocity. So its
// result for a unit displacement and for a unit velocity are the columns of
// the step's exact transition. Mass 1, k = omega^2, c = 2 zeta omega. It
// solves around the rest value 1.0: value = 1 + displacement. The velocity
// column starts at one step-length per step (1/dt) so the displacement it
// produces is about 1, as precise in float as the other column.
CSpringChainSource::SLink CSpringChainSource::linkFor(float freqHz, float dampingRatio) {
    const float                              omega = 2.F * std::numbers::pi_v<float> * freqHz;
    const Hyprutils::Animation::SSpringCurve curve{.stiffness = omega * omega, .damping = 2.F * dampingRatio * omega, .mass = 1.F};
    const float                              dtSec = static_cast<float>(STEP_MS / 1000.0);
    const std::chrono::duration<float>       dt(dtSec);
    const float                              unitVel = 1.F / dtSec;

    float                                    fromD = 2.F, fromDVel = 0.F;     // displacement 1, at rest
    float                                    fromV = 1.F, fromVVel = unitVel; // no displacement, moving
    Hyprutils::Animation::advanceSpring(fromD, fromDVel, curve, dt);
    Hyprutils::Animation::advanceSpring(fromV, fromVVel, curve, dt);
    return SLink{.dd = fromD - 1.F, .dv = (fromV - 1.F) / unitVel, .vd = fromDVel, .vv = fromVVel / unitVel};
}

std::string_view CSpringChainSource::kind() const {
    return "spring";
}

void CSpringChainSource::reseed(const SVec2f& pos, double nowMs) {
    m_points.assign(m_capacity, SPoint{.pos = pos, .vel = {}});
    m_target    = pos;
    m_seeded    = true;
    m_activeMs  = nowMs;
    m_pendingMs = 0.0;
    m_unsettled = false;
    m_last      = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = {}, .distPx = 0.0, .seed = nodeSeed(m_seedBase, 0), .segmentStart = true};
    rebuild();
}

void CSpringChainSource::insert(const SVec2f& pos, double nowMs, bool segmentStart) {
    // A break detaches the trail from what came before. A chain has one
    // connected body, so there is nothing to fade: it restarts at pos.
    if (!m_seeded || segmentStart) {
        reseed(pos, nowMs);
        return;
    }

    SVec2f       velocity{};
    const double dt = nowMs - m_last.birthTimeMs;
    if (dt > 0.0)
        velocity = {static_cast<float>((pos.x - m_last.posPx.x) / dt), static_cast<float>((pos.y - m_last.posPx.y) / dt)};
    m_last = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = velocity, .distPx = 0.0, .seed = m_last.seed, .segmentStart = false};

    // Only the head's target moves; the rest follow on the next tick.
    m_target    = pos;
    m_activeMs  = nowMs;
    m_unsettled = true;
    rebuild();
}

void CSpringChainSource::tick(double nowMs, double dtMs) {
    // At rest nothing moves, and the time since isn't motion to catch up on.
    if (!m_seeded || !m_unsettled) {
        m_pendingMs = 0.0;
        return;
    }

    // Wall-clock time in fixed steps; the remainder waits for the next tick,
    // so a tick shorter than a step (a second monitor rendering the same
    // frame) integrates nothing.
    m_pendingMs += std::clamp(dtMs, 0.0, MAX_DT_MS);
    bool changed = false;
    while (m_unsettled && m_pendingMs >= STEP_MS) {
        m_pendingMs -= STEP_MS;
        changed = true;
        if (!step()) { // ran away: start over on the pointer
            reseed(m_target, nowMs);
            return;
        }
    }

    // Births follow activeMs: fresh while moving, aging once it stops.
    if (m_unsettled && m_activeMs != nowMs) {
        m_activeMs = nowMs;
        changed    = true;
    }

    if (changed)
        rebuild();
}

bool CSpringChainSource::step() {
    // The head chases the pointer. Every next point chases the midpoint of
    // its predecessor's position before and after this step: holding the
    // position after would let each link see its target half a step early
    // and shorten the chain by half a step per link, and holding the one
    // before would lengthen it as much.
    SVec2f before = m_target, after = m_target;
    bool   rest = true;
    for (size_t k = 0; k < m_points.size(); ++k) {
        auto&        p      = m_points[k];
        const SLink& link   = k == 0 ? m_head : m_link;
        const SVec2f target = k == 0 ? m_target : SVec2f{.x = 0.5F * (before.x + after.x), .y = 0.5F * (before.y + after.y)};
        before              = p.pos;
        advanceAxis(p.pos.x, p.vel.x, target.x, link);
        advanceAxis(p.pos.y, p.vel.y, target.y, link);
        after = p.pos;

        // Also false for a non-finite point, or every point once the head
        // is not finite.
        if (!(std::hypot(p.pos.x - m_points.front().pos.x, p.pos.y - m_points.front().pos.y) <= MAX_REACH_PX))
            return false;
        rest = rest && near(p.pos, m_target, SETTLE_POS_PX) && near(p.vel, {}, SETTLE_VEL_PXS);
    }

    if (rest)
        m_points.assign(m_points.size(), SPoint{.pos = m_target, .vel = {}});
    m_unsettled = !rest;
    return true;
}

bool CSpringChainSource::warp(const SVec2f& pos, double nowMs) {
    // The whole chain moves by the jump, velocities kept: the rope lands
    // where the pointer did and carries on as it was moving.
    const SVec2f offset{.x = pos.x - m_target.x, .y = pos.y - m_target.y};
    for (auto& p : m_points) {
        p.pos.x += offset.x;
        p.pos.y += offset.y;
    }
    m_target           = pos;
    m_last.posPx       = pos;
    m_last.birthTimeMs = nowMs;
    m_activeMs         = nowMs;
    rebuild();
    return true;
}

void CSpringChainSource::configure(const std::map<std::string, double>& values) {
    const auto get = [&](const char* name, float current) {
        const auto it = values.find(name);
        return it == values.end() ? current : static_cast<float>(it->second);
    };
    m_freqHz           = get("freq_hz", m_freqHz);
    m_dampingRatio     = get("damping_ratio", m_dampingRatio);
    m_headFreqHz       = get("head_freq_hz", m_headFreqHz);
    m_headDampingRatio = get("head_damping_ratio", m_headDampingRatio);
    m_link             = linkFor(m_freqHz, m_dampingRatio);
    m_head             = linkFor(m_headFreqHz, m_headDampingRatio);

    if (const float step = get("age_step_ms", m_ageStepMs); step != m_ageStepMs) {
        m_ageStepMs = step;
        if (m_seeded)
            rebuild();
    }
}

void CSpringChainSource::resize(size_t capacity) {
    capacity = std::max<size_t>(capacity, 1);
    if (capacity == m_capacity)
        return;
    m_capacity = capacity;

    if (!m_seeded) {
        ++m_generation;
        return;
    }

    // Keeps the head end. New points start on the old tail, at rest.
    SPoint tail = m_points.back();
    tail.vel    = {};
    m_points.resize(capacity, tail);
    rebuild();
}

void CSpringChainSource::clear() {
    m_seeded    = false;
    m_unsettled = false;
    m_points.clear();
    m_nodes.clear();
    ++m_generation;
}

void   CSpringChainSource::expire(double, double) {}

size_t CSpringChainSource::size() const {
    return m_points.size();
}

size_t CSpringChainSource::capacity() const {
    return m_capacity;
}

bool CSpringChainSource::empty() const {
    return !m_seeded;
}

const SCursorNode& CSpringChainSource::newest() const {
    return m_last;
}

uint64_t CSpringChainSource::generation() const {
    return m_generation;
}

double CSpringChainSource::newestBirthMs() const {
    return m_nodes.front().birthTimeMs;
}

void CSpringChainSource::orderedCopy(std::vector<SGpuNode>& out, double refMs) const {
    out.clear();
    out.reserve(m_nodes.size());
    for (size_t k = m_nodes.size(); k-- > 0;) { // tail first
        const auto& n = m_nodes[k];
        out.push_back(SGpuNode{
            .posPx    = n.posPx,
            .birthMs  = static_cast<float>(n.birthTimeMs - refMs),
            .velocity = n.velocity,
            .distPx   = static_cast<float>(n.distPx),
            .bits     = (n.seed << 1) | (n.segmentStart ? GPU_BIT_SEGMENT_START : 0u),
        });
    }
}

bool CSpringChainSource::needsContinuousUpload() const {
    return m_unsettled;
}

size_t CSpringChainSource::visibleCount(double nowMs, double fadeMs) const {
    return visibleCountOf(m_nodes.size(), [this](size_t i) -> const SCursorNode& { return m_nodes[i]; }, nowMs, fadeMs);
}

std::optional<STrailBounds> CSpringChainSource::visibleBounds(double nowMs, double fadeMs, bool includeOlderNode) const {
    return visibleBoundsOf(m_nodes.size(), [this](size_t i) -> const SCursorNode& { return m_nodes[i]; }, nowMs, fadeMs, includeOlderNode);
}

bool CSpringChainSource::isSettled(double nowMs, double fadeMs) const {
    return !m_unsettled && visibleCount(nowMs, fadeMs) == 0;
}

void CSpringChainSource::rebuild() {
    const size_t n = m_points.size();
    m_nodes.resize(n);

    double dist = 0.0; // from the tail, where the one segment starts
    for (size_t k = n; k-- > 0;) {
        const auto& p = m_points[k];
        if (k + 1 < n)
            dist += std::hypot(p.pos.x - m_points[k + 1].pos.x, p.pos.y - m_points[k + 1].pos.y);
        m_nodes[k] = SCursorNode{
            .posPx        = p.pos,
            .birthTimeMs  = m_activeMs - static_cast<double>(k) * m_ageStepMs,
            .velocity     = {p.vel.x / 1000.F, p.vel.y / 1000.F}, // px/s -> px/ms
            .distPx       = dist,
            .seed         = nodeSeed(m_seedBase, k),
            .segmentStart = k + 1 == n,
        };
    }
    ++m_generation;
}
