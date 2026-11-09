#include "Layer.hpp"

#include <format>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;

namespace hyprtail {
    CLayer::CLayer(const SLayerSpec& spec) : slot(spec.name, spec.vertBuiltin, spec.fragBuiltin), m_name(spec.name), m_overrides(spec.defaults) {}

    const std::string& CLayer::name() const {
        return m_name;
    }

    void CLayer::setParamOverrides(std::map<std::string, std::string> overrides) {
        if (overrides == m_paramOverrides)
            return;
        m_paramOverrides = std::move(overrides);
        ++m_overridesVersion;
    }

    bool CLayer::resolved() const {
        return slot.shader() && res.generation == slot.generation();
    }

    bool CLayer::enabledSetting() const {
        return hyprtail::enabledSetting(m_overrides, m_paramOverrides);
    }

    shader::eTopology CLayer::topology() const {
        return slot.info().topology;
    }

    void CLayer::resolve() {
        if (!slot.shader() || (res.generation == slot.generation() && res.overrides == m_overridesVersion))
            return;

        const auto  key = "params:" + m_name;
        std::string problems;
        SResolved   r = resolveValues(slot.info(), m_overrides, m_paramOverrides, problems);
        r.generation  = slot.generation();
        r.overrides   = m_overridesVersion;
        res           = std::move(r);

        diag::resetKey(key);
        if (!problems.empty())
            diag::report(eSeverity::WARN, key, std::format("layer {}: parameter problems:{}", m_name, problems));
    }
}
