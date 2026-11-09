#include "Config.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <sstream>
#include <string_view>

#include <config/ConfigManager.hpp>
#include <config/values/types/BoolValue.hpp>
#include <config/values/types/StringValue.hpp>
#include <config/values/types/Vec2Value.hpp>

#include "ConfigParse.hpp"
#include "Diagnostics.hpp"
#include "Manifest.hpp"

using hyprtail::diag::eSeverity;
using namespace Config::Values;

namespace hyprtail::cfg {
    const char* warpModeName(eWarpMode m) {
        switch (m) {
            case eWarpMode::LINE: return "line";
            case eWarpMode::CURVE: return "curve";
            default: return "break";
        }
    }

    namespace {
        // Names are stored as raw const char* by IValue: literals only.
        // Min/max are also enforced by Hyprland when parsing, with its own
        // config error; read() re-checks so a bad value can never reach us.
        struct SRegistered {
            SP<CStringValue>                warp;
            SP<CStringValue>                trail;
            std::array<SP<CStringValue>, 4> layerVertex, layerFragment;
            SP<CStringValue>                params;
            SP<CStringValue>                screenshare;
            SP<CStringValue>                emitFrom;
            SP<CVec2Value>                  emitOffset;
        };

        SRegistered& reg() {
            static SRegistered r;
            return r;
        }

        const SValues DEFAULTS{};

        bool          add(HANDLE handle, const SP<IValue>& v) {
            if (HyprlandAPI::addConfigValueV2(handle, v))
                return true;
            diag::report(eSeverity::WARN, std::format("config:{}", v->name()), std::format("could not register config value {}; using its built-in default", v->name()));
            return false;
        }

        // One of `allowed`, else report and keep `fallback`.
        std::string checkEnum(const SP<CStringValue>& v, std::initializer_list<std::string_view> allowed, const std::string& fallback) {
            if (!v)
                return fallback;
            auto val = v->value();
            if (std::ranges::find(allowed, std::string_view{val}) != allowed.end()) {
                diag::resetKey(std::format("config:{}", v->name()));
                return val;
            }
            std::string list;
            for (const auto& a : allowed)
                list += (list.empty() ? "" : ", ") + std::string{a};
            diag::report(eSeverity::WARN, std::format("config:{}", v->name()), std::format(R"({} = "{}" isn't one of {}; keeping "{}")", v->name(), val, list, fallback));
            return fallback;
        }

    }

    bool registerValues(HANDLE handle) {
        auto& r = reg();

        r.warp = makeShared<CStringValue>("plugin:hyprtail:warp", R"(how the trail crosses a pointer warp: "break" (default), "line", or "curve")", warpModeName(DEFAULTS.warp));
        // IValue keeps the description as a raw pointer (IValue.hpp:13,
        // StringValue.cpp:8 at efb5099), so the built text is a static, alive
        // as long as the literals around it.
        static const std::string TRAIL_HELP = std::format("which trail to use: \"builtin:<name>\" ({}) "
                                                          "or the path of a .conf file relative to <hyprtail root>, e.g. \"presets/ink.conf\"",
                                                          trail::builtinList());
        r.trail                             = makeShared<CStringValue>("plugin:hyprtail:trail", TRAIL_HELP.c_str(), DEFAULTS.trail.c_str());
        r.emitFrom   = makeShared<CStringValue>("plugin:hyprtail:emit_from",
                                                "where on the cursor image trail points are emitted from: \"hotspot\" (default), or a normalized \"x y\" position in "
                                                "the cursor image box, each in 0..1 (0 0 = top-left, 0.5 0.5 = center)",
                                                "hotspot");
        r.emitOffset = makeShared<CVec2Value>("plugin:hyprtail:emit_offset", "fixed pixel offset added after emit_from, logical px, each component within +-128",
                                              Config::VEC2{sc<float>(DEFAULTS.emitOffsetPx.x), sc<float>(DEFAULTS.emitOffsetPx.y)});
        // No SVec2ValueOptions::validator: at the pin neither provider applies
        // a CVec2Value validator (LuaConfigUtils.cpp:41-42, legacy
        // ConfigManager.cpp:501-502 take only defaultVal()), so the bound is
        // enforced in read() alone.

        // Per-layer shader overrides: static keys indexed by
        // position in the trail's layer list, capped at 4. Names must be
        // literals (see the SRegistered comment above), so these are
        // spelled out rather than built with std::format.
        static constexpr std::array<const char*, 4> LAYER_VERT_KEYS{"plugin:hyprtail:layer1_vertex", "plugin:hyprtail:layer2_vertex", "plugin:hyprtail:layer3_vertex",
                                                                    "plugin:hyprtail:layer4_vertex"};
        static constexpr std::array<const char*, 4> LAYER_FRAG_KEYS{"plugin:hyprtail:layer1_fragment", "plugin:hyprtail:layer2_fragment", "plugin:hyprtail:layer3_fragment",
                                                                    "plugin:hyprtail:layer4_fragment"};
        for (size_t i = 0; i < 4; ++i) {
            r.layerVertex[i]   = makeShared<CStringValue>(LAYER_VERT_KEYS[i], "vertex shader override for this layer of the trail, empty uses the trail's own shader", "");
            r.layerFragment[i] = makeShared<CStringValue>(LAYER_FRAG_KEYS[i], "fragment shader override for this layer of the trail, empty uses the trail's own shader", "");
        }
        r.params      = makeShared<CStringValue>("plugin:hyprtail:params", R"(per-layer parameter overrides: "<layer>:<name>=<value> ...")", "");
        r.screenshare = makeShared<CStringValue>("plugin:hyprtail:screenshare", R"("exclude" (default) to keep the trail out of monitor/region captures and mirrors, or "include")",
                                                 DEFAULTS.screenshare.c_str());

        bool ok = true;
        for (const SP<IValue>& v : std::initializer_list<SP<IValue>>{r.warp, r.trail, r.params, r.screenshare, r.emitFrom, r.emitOffset})
            ok = add(handle, v) && ok;
        for (size_t i = 0; i < 4; ++i) {
            ok = add(handle, r.layerVertex[i]) && ok;
            ok = add(handle, r.layerFragment[i]) && ok;
        }
        return ok;
    }

