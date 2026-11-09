#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <plugins/PluginAPI.hpp>
#include <helpers/math/Math.hpp>

#include "ConfigParse.hpp" // hyprtailRoot(), resolveShaderPath()

// User settings, registered through the V2 config API
// (HyprlandAPI::addConfigValueV2), which works with both the Lua and the
// legacy hyprlang config provider. V1 addConfigValue/getConfigValue only work
// with the legacy provider at the pinned commit.
//
// Lua:      hl.config({ plugin = { hyprtail = { warp = "line" } } })
// hyprlang: plugin:hyprtail:warp = line
//
// The source's own settings (min_spacing, max_spacing, warp_speed, warp_max_ms, warp_p1/p2; a spring's capacity)
// live in the trail's preset as source:<name> keys, see Source.hpp.
namespace hyprtail::cfg {
    // Replaces the removed `interpolate_warps` (false = break, true = line).
    enum class eWarpMode : uint8_t {
        BREAK, // default: a warp starts a new segment
        LINE,  // connects with a straight sweep (today's interpolate_warps = true)
        CURVE, // connects with nodes along a quadratic Bezier
    };

    const char* warpModeName(eWarpMode m);

    struct SValues {
        std::string trail = "builtin:ink";
        eWarpMode   warp  = eWarpMode::BREAK;

        // Emit offset. nullopt = "hotspot" (default, today's
        // behavior); otherwise a position normalized to the cursor image
        // box (CPointerManager::getCursorBoxGlobal). emitOffsetPx (logical
        // px) is added after, always.
        std::optional<Vector2D> emitFromNorm;
        Vector2D                emitOffsetPx{0.0, 20.0};

        // Per-layer shader overrides, static keys indexed by the layer's
        // position in the trail's layer list: layer1_vertex
        // .. layer4_fragment. "" = the trail's own shader for that stage,
        // "builtin:<file>" = a shipped shader, else a path.
        // An index the current trail has no layer for is a plugin warning.
        std::array<std::string, 4> layerVertex, layerFragment;

        // "<layer>:<name>=<value> ..." parameter overrides,
        // validated against each layer's shader-declared params.
        std::string params;

        // Screenshare exclude: "exclude" (default) or
        // "include" (today's behavior). A bad value warns and keeps the
        // previous one, same as the numeric settings.
        std::string screenshare = "exclude";
    };

    // Register all values. Call in PLUGIN_INIT. Returns false if any failed
    // (reported); the plugin then runs on built-in defaults for those.
    bool registerValues(HANDLE handle);

    // Release our references (PLUGIN_EXIT / teardown).
    void releaseValues();

    // Current values. Invalid ones are reported (config:<key>) and replaced by
    // the corresponding field of `previous`.
    SValues read(const SValues& previous);
}
