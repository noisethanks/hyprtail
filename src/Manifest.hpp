#pragma once

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Source.hpp"

// Trail manifests: the file format of built-in trails and user
// presets, plain "key = value" lines, '#' to end-of-line comments (also
// after a value: values have no quoting, so any '#' ends one), blank lines
// ignored. Layer keys are prefixed "<layer>:", as in
// "plugin:hyprtail:...".
//
//   contract    = 2                          required, exactly once
//   description = ...                        optional, at most once
//   layers      = <name>[, <name>...]         required, 1-4, no duplicates
//   source      = pointer | spring            optional, at most once, default pointer
//
//   source:<name>    = <value>                a setting of the source (Source.hpp)
//   <layer>:vertex   = a shader path, e.g. shaders/taper.vert (or builtin:taper.vert)
//   <layer>:fragment = a shader path, e.g. shaders/gradient.frag (or builtin:gradient.frag)
//   <layer>:<name>   = <value>                a parameter default
//
// The source is the one thing every layer of a trail draws from, so it is
// declared once, by the manifest, and its settings live under the reserved
// layer name "source" (a layer can't be called that).
//
// Hyprland-free: the grammar and the embedded built-ins. Resolving a trail
// name and its shader stages is TrailResolve.hpp.
namespace hyprtail::trail {
    // Manifest grammar version, unrelated to shader::CONTRACT_VERSION:
    // this one just lets a future breaking change to the manifest format's own
    // syntax name itself, the same way shader contracts do.
    constexpr int CONTRACT_VERSION = 2;

    // Parses the plain-text grammar only: purely structural checks (unknown
    // top-level key, a "<layer>:" key for a layer not in `layers`, a bad or
    // missing contract, `layers` empty/duplicated/over 4). What "vertex" and
    // "fragment" resolve to, and whether a parameter name the manifest sets
    // is one the layer's shader actually declares, need the shader itself
    // (a built-in name lookup, or a compiled program's declared params) and
    // are checked one level up, in load() and CLayer::resolve() respectively.
    struct SManifest {
        std::string                                               description;
        std::vector<std::string>                                  layers;    // draw order, first = bottom
        std::map<std::string, std::map<std::string, std::string>> layerKeys; // layer name -> (key -> raw value text, incl. "vertex"/"fragment")
        std::string                                               sourceKind{source::DEFAULT_KIND};
        std::map<std::string, std::string>                        sourceKeys; // "source:<name>" -> raw value text

        bool                                                      operator==(const SManifest&) const = default;
    };

    std::expected<SManifest, std::string> parse(std::string_view text);

    // The embedded built-in trails (hyprtail/presets/<name>.conf, #embed):
    // the manifest text of "builtin:<name>", empty if there is none.
    std::string_view builtinManifest(std::string_view name);

    // Every built-in name, sorted.
    const std::vector<std::string_view>& builtinNames();

    // "builtin:a, builtin:b, ...", for messages.
    std::string builtinList();
}
