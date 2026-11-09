#pragma once

#include <vector>

#include "TrailBuffer.hpp"

// Defaults and limits of the spring source's settings (`source:<name>`).
// Every link is a damped spring given by its natural frequency (Hz) and
// damping ratio (1 = critical). The frequencies are high because every point
// chases the one before it: the tail's lag adds up over the chain, about
// damping_ratio / (pi * freq_hz) per link, so each link has to answer in a
// few ms for the whole chain to lag by a few hundred.
namespace spring {
    constexpr float FREQ_HZ       = 40.F;
    constexpr float DAMPING_RATIO = 0.9F;
    constexpr float AGE_STEP_MS   = 10.F;

    // The chain is a cascade of identical second-order low-pass links. Below
    // a damping ratio of 1/sqrt(2) each link's gain peaks above 1, and the
    // tail's response grows like that peak to the power of the chain's
    // length. 0.71 is the first two-digit value at or above the edge. The
    // head drives the chain without being driven by a link like itself, so
    // it may bounce: its peak is not compounded.
    constexpr float MIN_DAMPING_RATIO      = 0.71F;
    constexpr float MIN_HEAD_DAMPING_RATIO = 0.2F;
}

// A chain of `capacity` points that chase each other (source `spring`).
// The head point chases the pointer, every other point chases the
// one before it, each axis an independent damped spring. The head has its
// own frequency and damping ratio (head_freq_hz, head_damping_ratio), the
// rest share the chain's (freq_hz, damping_ratio).
// Unlike the pointer history nothing is appended: the points are fixed, so
// size() is the capacity once the chain has been seeded by the first insert.
//
// Time: the chain integrates wall-clock time in fixed steps of STEP_MS,
// carrying the remainder to the next tick, so its motion doesn't depend on
// how often or how regularly it is ticked. A step is exact per link for a
// constant target (hyprutils advanceSpring); each link's target over a step
// is the midpoint of its predecessor's position before and after that step.
//
// Point data for the shaders, in the ring's conventions so every geometry
// shader works unchanged: the oldest node is the tail and the newest the
// head, only the tail starts a segment, velocity is px/ms, distPx
// accumulates from the tail. A point has no birth of its own: point k is
// given birth time (activeMs - k * age_step_ms), where activeMs is the last
// time the chain was moving or the pointer inserted a target. So the chain
// is fresh while it moves, tapers toward the tail (the tail is older), and
// fades as one once it stops.
class CSpringChainSource final : public ISource {
  public:
    CSpringChainSource(size_t capacity, uint64_t seedBase);

    // The fixed integration step. Small against the fastest allowed link
    // (freq_hz 100: omega * step = 0.63 rad), so holding a link's target at
    // its predecessor's midpoint for a step stays accurate, and cheap: at
    // most 33 steps per tick over 64 points is a few thousand multiply-adds.
    static constexpr double STEP_MS = 1.0;

    // Longest time one tick integrates. After a gap with no renders the
    // chain would otherwise cross the whole distance to the pointer in one
    // go instead of following it.
    static constexpr double MAX_DT_MS = 33.0;

    // A point farther than this from the head means the chain ran away
    // (a setting or a jump the cascade can't absorb, or a non-finite value):
    // it is re-seeded on the pointer. Larger than any distance a rope lags
    // over a desktop of several monitors.
    static constexpr float MAX_REACH_PX = 8192.F;

    std::string_view       kind() const override;
    void                   insert(const SVec2f& pos, double nowMs, bool segmentStart) override;
    void                   tick(double nowMs, double dt) override;
    bool                   warp(const SVec2f& pos, double nowMs) override;
    void                   configure(const std::map<std::string, double>& values) override;
    void                   resize(size_t capacity) override;
    void                   clear() override;
    // The chain's length is its capacity: nothing expires.
    void                        expire(double nowMs, double keepMs) override;

    size_t                      size() const override;
    size_t                      capacity() const override;
    bool                        empty() const override;
    const SCursorNode&          newest() const override;
    uint64_t                    generation() const override;
    double                      newestBirthMs() const override;
    void                        orderedCopy(std::vector<SGpuNode>& out, double refMs) const override;
    bool                        needsContinuousUpload() const override;
    size_t                      visibleCount(double nowMs, double fadeMs) const override;
    std::optional<STrailBounds> visibleBounds(double nowMs, double fadeMs, bool includeOlderNode = true) const override;
    bool                        isSettled(double nowMs, double fadeMs) const override;

  private:
    struct SPoint {
        SVec2f pos;
        SVec2f vel; // px/s, the point's own velocity (the spring's state)
    };

    // One link's exact motion over STEP_MS, linear in its state relative to
    // the target: (d, v) -> (dd * d + dv * v, vd * d + vv * v).
    struct SLink {
        float dd = 1.F, dv = 0.F, vd = 0.F, vv = 1.F;
    };

    static SLink linkFor(float freqHz, float dampingRatio);

    // Every point at pos, at rest.
    void reseed(const SVec2f& pos, double nowMs);

    // One STEP_MS. False if the chain ran away (MAX_REACH_PX).
    bool step();

    // Rebuilds m_nodes from the points and activeMs, and bumps the generation.
    void                     rebuild();

    size_t                   m_capacity = 0;
    uint64_t                 m_seedBase = 0;
    bool                     m_seeded   = false; // false until the first insert (and after clear)
    std::vector<SPoint>      m_points;           // head first
    std::vector<SCursorNode> m_nodes;            // head first, derived from m_points
    SVec2f                   m_target;           // where the head is chasing
    SCursorNode              m_last{};           // the last insert, newest()
    double                   m_activeMs   = 0.0;
    double                   m_pendingMs  = 0.0;   // ticked time not yet integrated, under STEP_MS
    bool                     m_unsettled  = false; // a point is still moving, or chasing a target it hasn't reached
    uint64_t                 m_generation = 0;
    float                    m_freqHz = spring::FREQ_HZ, m_dampingRatio = spring::DAMPING_RATIO;
    float                    m_headFreqHz = spring::FREQ_HZ, m_headDampingRatio = spring::DAMPING_RATIO;
    SLink                    m_head, m_link;
    float                    m_ageStepMs = spring::AGE_STEP_MS;
};
