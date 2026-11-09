#pragma once

#include <map>
#include <string>
#include <vector>

#include "LayerValues.hpp"
#include "Params.hpp"
#include "RenderUtil.hpp"
#include "ShaderSlot.hpp"
#include "TrailResolve.hpp" // SLayerSpec

// One layer of a trail: a shader program (geometry + shading)
// over the trail's shared source, with its parameter values and its own
// per-monitor damage lifecycle.
namespace hyprtail {
    class CLayer {
      public:
        explicit CLayer(const SLayerSpec& spec);

        const std::string& name() const;

        // Entries of the `params` config string addressed to
        // this layer, value text by parameter name. Applied on top of the
        // active trail's own per-layer defaults (SLayerSpec::defaults, set
        // once at construction -- a manifest edit rebuilds the whole CLayer
        // with a fresh set), so `params` wins. An unknown name is reported
        // (params:<layer>) and ignored, for the manifest and `params` alike.
        void setParamOverrides(std::map<std::string, std::string> overrides);

        // Recompute res if the program or the overrides changed. Reports bad
        // values (params:<layer>) and keeps the declared default for them.
        void resolve();
        bool resolved() const; // res matches the active program

        // "enabled" from the overrides alone, usable before any program is
        // active (a disabled layer isn't compiled).
        bool              enabledSetting() const;

        shader::eTopology topology() const;

        CShaderSlot       slot;
        CMonitorDamage    damage;
        SResolved         res;

        // Set after an unrecoverable failure (built-in shader, GL,
        // exception): the layer clears its last box once and stays off
        // until the plugin is reloaded.
        bool disabled = false;

      private:
        std::string                        m_name;
        std::map<std::string, std::string> m_overrides;
        std::map<std::string, std::string> m_paramOverrides;
        uint64_t                           m_overridesVersion = 0; // bumped on either map changing
    };
}
