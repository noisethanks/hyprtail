#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <helpers/math/Math.hpp>

#include "Diagnostics.hpp"
#include "ShaderSlot.hpp"

// `hyprctl hyprtail`: a snapshot of the plugin's state, taken
// on the main thread by the command handler, and its text / JSON rendering.
namespace hyprtail::status {
    struct SLayer {
        std::string                                      name;
        bool                                             enabled = false, disabled = false, resolved = false;
        double                                           fadeMs = 0.0, startMs = 0.0, durationMs = 0.0;
        float                                            extentPx = 0.F;
        std::vector<std::pair<std::string, std::string>> params; // name, value
        SSlotStatus                                      shader;
    };

    struct SMonitor {
        std::string                      name;
        uint64_t                         renders      = 0; // renders of this monitor seen (RENDER_BEGIN)
        uint64_t                         hookRuns     = 0; // lifecycle run from the cursor hook
        uint64_t                         fallbackRuns = 0; // lifecycle run from RENDER_LAST_MOMENT
        uint64_t                         draws        = 0; // renders that drew at least one layer
        uint64_t                         emptySkips   = 0; // renders without damage (workspace skipped)
        uint64_t                         postChecks   = 0; // renders whose RENDER_POST screenshare self-check ran
        std::vector<std::optional<CBox>> layerBoxes;       // per layer: last drawn box, logical monitor-local

        // Screenshare exclude.
        bool needsCopyFB     = false; // needsACopyFB() as of the last render: mirrored, or captured
        bool captureFallback = false; // the capture hook missed a render here; not drawing while it needs a copy
    };

    struct SSnapshot {
        std::string rev, builtHash, runningHash, trail, screenshare;
        bool        cursorHook = false, warpHook = false, captureHook = false;
        uint64_t    renders = 0;

        struct {
            std::string           kind  = "pointer"; // the trail's `source`
            size_t                nodes = 0, capacity = 0;
            uint64_t              generation   = 0;
            bool                  moving       = false; // the source's points are still moving (spring)
            bool                  pendingBreak = false;
            std::string           warpMode     = "break";
            std::array<double, 2> warpP1{0.0, 0.0}, warpP2{1.0, 1.0}; // the source's warp_p1/warp_p2, as resolved
            double                warpSpeed = 0.0, warpMaxMs = 0.0;   // warp_speed (px/ms), warp_max_ms
            double                minSpacing = 0.0, maxSpacing = 0.0; // px; maxSpacing infinite for a source without it
            size_t                warpPending = 0;                    // a playing warp's nodes not drawn yet
            std::optional<double> keepMs;                             // pointer: how long nodes are kept (retention by age); nullopt for spring
            uint64_t              ceilingDrops = 0;                   // pointer: nodes a layer still drew, overwritten in a full ring
            bool                  gpuFailed    = false;
            double                stillMs      = 0.0;
        } source;

        // Suppress conditions: session lock, pointer constraint,
        // and the hyprtail:no_trail rule of the window under the pointer
        // (appRule: as of the last render). hoveredClass/hoveredTitle name the
        // window under the pointer now (empty if none) -- additive JSON
        // fields, keep the bools first so the JSON schema stays stable.
        struct {
            bool        locked = false, constrained = false, appRule = false;
            std::string hoveredClass, hoveredTitle;
        } suppress;

        std::vector<SLayer>   layers;
        std::vector<SMonitor> monitors;
        diag::SStats          diag;
        std::string           errorFile;
    };

    std::string text(const SSnapshot& s);
    std::string json(const SSnapshot& s);
}
