#pragma once

#include <expected>
#include <map>
#include <string>
#include <vector>

#include "Manifest.hpp"
#include "Source.hpp"

// Resolving a trail name to its layers, without Hyprland, so tests/unit can
// load a preset from disk. Reporting and fallback are Trail.hpp's load().
//
// Two namespaces, no shadowing. A trail is "builtin:<name>", an embedded
// built-in, or the path of a .conf file with its extension, e.g.
// "presets/ink.conf" (cfg::resolveShaderPath(): relative ones against
// <hyprtail root>, cfg::hyprtailRoot(): $XDG_CONFIG_HOME/hypr/hyprtail,
// fallback ~/.config/hypr/hyprtail; "~" and absolute as given). A bare
// "<name>" is an error.
//
// A shader stage is "builtin:<file>", the embedded "shaders/<file>", in any
// trail. Otherwise it is a path relative to the same root: in a built-in
// trail it is looked up in the embedded shader table by that string and
// never touches the disk; in a user preset it is a file, and a missing one
// is a warning (Trail.hpp load()).
namespace hyprtail {
    struct SLayerSpec {
        std::string                        name;
        std::string                        vertBuiltin, fragBuiltin; // shader::builtin() keys ("shaders/taper.vert"); the slot's always-safe identity
        std::string                        vertPath, fragPath;       // trail's own per-stage override, absolute path, "" = use *Builtin above
        std::map<std::string, std::string> defaults;                 // param name -> value text, from the trail manifest

        bool                               operator==(const SLayerSpec&) const = default;
    };
}

namespace hyprtail::trail {
    // A resolved trail, ready for CLayer construction: name, description,
    // and up to 4 SLayerSpecs with vertex/fragment already resolved to an
    // embedded shader::builtin() key ("shaders/taper.vert") or an absolute
    // path.
    struct SResolved {
        std::string                        name, description;
        std::vector<SLayerSpec>            layers;
        std::string                        sourceKind{source::DEFAULT_KIND};
        std::map<std::string, std::string> sourceDefaults; // source settings from the manifest

        bool                               operator==(const SResolved&) const = default;
    };

    // Resolves `name` (see above), never falling back to a built-in. Shader
    // files a user preset names that aren't on disk are added to `missing`
    // as resolved paths; the trail itself still resolves.
    std::expected<SResolved, std::string> resolve(const std::string& name, std::vector<std::string>& missing);
}
