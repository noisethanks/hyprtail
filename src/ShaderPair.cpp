#include "ShaderPair.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <regex>
#include <sstream>

namespace hyprtail {
    std::expected<SProgramInfo, std::string> programInfo(const SShaderPair& pair) {
        SProgramInfo info;
        info.topology  = pair.vert.topology.value_or(shader::eTopology::PATH);
        info.instances = pair.vert.instances;

        // #pragma hyprtail expects: the fragment shader's
        // declared topology compatibility against what the vertex
        // shader actually provides.
        if (auto mismatch = shader::expectsMismatch(pair.vert, pair.frag))
            return std::unexpected(std::move(*mismatch));

        for (const auto* src : {&pair.vert, &pair.frag}) {
            for (const auto& p : src->params) {
                const auto it = std::ranges::find_if(info.params, [&](const auto& d) { return d.name == p.decl.name; });
                if (it == info.params.end()) {
                    info.params.push_back(p.decl);
                    continue;
                }
                const bool same = it->type == p.decl.type && params::format(it->def) == params::format(p.decl.def) && it->min == p.decl.min && it->max == p.decl.max;
                if (!same)
                    return std::unexpected(std::format("param {} is declared differently in the two stages ({}); declare it identically", p.decl.name, p.where));
            }
        }

        // An instanced topology's K param, against the merged parameters.
        if (auto problem = shader::instanceCountProblem(pair.vert, info.params))
            return std::unexpected(std::move(*problem));

        for (const auto* src : {&pair.vert, &pair.frag}) {
            for (const auto& pad : src->padding) {
                for (const auto& n : pad.expr.names()) {
                    const bool param =
                        std::ranges::any_of(info.params, [&](const auto& d) { return d.name == n && d.type != params::eType::VEC2 && d.type != params::eType::COLOR; });
                    const bool reserved = std::ranges::any_of(shader::reservedParams(), [&](const auto& r) { return r.decl.name == n; });
                    if (!param && !reserved)
                        return std::unexpected(std::format("{}: padding uses \"{}\", which isn't a float, int or bool param of this program", pad.where, n));
                }
                info.padding.push_back(pad);
            }
        }
        return info;
    }

    std::optional<std::string> varyingCheck(const SShaderPair& pair) {
        // A trailing // comment is allowed: the vertex prelude's outputs have one.
        static const std::regex RE_OUT{R"re(^\s*(?:flat\s+)?out\s+(\w+)\s+(\w+)\s*;\s*(?://.*)?$)re"};
        static const std::regex RE_IN{R"re(^\s*(?:flat\s+)?in\s+(\w+)\s+(\w+)\s*;\s*(?://.*)?$)re"};

        const auto              scan = [](const std::string& text, const std::regex& re) {
            std::map<std::string, std::string> out; // name -> type
            std::istringstream                 in{text};
            std::string                        line;
            std::smatch                        m;
            while (std::getline(in, line))
                if (std::regex_match(line, m, re))
                    out.emplace(m[2].str(), m[1].str());
            return out;
        };

        const auto               vertOuts = scan(pair.vert.text, RE_OUT);
        const auto               fragIns  = scan(pair.frag.text, RE_IN);

        static const std::string STANDARD = "ht_vLocal, ht_vAge, ht_vLife, ht_vSpeed, ht_vDist, ht_vSeed";
        std::vector<std::string> problems;
        for (const auto& [name, fragType] : fragIns) {
            const auto it = vertOuts.find(name);
            if (it == vertOuts.end())
                problems.push_back(std::format("fragment shader {} reads `{}`, which geometry shader {} doesn't write (standard varyings: {})", pair.frag.sourceNames.front(), name,
                                               pair.vert.sourceNames.front(), STANDARD));
            else if (it->second != fragType)
                problems.push_back(std::format("fragment shader {} declares `{}` as {}; geometry shader {} declares it as {}", pair.frag.sourceNames.front(), name, fragType,
                                               pair.vert.sourceNames.front(), it->second));
        }
        if (problems.empty())
            return std::nullopt;

        std::string out = "varying mismatch:";
        for (const auto& p : problems)
            out += "\n  " + p;
        return out;
    }
}
