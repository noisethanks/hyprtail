#include "TrailResolve.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "ConfigParse.hpp"
#include "ShaderSource.hpp"

namespace hyprtail::trail {
    namespace {
        // "builtin:<name>" is the embedded built-in, never a file. As a trail
        // it names an embedded trail; as a shader stage it is shorthand for
        // the embedded "shaders/<name>" (shader::builtinStageKey()). Anything else
        // is a path, resolved by cfg::resolveShaderPath(): relative ones
        // against the hyprtail root, "~" and absolute as given. (Shader
        // includes follow the same rule: "helpers/<name>" is the embedded
        // helper, a path is a file. See ShaderSource.cpp.)
        constexpr std::string_view              BUILTIN_PREFIX = "builtin:";

        std::expected<std::string, std::string> readFile(const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::unexpected(std::format("can't open {}", path.string()));
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        // Resolves one shader stage of one layer to {embedded key, path
        // override}; a path override is "" for an embedded shader.
        //  - "builtin:<name>": the embedded shaders/<name>, in any trail.
        //  - anything else in a built-in trail: looked up in the embedded
        //    shader table by the path as written ("shaders/taper.vert"),
        //    never on disk. That keeps the zero-file first run working and a
        //    stale copied folder from changing what builtin:<name> means.
        //  - anything else in a user preset: a path, resolved like
        //    layerN_vertex/layerN_fragment (cfg::resolveShaderPath). One that
        //    isn't on disk is added to `missing`; load() reports it.
        // Always returns a safe embedded identity (`safeBuiltin`) alongside a
        // path override, so CShaderSlot's constructor -- which
        // unconditionally looks up the embedded text -- never sees an
        // unresolvable name (a bad embedded reference is instead caught
        // structurally, right here, before any CLayer/CShaderSlot exists).
        std::expected<std::pair<std::string, std::string>, std::string> resolveStage(const std::string& layerName, const std::map<std::string, std::string>& keys, bool embedded,
                                                                                     const char* stageKey, const char* safeBuiltin, std::vector<std::string>& missing) {
            const auto it = keys.find(stageKey);
            if (it == keys.end())
                return std::unexpected(std::format(R"(layer "{}" needs "{}:{}")", layerName, layerName, stageKey));
            const std::string& value = it->second;

            if (auto key = shader::builtinStageKey(value)) {
                if (shader::builtin(*key).empty())
                    return std::unexpected(std::format(R"(layer "{}": "{}" isn't a built-in shader)", layerName, value));
                return std::pair<std::string, std::string>{std::move(*key), ""};
            }

            if (value.empty())
                return std::unexpected(std::format("layer \"{}:{}\" is empty", layerName, stageKey));

            if (embedded) {
                if (shader::builtin(value).empty())
                    return std::unexpected(std::format(R"(layer "{}": "{}" isn't an embedded shader; a built-in trail names them like "shaders/taper.vert")", layerName, value));
                return std::pair<std::string, std::string>{value, ""};
            }

            std::filesystem::path p = cfg::resolveShaderPath(value);
            if (!p.is_absolute())
                return std::unexpected(std::format(R"(layer "{}": can't resolve "{}" (no usable HOME or XDG_CONFIG_HOME))", layerName, value));
            std::error_code ec;
            if (const auto canon = std::filesystem::weakly_canonical(p, ec); !ec)
                p = canon;
            if (!std::filesystem::exists(p))
                missing.push_back(p.string());
            return std::pair<std::string, std::string>{safeBuiltin, p.string()};
        }

        std::expected<SLayerSpec, std::string> resolveLayer(const std::string& layerName, const std::map<std::string, std::string>& keys, bool embedded,
                                                            std::vector<std::string>& missing) {
            auto vert = resolveStage(layerName, keys, embedded, "vertex", "shaders/taper.vert", missing);
            if (!vert)
                return std::unexpected(vert.error());
            auto frag = resolveStage(layerName, keys, embedded, "fragment", "shaders/gradient.frag", missing);
            if (!frag)
                return std::unexpected(frag.error());

            SLayerSpec spec;
            spec.name        = layerName;
            spec.vertBuiltin = vert->first;
            spec.vertPath    = vert->second;
            spec.fragBuiltin = frag->first;
            spec.fragPath    = frag->second;
            for (const auto& [k, v] : keys)
                if (k != "vertex" && k != "fragment")
                    spec.defaults[k] = v;
            return spec;
        }

        // What to write instead of a trail that is neither "builtin:<name>"
        // nor a .conf path (the old bare "<name>" form, no extension).
        std::string notATrail(const std::string& name) {
            if (name.empty())
                return R"(trail is empty; write "builtin:<name>" for a built-in, or the path of a .conf file relative to <hyprtail root>, e.g. "presets/mine.conf")";
            const auto own  = name.contains('/') ? name + ".conf" : "presets/" + name + ".conf";
            const auto hint = builtinManifest(name).empty() ? std::string{} : std::format(R"("builtin:{}" for the built-in, or )", name);
            return std::format(R"("{}" isn't a trail: write {}"{}" for your own file (the path of a .conf file with its extension, relative to <hyprtail root>))", name, hint, own);
        }
    }

    std::expected<SResolved, std::string> resolve(const std::string& name, std::vector<std::string>& missing) {
        std::string text;
        bool        embedded = false;

        if (name.starts_with(BUILTIN_PREFIX)) {
            embedded           = true;
            const auto builtin = builtinManifest(std::string_view{name}.substr(BUILTIN_PREFIX.size()));
            if (builtin.empty())
                return std::unexpected(std::format("unknown built-in trail \"{}\" (built-in: {})", name, builtinList()));
            text = std::string{builtin};
        } else {
            if (!name.ends_with(".conf"))
                return std::unexpected(notATrail(name));
            const auto file = cfg::resolveShaderPath(name);
            if (!file.is_absolute())
                return std::unexpected("can't locate the hyprtail config directory (no usable HOME or XDG_CONFIG_HOME)");
            if (!std::filesystem::exists(file)) {
                const auto stem = std::filesystem::path{name}.stem().string();
                return std::unexpected(
                    std::format("no such file: {}{}", file.string(), builtinManifest(stem).empty() ? "" : std::format(" (for the built-in, use \"builtin:{}\")", stem)));
            }
            auto read = readFile(file);
            if (!read)
                return std::unexpected(read.error());
            text = std::move(*read);
        }

        auto manifest = parse(text);
        if (!manifest)
            return std::unexpected(manifest.error());

        SResolved out;
        out.name           = name;
        out.description    = manifest->description;
        out.sourceKind     = manifest->sourceKind;
        out.sourceDefaults = manifest->sourceKeys;
        static const std::map<std::string, std::string> empty;
        for (const auto& layerName : manifest->layers) {
            const auto it   = manifest->layerKeys.find(layerName);
            auto       spec = resolveLayer(layerName, it == manifest->layerKeys.end() ? empty : it->second, embedded, missing);
            if (!spec)
                return std::unexpected(spec.error());
            out.layers.push_back(std::move(*spec));
        }
        return out;
    }
}
