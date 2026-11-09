#include "Source.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

#include "SpringChain.hpp"

namespace hyprtail::source {
    namespace {
        std::vector<params::SDecl> parseDecls(std::initializer_list<std::string> texts) {
            std::vector<params::SDecl> out;
            for (const auto& t : texts) {
                auto d = params::parseDecl(t);
                if (d)
                    out.push_back(std::move(*d));
            }
            return out; // the texts are fixed; unit tests check none was dropped
        }

        std::string capacityDecl() {
            return std::format("int capacity {} 2 4096 // rope length, points", CAPACITY);
        }

        std::string minSpacingDecl() {
            return std::format("float min_spacing {} 0 256 // smallest move that adds a point, px", MIN_SPACING);
        }

        const std::vector<params::SDecl>& pointerDecls() {
            static const auto d = parseDecls({
                minSpacingDecl(),
                std::format("float max_spacing {} 0.5 256 // largest gap between points, px; lower = smoother curves", MAX_SPACING),
                std::format("float warp_speed {} 0.05 100 // how fast a warp is drawn, px/ms", WARP_SPEED),
                std::format("float warp_max_ms {} 1 5000 // longest a warp takes to draw, ms", WARP_MAX_MS),
                "vec2 warp_p1 0,0 -1 2 // warp easing curve, first control point x,y",
                "vec2 warp_p2 1,1 -1 2 // warp easing curve, second control point x,y",
            });
            return d;
        }

        // Spring frequencies: 100 Hz keeps omega * step under 1 rad at the
        // chain's fixed step (CSpringChainSource::STEP_MS).
        constexpr float MIN_FREQ_HZ       = 0.5F;
        constexpr float MAX_FREQ_HZ       = 100.F;
        constexpr float MAX_DAMPING_RATIO = 10.F;

        // The head's settings a preset leaves unset follow the chain's, so
        // setting only freq_hz and damping_ratio gives a uniform chain.
        constexpr std::array<std::pair<const char*, const char*>, 2> SPRING_HEAD_FOLLOWS = {{{"head_freq_hz", "freq_hz"}, {"head_damping_ratio", "damping_ratio"}}};

        const std::vector<params::SDecl>&                            springDecls() {
            static const auto d = parseDecls({
                capacityDecl(),
                minSpacingDecl(),
                std::format("float freq_hz {} {} {} // rope stiffness, Hz; lower = lazier rope, longer lag", spring::FREQ_HZ, MIN_FREQ_HZ, MAX_FREQ_HZ),
                std::format("float damping_ratio {} {} {} // rope bounce: 1 = none, lower = more wobble", spring::DAMPING_RATIO, spring::MIN_DAMPING_RATIO, MAX_DAMPING_RATIO),
                std::format("float head_freq_hz {} {} {} // head stiffness, Hz; lower = slower, wider swings", spring::FREQ_HZ, MIN_FREQ_HZ, MAX_FREQ_HZ),
                std::format("float head_damping_ratio {} {} {} // head bounce: lower = more swings", spring::DAMPING_RATIO, spring::MIN_HEAD_DAMPING_RATIO, MAX_DAMPING_RATIO),
                std::format("float age_step_ms {} 0 1000 // tail fade per point, ms; higher = shorter tail", spring::AGE_STEP_MS),
            });
            return d;
        }
    }

    bool known(std::string_view kind) {
        return kind == "pointer" || kind == "spring";
    }

    std::string_view kindList() {
        return "pointer, spring";
    }

    const std::vector<params::SDecl>& decls(std::string_view kind) {
        static const std::vector<params::SDecl> none;
        return kind == "spring" ? springDecls() : kind == "pointer" ? pointerDecls() : none;
    }

    std::unique_ptr<ISource> make(std::string_view kind, size_t capacity, uint64_t seedBase) {
        if (kind == "spring")
            return std::make_unique<CSpringChainSource>(capacity, seedBase);
        return std::make_unique<CTrailRing>(capacity, seedBase);
    }

    SSettings resolve(std::string_view kind, const std::map<std::string, std::string>& defaults, const std::map<std::string, std::string>& overrides, std::string& problems) {
        std::vector<std::pair<params::SDecl, params::SValue>> all;
        for (const auto& d : decls(kind))
            all.emplace_back(d, d.def);

        params::applyOverrides(all, defaults, "source", problems);
        params::applyOverrides(all, overrides, "source", problems);

        const auto controlPoint = [](const params::SValue& v) { return SVec2f{.x = std::clamp(static_cast<float>(v.x), 0.F, 1.F), .y = static_cast<float>(v.y)}; };

        SSettings  out;
        for (const auto& [decl, value] : all) {
            if (decl.name == "capacity")
                out.capacity = static_cast<size_t>(value.x);
            else if (decl.name == "min_spacing")
                out.minSpacingPx = static_cast<float>(value.x);
            else if (decl.name == "max_spacing")
                out.maxSpacingPx = static_cast<float>(value.x);
            else if (decl.name == "warp_speed")
                out.warpSpeed = static_cast<float>(value.x);
            else if (decl.name == "warp_max_ms")
                out.warpMaxMs = static_cast<float>(value.x);
            else if (decl.name == "warp_p1")
                out.warpP1 = controlPoint(value);
            else if (decl.name == "warp_p2")
                out.warpP2 = controlPoint(value);
            else if (const auto s = params::scalar(value))
                out.values[decl.name] = *s;
        }
        // Unbounded for a kind that doesn't declare it, and never below
        // min_spacing.
        if (std::ranges::none_of(decls(kind), [](const auto& d) { return d.name == "max_spacing"; }))
            out.maxSpacingPx = std::numeric_limits<float>::infinity();
        out.maxSpacingPx = std::max(out.maxSpacingPx, out.minSpacingPx);
        if (kind == "spring")
            for (const auto& [head, chain] : SPRING_HEAD_FOLLOWS)
                if (!defaults.contains(head) && !overrides.contains(head))
                    out.values[head] = out.values[chain];
        return out;
    }
}
