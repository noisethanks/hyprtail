#include "Manifest.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace hyprtail::trail {
    namespace {
        std::string_view trim(std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        }

        // "a, b ,c" -> ["a", "b", "c"], each trimmed, empty pieces dropped.
        std::vector<std::string> splitCommaTrim(std::string_view s) {
            std::vector<std::string> out;
            size_t                   start = 0;
            while (start <= s.size()) {
                const auto comma = s.find(',', start);
                const auto piece = trim(s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
                if (!piece.empty())
                    out.emplace_back(piece);
                if (comma == std::string_view::npos)
                    break;
                start = comma + 1;
            }
            return out;
        }
    }

    std::expected<SManifest, std::string> parse(std::string_view text) {
        struct SRaw {
            std::optional<std::string>              contract, description, sourceKind;
            std::optional<std::vector<std::string>> layers;
        } raw;
        std::map<std::string, std::map<std::string, std::string>> layerKeys;

        std::istringstream                                        in{std::string{text}};
        std::string                                               rawLine;
        int                                                       lineNo = 0;
        while (std::getline(in, rawLine)) {
            ++lineNo;
            const auto       where = std::format("line {}", lineNo);

            std::string_view line = rawLine;
            if (const auto hash = line.find('#'); hash != std::string_view::npos)
                line = line.substr(0, hash);
            line = trim(line);
            if (line.empty())
                continue;

            const auto eq = line.find('=');
            if (eq == std::string_view::npos)
                return std::unexpected(std::format(R"({}: malformed line, expected "key = value": "{}")", where, line));
            const auto key   = trim(line.substr(0, eq));
            const auto value = trim(line.substr(eq + 1));
            if (key.empty())
                return std::unexpected(std::format("{}: empty key", where));

            const auto colon = key.find(':');
            if (colon == std::string_view::npos) {
                if (key == "contract") {
                    if (raw.contract)
                        return std::unexpected(std::format("{}: duplicate \"contract\"", where));
                    raw.contract = std::string{value};
                } else if (key == "description") {
                    if (raw.description)
                        return std::unexpected(std::format("{}: duplicate \"description\"", where));
                    raw.description = std::string{value};
                } else if (key == "layers") {
                    if (raw.layers)
                        return std::unexpected(std::format("{}: duplicate \"layers\"", where));
                    raw.layers = splitCommaTrim(value);
                } else if (key == "source") {
                    if (raw.sourceKind)
                        return std::unexpected(std::format("{}: duplicate \"source\"", where));
                    if (!source::known(value))
                        return std::unexpected(std::format("{}: unknown source \"{}\" ({})", where, value, source::kindList()));
                    raw.sourceKind = std::string{value};
                } else
                    return std::unexpected(std::format(R"({}: unknown key "{}" (contract, description, layers, source, or "<layer>:<name>"))", where, key));
                continue;
            }

            const auto layer = trim(key.substr(0, colon));
            const auto name  = trim(key.substr(colon + 1));
            if (layer.empty() || name.empty())
                return std::unexpected(std::format("{}: malformed layer key \"{}\"", where, key));
            auto& entry = layerKeys[std::string{layer}];
            if (entry.contains(std::string{name}))
                return std::unexpected(std::format("{}: duplicate \"{}\"", where, key));
            entry[std::string{name}] = std::string{value};
        }

        if (!raw.contract)
            return std::unexpected("missing \"contract = 2\"");
        if (*raw.contract != std::to_string(CONTRACT_VERSION))
            return std::unexpected(std::format("this hyprtail implements manifest contract {}, the manifest declares contract {}", CONTRACT_VERSION, *raw.contract));

        if (!raw.layers || raw.layers->empty())
            return std::unexpected("missing or empty \"layers\"");
        if (raw.layers->size() > 4)
            return std::unexpected(std::format("\"layers\" lists {} layers; at most 4", raw.layers->size()));
        {
            std::set<std::string> seen;
            for (const auto& l : *raw.layers) {
                if (l == source::KEY_PREFIX)
                    return std::unexpected(std::format("\"{}\" is reserved for the source's settings and can't be a layer name", l));
                if (!seen.insert(l).second)
                    return std::unexpected(std::format(R"("layers" lists "{}" twice)", l));
            }
        }

        // "source:<name>" keys are the source's settings, not a layer's.
        std::map<std::string, std::string> sourceKeys;
        if (const auto it = layerKeys.find(std::string{source::KEY_PREFIX}); it != layerKeys.end()) {
            sourceKeys = std::move(it->second);
            layerKeys.erase(it);
        }
        for (const auto& [layer, keys] : layerKeys) {
            if (std::ranges::find(*raw.layers, layer) == raw.layers->end())
                return std::unexpected(std::format(R"("{}:..." keys given, but "{}" isn't in "layers")", layer, layer));
        }

        return SManifest{
            .description = raw.description.value_or(""),
            .layers      = std::move(*raw.layers),
            .layerKeys   = std::move(layerKeys),
            .sourceKind  = raw.sourceKind.value_or(std::string{source::DEFAULT_KIND}),
            .sourceKeys  = std::move(sourceKeys),
        };
    }

    namespace {
        constexpr unsigned char JITTER_CONF[] = {
#embed "../hyprtail/presets/jitter.conf"
        };
        constexpr unsigned char VIVID_CONF[] = {
#embed "../hyprtail/presets/vivid.conf"
        };
        constexpr unsigned char COMET_CONF[] = {
#embed "../hyprtail/presets/comet.conf"
        };
        constexpr unsigned char EMBERS_CONF[] = {
#embed "../hyprtail/presets/embers.conf"
        };
        constexpr unsigned char SPRING_CONF[] = {
#embed "../hyprtail/presets/spring.conf"
        };
        constexpr unsigned char INK_CONF[] = {
#embed "../hyprtail/presets/ink.conf"
        };
        constexpr unsigned char MOSAIC_CONF[] = {
#embed "../hyprtail/presets/mosaic.conf"
        };
        constexpr unsigned char HELIX_CONF[] = {
#embed "../hyprtail/presets/helix.conf"
        };

        template <size_t N>
        constexpr std::string_view view(const unsigned char (&data)[N]) {
            return {reinterpret_cast<const char*>(data), N};
        }

        // Keys are the file stems; every hyprtail/presets/*.conf is listed
        // here (make test-unit checks both directions).
        const std::map<std::string, std::string_view, std::less<>>& table() {
            static const std::map<std::string, std::string_view, std::less<>> m{
                {"jitter", view(JITTER_CONF)}, {"vivid", view(VIVID_CONF)}, {"comet", view(COMET_CONF)},   {"embers", view(EMBERS_CONF)},
                {"spring", view(SPRING_CONF)}, {"ink", view(INK_CONF)},     {"mosaic", view(MOSAIC_CONF)}, {"helix", view(HELIX_CONF)},
            };
            return m;
        }
    }

    std::string_view builtinManifest(std::string_view name) {
        const auto it = table().find(name);
        return it == table().end() ? std::string_view{} : it->second;
    }

    const std::vector<std::string_view>& builtinNames() {
        static const std::vector<std::string_view> names = [] {
            std::vector<std::string_view> out;
            for (const auto& [name, text] : table())
                out.emplace_back(name);
            return out;
        }();
        return names;
    }

    std::string builtinList() {
        std::string out;
        for (const auto name : builtinNames())
            out += std::format("{}builtin:{}", out.empty() ? "" : ", ", name);
        return out;
    }
}
