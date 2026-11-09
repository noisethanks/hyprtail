#include "Trail.hpp"

#include <format>
#include <optional>
#include <string>
#include <vector>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;

namespace hyprtail::trail {
    namespace {
        constexpr const char* FALLBACK_TRAIL = "builtin:ink";

        // Absolute last resort if even the embedded "ink" manifest somehow
        // fails to parse: a single trail layer on pragma defaults. Never
        // expected to actually run -- it exists so a mistake in this
        // codebase's own built-ins degrades instead of throwing/crashing.
        SResolved hardcodedFallback() {
            SLayerSpec trail{.name = "trail", .vertBuiltin = "shaders/taper.vert", .fragBuiltin = "shaders/gradient.frag"};
            return SResolved{.name = FALLBACK_TRAIL, .description = "fallback", .layers = {std::move(trail)}};
        }
    }

    std::optional<SResolved> load(const std::string& name, bool haveActive) {
        const auto               key = "trail:" + name;
        std::vector<std::string> missing;
        auto                     r = resolve(name, missing);

        if (r && missing.empty()) {
            diag::resetKey(key);
            return std::move(*r);
        }

        if (!r)
            diag::report(eSeverity::ERR, key, std::format("trail \"{}\": {}\nUsing \"{}\" instead.", name, r.error(), FALLBACK_TRAIL));
        else {
            // The trail itself is fine, but shader files it names aren't on
            // disk: degraded, not off, so a warning, and what is showing
            // stays (nothing is showing yet at startup: the fallback).
            std::string list;
            for (const auto& m : missing)
                list += "\n  " + m;
            diag::report(eSeverity::WARN, key,
                         std::format("trail \"{}\": shader file(s) not found:{}\n{}", name, list,
                                     haveActive ? "Keeping the current trail." : std::format("Using \"{}\" instead.", FALLBACK_TRAIL)));
            if (haveActive)
                return std::nullopt;
        }

        if (name == FALLBACK_TRAIL)
            return hardcodedFallback();

        std::vector<std::string> none;
        if (auto fallback = resolve(FALLBACK_TRAIL, none))
            return std::move(*fallback);
        return hardcodedFallback();
    }
}
