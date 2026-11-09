#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Params.hpp"
#include "ShaderPair.hpp"

// A layer's parameter values: the program's
// declarations, the trail manifest's per-layer defaults and the `params`
// string, resolved. No GL: CLayer (Layer.hpp) runs this whenever its program
// or its overrides change.
namespace hyprtail {
    // Values for the active program, recomputed when the program or the
    // overrides change.
    struct SResolved {
        uint64_t                                              generation = UINT64_MAX; // slot generation resolved for
        uint64_t                                              overrides  = UINT64_MAX; // overrides version resolved for
        std::vector<std::pair<params::SDecl, params::SValue>> values;                  // the program's params

        // Reserved lifecycle parameters.
        bool   enabled        = true;
        bool   drawWhenHidden = true;
        double fadeMs         = 500.0;
        double startMs        = 500.0;
        double durationMs     = 1500.0;

        // Largest padding expression of the program, px, and the layer's
        // damage_padding on top (reserved). Their sum is the layer's reach.
        float paddingPx       = 0.F;
        float damagePaddingPx = 0.F;

        // Instanced topology: copies drawn per node (K), 1..64.
        // 0 for other topologies.
        int instances = 0;
    };

    // Reserved defaults, then the program's own; `manifest` (the trail's
    // per-layer defaults) on top, then `paramString` (the `params` setting) on
    // top of that, so it wins. Bad names and values are appended to
    // `problems` and keep the earlier value. generation and overrides are
    // left for the caller.
    SResolved resolveValues(const SProgramInfo& info, const std::map<std::string, std::string>& manifest, const std::map<std::string, std::string>& paramString,
                            std::string& problems);

    // "enabled" from the overrides alone (`paramString` over `manifest`), usable
    // before any program is active. Unset or unparsable: enabled.
    bool enabledSetting(const std::map<std::string, std::string>& manifest, const std::map<std::string, std::string>& paramString);
}