    void releaseValues() {
        reg() = {};
    }

    SValues read(const SValues& previous) {
        auto&   r = reg();
        SValues v = previous;

        {
            const auto text = checkEnum(r.warp, {"break", "line", "curve"}, warpModeName(previous.warp));
            v.warp          = text == "line" ? eWarpMode::LINE : text == "curve" ? eWarpMode::CURVE : eWarpMode::BREAK;
        }
        if (r.trail)
            v.trail = r.trail->value();

        if (r.emitFrom) {
            const auto  text = r.emitFrom->value();
            const char* key  = "config:plugin:hyprtail:emit_from";
            if (text == "hotspot") {
                v.emitFromNorm = std::nullopt;
                diag::resetKey(key);
            } else {
                const auto parsed = parseTwoFloats(text);
                const auto kept   = previous.emitFromNorm ? std::format("\"{} {}\"", previous.emitFromNorm->x, previous.emitFromNorm->y) : std::string{"\"hotspot\""};
                if (parsed && emitFromInRange(*parsed)) {
                    v.emitFromNorm = Vector2D{parsed->x, parsed->y};
                    diag::resetKey(key);
                } else {
                    diag::report(
                        eSeverity::WARN, key,
                        std::format("plugin:hyprtail:emit_from = \"{}\" {}; keeping {}", text, parsed ? "has a component outside 0..1" : R"(isn't "hotspot" or "x y")", kept));
                    v.emitFromNorm = previous.emitFromNorm;
                }
            }
        }
        if (r.emitOffset) {
            const auto  vec  = r.emitOffset->value();
            const char* key  = "config:plugin:hyprtail:emit_offset";
            const auto  next = resolveEmitOffset(vec.x, vec.y, SPair{previous.emitOffsetPx.x, previous.emitOffsetPx.y});
            v.emitOffsetPx   = {next.x, next.y};
            if (emitOffsetInRange(vec.x, vec.y))
                diag::resetKey(key);
            else
                diag::report(eSeverity::WARN, key,
                             std::format("plugin:hyprtail:emit_offset = {} {} is outside +-{} on an axis; keeping {} {}", vec.x, vec.y, EMIT_OFFSET_MAX_PX, previous.emitOffsetPx.x,
                                         previous.emitOffsetPx.y));
        }

        for (size_t i = 0; i < 4; ++i) {
            if (r.layerVertex[i])
                v.layerVertex[i] = r.layerVertex[i]->value();
            if (r.layerFragment[i])
                v.layerFragment[i] = r.layerFragment[i]->value();
        }
        if (r.params)
            v.params = r.params->value();

        v.screenshare = checkEnum(r.screenshare, {"exclude", "include"}, previous.screenshare);

        return v;
    }
}
