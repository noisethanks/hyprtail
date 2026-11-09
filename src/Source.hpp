#pragma once

#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Params.hpp"
#include "TrailBuffer.hpp"

// Trail sources by name: the values a manifest's `source` key can
// take, the settings each one declares (`source:<name>` keys and `params`
// entries, same value syntax and range checks as shader parameters), and the
// factory. No Hyprland headers.
//
//   pointer   real pointer history (CTrailRing): min_spacing, max_spacing,
//             warp_speed, warp_max_ms, warp_p1, warp_p2
//   spring    a chain of points chasing each other (CSpringChainSource):
//             capacity, min_spacing, freq_hz, damping_ratio, head_freq_hz,
//             head_damping_ratio, age_step_ms
//
// capacity       the spring chain's length, 2..4096. The pointer history has
//                no such key: it holds RING_CEILING nodes and keeps them by
//                age, as long as its longest-fading layer draws them
//                (CTrailRing::expire).
// min_spacing    logical px the pointer travels before a point is taken, 0..256
// max_spacing    logical px two consecutive points are at most apart, 0.5..256:
//                a longer step (fast motion, a low refresh rate, a warp) is
//                filled with points along it (emitTo, TrailBuffer.hpp).
//                Below min_spacing it counts as min_spacing.
// freq_hz        natural frequency of every link after the head, 0.5..100
// damping_ratio  damping ratio of those links (1 = critical), 0.71..10:
//                lower makes the chain's cascade amplify (SpringChain.hpp)
// head_freq_hz, head_damping_ratio
//                the head link's, which chases the pointer; same frequency
//                range, ratio 0.2..10. Unset, they are the chain's values.
// age_step_ms    ms of age per point, for fading and taper only, 0..1000
// warp_speed     logical px/ms a warp draws at, on average (warp = "line" |
//                "curve"), 0.05..100: it takes chord / warp_speed ms to draw
//                from origin to target, and fades in the same order
// warp_max_ms    the longest a warp takes to draw, ms, 1..5000
// warp_p1/p2     the inner control points of the bezier that times a warp,
//                "x,y", each coordinate -1..2 (hl.curve's range); the ends
//                are (0,0) and (1,1). The defaults, 0,0 and 1,1, are linear.
//
// The warp keys belong to `pointer` only: on a line or curve warp a spring
// chain moves by the jump as one body. With the plugin's warp = "break" they
// have no effect.
namespace hyprtail::source {
    // The source of a trail that names none.
    constexpr std::string_view DEFAULT_KIND = "pointer";

    // The layer-key prefix that addresses the source in a manifest and in
    // the `params` string: "source:<name>". Not usable as a layer name.
    constexpr std::string_view KEY_PREFIX = "source";

    bool                       known(std::string_view kind);
    std::string_view           kindList(); // "pointer, spring"

    // The settings `kind` declares; empty for an unknown kind.
    const std::vector<params::SDecl>& decls(std::string_view kind);

    // Declared defaults of the settings above (CAPACITY: spring's).
    constexpr size_t CAPACITY    = 64;
    constexpr float  MIN_SPACING = 2.F;
    constexpr float  MAX_SPACING = 8.F;
    constexpr float  WARP_SPEED  = 4.F;
    constexpr float  WARP_MAX_MS = 250.F;

    // A source's resolved settings. The plugin applies capacity, the spacing
    // and the warp keys itself; `values` holds the rest, the kind's own, for
    // ISource::configure. A kind that doesn't declare a key gets its default,
    // except max_spacing: unbounded for a kind without it (a spring chain's
    // insert moves its head, there is no step to fill), and capacity:
    // RING_CEILING for a kind without it (the pointer history).
    struct SSettings {
        size_t                        capacity     = RING_CEILING;
        float                         minSpacingPx = MIN_SPACING;
        float                         maxSpacingPx = MAX_SPACING; // infinite for a kind without it
        float                         warpSpeed    = WARP_SPEED;  // px/ms
        float                         warpMaxMs    = WARP_MAX_MS;
        SVec2f                        warpP1{.x = 0.F, .y = 0.F}, warpP2{.x = 1.F, .y = 1.F}; // x clamped to 0..1, see resolve()
        std::map<std::string, double> values;

        bool                          operator==(const SSettings&) const = default;
    };

    // A new source of `kind` (which must be known) holding `capacity` nodes.
    std::unique_ptr<ISource> make(std::string_view kind, size_t capacity, uint64_t seedBase);

    // Declared defaults, then the manifest's `defaults`, then the `params`
    // string's `overrides`, each by name. Unknown names and bad values
    // (a control point coordinate outside -1..2 among them) are described in
    // `problems` ("\n  name: ...", see params::applyOverrides) and leave the
    // earlier value. Control point x is then clamped to 0..1: with both
    // inner x in 0..1 a cubic bezier's x never decreases, which
    // CBezierCurve::getYForPoint's binary search over its baked points
    // assumes (hyprutils BezierCurve.cpp:61-77).
    SSettings resolve(std::string_view kind, const std::map<std::string, std::string>& defaults, const std::map<std::string, std::string>& overrides, std::string& problems);
}
