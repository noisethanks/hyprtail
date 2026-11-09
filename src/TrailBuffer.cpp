#include "TrailBuffer.hpp"

#include <algorithm>
#include <cmath>

CTrailRing::CTrailRing(size_t capacity, uint64_t seedBase) : m_nodes(std::max<size_t>(capacity, 1)), m_seedBase(seedBase) {}

std::string_view CTrailRing::kind() const {
    return "pointer";
}

void CTrailRing::insert(const SVec2f& pos, double nowMs, bool segmentStart) {
    segmentStart = segmentStart || m_count == 0;

    SVec2f velocity{};
    double dist = 0.0; // restarts at every segment start
    if (!segmentStart) {
        const auto&  prev = newest();
        const double dt   = nowMs - prev.birthTimeMs;
        if (dt > 0.0)
            velocity = {static_cast<float>((pos.x - prev.posPx.x) / dt), static_cast<float>((pos.y - prev.posPx.y) / dt)};
        dist = prev.distPx + std::hypot(pos.x - prev.posPx.x, pos.y - prev.posPx.y);
    }

    // Full: the oldest node is overwritten. expire() leaves at most one node
    // older than keepMs, so that is only a drop if it is younger.
    if (m_count == m_nodes.size() && nowMs - at(m_count - 1).birthTimeMs < m_keepMs)
        ++m_drops;

    const auto seed = nodeSeed(m_seedBase, m_inserted++); // 31 bits

    m_nodes[m_head] = SCursorNode{.posPx = pos, .birthTimeMs = nowMs, .velocity = velocity, .distPx = dist, .seed = seed, .segmentStart = segmentStart};
    m_head          = (m_head + 1) % m_nodes.size();
    m_count         = std::min(m_count + 1, m_nodes.size());
    ++m_generation;
}

void CTrailRing::tick(double, double) {}

bool CTrailRing::warp(const SVec2f&, double) {
    return false;
}

void CTrailRing::configure(const std::map<std::string, double>&) {}

void CTrailRing::resize(size_t capacity) {
    capacity = std::max<size_t>(capacity, 1);
    if (capacity == m_nodes.size())
        return;

    const size_t             keep   = std::min(m_count, capacity);
    const size_t             cap    = m_nodes.size();
    const size_t             oldest = (m_head + cap - keep) % cap; // oldest of the kept ones
    std::vector<SCursorNode> nodes(capacity);
    for (size_t i = 0; i < keep; ++i)
        nodes[i] = m_nodes[(oldest + i) % cap];

    m_nodes = std::move(nodes);
    m_count = keep;
    m_head  = keep % capacity;
    ++m_generation;
}

void CTrailRing::clear() {
    m_head  = 0;
    m_count = 0;
    ++m_generation;
}

void CTrailRing::expire(double nowMs, double keepMs) {
    m_keepMs          = keepMs;
    const size_t from = m_count;
    while (m_count >= 2 && nowMs - at(m_count - 2).birthTimeMs >= keepMs)
        --m_count; // the oldest is at m_head - m_count
    if (m_count != from)
        ++m_generation;
}

uint64_t CTrailRing::drops() const {
    return m_drops;
}

size_t CTrailRing::size() const {
    return m_count;
}

size_t CTrailRing::capacity() const {
    return m_nodes.size();
}

bool CTrailRing::empty() const {
    return m_count == 0;
}

const SCursorNode& CTrailRing::newest() const {
    return m_nodes[(m_head + m_nodes.size() - 1) % m_nodes.size()];
}

uint64_t CTrailRing::generation() const {
    return m_generation;
}

double CTrailRing::newestBirthMs() const {
    return newest().birthTimeMs;
}

bool CTrailRing::needsContinuousUpload() const {
    return false;
}

bool CTrailRing::isSettled(double nowMs, double fadeMs) const {
    return visibleCount(nowMs, fadeMs) == 0;
}

void CTrailRing::orderedCopy(std::vector<SGpuNode>& out, double refMs) const {
    out.clear();
    out.reserve(m_count);

    const size_t cap    = m_nodes.size();
    const size_t oldest = (m_head + cap - m_count) % cap;
    for (size_t i = 0; i < m_count; ++i) {
        const auto& n = m_nodes[(oldest + i) % cap];
        out.push_back(SGpuNode{
            .posPx    = n.posPx,
            .birthMs  = static_cast<float>(n.birthTimeMs - refMs),
            .velocity = n.velocity,
            .distPx   = static_cast<float>(n.distPx),
            .bits     = (n.seed << 1) | (n.segmentStart ? GPU_BIT_SEGMENT_START : 0u),
        });
    }
}

const SCursorNode& CTrailRing::at(size_t i) const {
    const size_t cap = m_nodes.size();
    return m_nodes[(m_head + cap - 1 - i) % cap];
}

size_t CTrailRing::visibleCount(double nowMs, double fadeMs) const {
    return visibleCountOf(m_count, [this](size_t i) -> const SCursorNode& { return at(i); }, nowMs, fadeMs);
}

std::optional<STrailBounds> CTrailRing::visibleBounds(double nowMs, double fadeMs, bool includeOlderNode) const {
    return visibleBoundsOf(m_count, [this](size_t i) -> const SCursorNode& { return at(i); }, nowMs, fadeMs, includeOlderNode);
}

bool emitTo(ISource& s, const SVec2f& pos, double fromMs, double nowMs, float minSpacingPx, float maxSpacingPx, bool segmentStart, bool land) {
    if (s.empty()) {
        s.insert(pos, nowMs, true);
        return true;
    }

    // A copy: the inserts below replace what newest() refers to.
    const SCursorNode prev = s.newest();
    const float       dx = pos.x - prev.posPx.x, dy = pos.y - prev.posPx.y;
    const double      d = std::hypot(dx, dy);
    if (d == 0.0 || (d < minSpacingPx && !land))
        return false;
    if (segmentStart) {
        s.insert(pos, nowMs, true);
        return true;
    }

    const double end   = std::max(nowMs, prev.birthTimeMs);
    const double start = std::clamp(fromMs, prev.birthTimeMs, end);
    const double step  = std::max({static_cast<double>(maxSpacingPx), static_cast<double>(minSpacingPx), 0.01});

    // Fill points at step, 2 step, ... strictly before pos: the last `capacity`
    // of them. The count is clamped as a double so a huge step can't
    // overflow it.
    const auto fill  = static_cast<uint64_t>(std::clamp(std::ceil(d / step) - 1.0, 0.0, 1e15));
    const auto keep  = static_cast<uint64_t>(s.capacity());
    const auto first = fill > keep ? fill - keep + 1 : uint64_t{1};
    double     last  = 0.0; // distance of the last fill point
    for (uint64_t k = first; k <= fill; ++k) {
        const double f = static_cast<double>(k) * step / d;
        s.insert({prev.posPx.x + static_cast<float>(dx * f), prev.posPx.y + static_cast<float>(dy * f)}, start + ((end - start) * f), false);
        last = static_cast<double>(k) * step;
    }
    if (land || d - last >= minSpacingPx)
        s.insert(pos, end, false);
    return true;
}
