#pragma once

#include <optional>
#include <string>

#include "TrailResolve.hpp"

// Loading a trail: resolution (names, paths, shader stages) is
// TrailResolve.hpp, the manifest grammar Manifest.hpp. This adds reporting
// and the fallback.
namespace hyprtail::trail {
    // Resolves `name`: "builtin:<name>" or the path of a .conf file (see
    // TrailResolve.hpp). Any failure (unknown builtin, a bare name or other value that
    // isn't a .conf path, no such file, parse error, an unrecognized shader
    // reference, an unresolvable path) is reported (diag, "trail:<name>")
    // and falls back to the embedded "builtin:ink" manifest, which is
    // guaranteed to parse -- it ships with the plugin.
    //
    // One exception: a user preset that parses but names shader files that
    // aren't on disk is a warning, not an error. With `haveActive` (a trail
    // is already showing) it returns nullopt and the caller keeps what it
    // has; without, it falls back like the others.
    std::optional<SResolved> load(const std::string& name, bool haveActive);
}
