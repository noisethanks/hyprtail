#pragma once

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

// Hyprland-free parsing and bounds for the emit settings, and the config
// path resolver, kept out of Config.cpp so tests/unit can link them without
// the compositor.
namespace hyprtail::cfg {
    struct SPair {
        double x = 0.0, y = 0.0;
        bool   operator==(const SPair&) const = default;
    };

    // emit_offset bound, logical px, per component (inclusive). A nudge
    // relative to the cursor image, not a placement: typical cursors are
    // 24-48 logical px and large accessibility ones about 128, so 128 covers
    // a shift of a full large cursor's width without allowing arbitrary
    // placement.
    inline constexpr double EMIT_OFFSET_MAX_PX = 128.0;

    // "x y": two whitespace-separated finite numbers and nothing else, same
    // grammar as the Lua vec2 string form (LuaConfigVec2.cpp:14-25).
    inline std::optional<SPair> parseTwoFloats(std::string_view text) {
        std::istringstream in{std::string{text}};
        double             x = 0.0, y = 0.0;
        std::string        tail;
        if (!(in >> x >> y) || (in >> tail) || !std::isfinite(x) || !std::isfinite(y))
            return std::nullopt;
        return SPair{x, y};
    }

    // emit_from's "fx fy" form: both components in [0, 1], inclusive.
    inline bool emitFromInRange(const SPair& p) {
        return p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0;
    }

    // emit_offset: finite, each component within +-EMIT_OFFSET_MAX_PX.
    inline bool emitOffsetInRange(double x, double y) {
        return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= EMIT_OFFSET_MAX_PX && std::abs(y) <= EMIT_OFFSET_MAX_PX;
    }

    // The value emit_offset resolves to: the read value when in range, else
    // `previous` (the caller reports the rejection). On the first parse
    // `previous` is the default {0, 0}.
    inline SPair resolveEmitOffset(double x, double y, const SPair& previous) {
        return emitOffsetInRange(x, y) ? SPair{x, y} : previous;
    }

    // The cursor image box in global logical px (getCursorBoxGlobal: pos =
    // pointer - hotspot, size = image / scale).
    struct SRect {
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    };

    // The emit point: the pointer ("hotspot"), or the `fromNorm` position in
    // the cursor image box when emit_from sets one and there is a cursor
    // image (`cursorBox`), then `offset` added. Doubles throughout; the
    // caller rounds to float once, at the end.
    inline SPair emitPoint(const SPair& pointer, const std::optional<SRect>& cursorBox, const std::optional<SPair>& fromNorm, const SPair& offset) {
        SPair base = pointer;
        if (fromNorm && cursorBox)
            base = {.x = cursorBox->x + (cursorBox->w * fromNorm->x), .y = cursorBox->y + (cursorBox->h * fromNorm->y)};
        return {.x = base.x + offset.x, .y = base.y + offset.y};
    }

    // hyprtail's config root: $XDG_CONFIG_HOME/hypr/hyprtail, else
    // ~/.config/hypr/hyprtail. Empty if neither variable is usable.
    inline std::filesystem::path hyprtailRoot() {
        const char* xdg  = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && xdg[0] == '/')
            return std::filesystem::path{xdg} / "hypr" / "hyprtail";
        if (home && home[0] == '/')
            return std::filesystem::path{home} / ".config" / "hypr" / "hyprtail";
        return {};
    }

    // The one resolver for every path hyprtail reads from the user's config
    // (shader paths from the config or a trail manifest, and the `trail`
    // setting's .conf file): "" -> empty (built-in); "~" / "~/..." -> $HOME;
    // relative -> against hyprtailRoot() (deliberately not the Hyprland
    // config directory, unlike decoration:screen_shader: one base for every
    // hyprtail path). Not absolute if HOME/XDG_CONFIG_HOME aren't usable.
    inline std::filesystem::path resolveShaderPath(const std::string& configured) {
        if (configured.empty())
            return {};

        if (configured == "~" || configured.starts_with("~/")) {
            const char* home = std::getenv("HOME");
            if (home && home[0] == '/')
                return std::filesystem::path{home} / configured.substr(configured.size() > 1 ? 2 : 1);
            return configured;
        }

        std::filesystem::path p{configured};
        if (p.is_absolute())
            return p;
        return hyprtailRoot() / p;
    }
}
