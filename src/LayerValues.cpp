#include "LayerValues.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace hyprtail {
    SResolved resolveValues(const SProgramInfo& info, const std::map<std::string, std::string>& manifest, const std::map<std::string, std::string>& paramString,
                            std::string& problems) {
        // Declarations: reserved first, then the program's own.
        std::vector<std::pair<params::SDecl, params::SValue>> all;
        for (const auto& r : shader::reservedParams()) {
            auto decl = r.decl;
            if (decl.name == "draw_when_cursor_hidden" && info.topology == shader::eTopology::QUAD)
                decl.def.x = 0.0; // an idle marker defeats a cursor hide
            all.emplace_back(decl, decl.def);
        }
        for (const auto& d : info.params)
            all.emplace_back(d, d.def);

        // The manifest's per-layer defaults first, then the `params` string
        // on top, so params wins. Both report an unknown name: a trail
        // manifest naming a param its own shader doesn't declare is an
        // authoring mistake worth surfacing.
        params::applyOverrides(all, manifest, "layer", problems);
        params::applyOverrides(all, paramString, "layer", problems);

        const auto lookup = [&all](std::string_view n) -> std::optional<double> {
            const auto it = std::ranges::find_if(all, [&](const auto& e) { return e.first.name == n; });
            return it == all.end() ? std::nullopt : params::scalar(it->second);
        };
        const auto get = [&](std::string_view n) { return lookup(n).value_or(0.0); };

        SResolved  r;
        r.enabled         = get("enabled") != 0.0;
        r.drawWhenHidden  = get("draw_when_cursor_hidden") != 0.0;
        r.fadeMs          = get("fade_ms");
        r.startMs         = get("start_ms");
        r.durationMs      = get("duration_ms");
        r.damagePaddingPx = static_cast<float>(get("damage_padding"));

        for (const auto& pad : info.padding) {
            const auto v = pad.expr.eval(lookup);
            if (!v) {
                problems += std::format("\n  padding at {}: {}; ignoring it", pad.where, v.error());
                continue;
            }
            if (*v < 0.0 || *v > 4096.0)
                problems += std::format("\n  padding at {} = {} is outside 0..4096; clamped", pad.where, *v);
            r.paddingPx = std::max(r.paddingPx, static_cast<float>(std::clamp(*v, 0.0, 4096.0)));
        }

        if (info.topology == shader::eTopology::INSTANCED) {
            // A param K was range-checked (1..64) when the program was put
            // together and again for every value set; the clamp only keeps a
            // bad draw from ever sizing a buffer.
            const double k = info.instances.param.empty() ? info.instances.literal : get(info.instances.param);
            r.instances    = std::clamp(static_cast<int>(k), 1, shader::MAX_INSTANCES);
        }

        for (size_t i = shader::reservedParams().size(); i < all.size(); ++i)
            r.values.push_back(all[i]);
        return r;
    }

    bool enabledSetting(const std::map<std::string, std::string>& manifest, const std::map<std::string, std::string>& paramString) {
        const auto pick = [](const std::map<std::string, std::string>& m) -> std::optional<std::string> {
            const auto it = m.find("enabled");
            return it == m.end() ? std::nullopt : std::optional{it->second};
        };
        const auto text = pick(paramString).or_else([&] { return pick(manifest); });
        if (!text)
            return true;
        const auto v = params::parseValue(params::eType::BOOL, *text);
        return !v || v->x != 0.0;
    }
}
