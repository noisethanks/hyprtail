#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "ShaderSource.hpp"

// Preprocessed vertex + fragment source for one program.
struct SShaderPair {
    hyprtail::shader::SSource vert, frag;
    bool                      builtin = true; // both stages built-in

    // Which files the pair came from ("" = built-in stage), resolved. Used to
    // tell an edit of the same files (keep the previous program on failure)
    // from a config change to other files (fall back to built-in).
    std::string vertOrigin, fragOrigin;
};

// The checks on a pair that need no GL (CShaderSlot runs them around the
// real compile and link, ShaderSlot.cpp).
namespace hyprtail {
    // What a compiled program declares (contract 2): its topology, the
    // parameters of both stages (merged) and its padding expressions.
    struct SProgramInfo {
        shader::eTopology             topology = shader::eTopology::PATH;
        shader::SInstanceCount        instances; // instanced topology: K, a literal or a param name
        std::vector<params::SDecl>    params;
        std::vector<shader::SPadding> padding;
    };

    // The declarations of both stages have to fit together: the fragment
    // shader's `expects`, one topology (vertex), parameters declared
    // identically wherever they appear, an instanced K param that fits,
    // padding expressions over known scalar parameters. The merged
    // declarations, or why the pair doesn't fit.
    std::expected<SProgramInfo, std::string> programInfo(const SShaderPair& pair);

    // Text-only scan for the varying check: fragment `in`
    // declarations not matched by a vertex `out` of the same name and type.
    // Regex-based, not real GLSL parsing (misses a multi-name declaration or
    // a layout() qualifier), so it is consulted only after a real link
    // failure, to explain it. nullopt if nothing is found.
    std::optional<std::string> varyingCheck(const SShaderPair& pair);
}
