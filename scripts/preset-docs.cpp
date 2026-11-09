// make preset-docs / make check-preset-docs: keeps the generated parts of
// the shipped presets (hyprtail/presets/*.conf) in step with their sources.
//
//   preset-docs [--check] <template> <preset.conf>...
//
// Per preset:
//   - The common block: the lines from "# --- common:start" to
//     "# --- common:end" become the template file's lines between the two
//     markers. A preset without the block gets it at the top, then a blank
//     line. Every template line must be a comment, so the manifest grammar
//     sees nothing new.
//   - Documented keys: the keys named on "# doc: <layer>:<key> ..." lines
//     (any number of them). Each such key line gets the key's param doc as
//     a trailing "# <doc>": the "// <doc>" of its shader's param pragma, or
//     of a source setting (src/Params.hpp), or a reserved param's wording
//     for the layer's topology (shader::SReserved::doc). A trailing
//     comment starting "#!" is hand-written and kept as it is; any other
//     trailing comment on a key line is generated and is replaced, or
//     removed if the key is no longer documented. Within each run of
//     consecutive key lines the trailing comments start in one column, two
//     spaces past the longest commented line.
//
// A documented key that the preset doesn't set, that names an unknown
// layer, or whose param has no doc is an error. --check rewrites nothing
// and fails if any preset would change. Only built-in stages
// (builtin:<file>) are read: shipped presets use nothing else.
//
// Built by the Makefile from this file and the Hyprland-free sources the
// unit tests link; run from the repo root.

#include <algorithm>
#include <cctype>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../src/Manifest.hpp"
#include "../src/Params.hpp"
#include "../src/ShaderPair.hpp"
#include "../src/ShaderSource.hpp"
#include "../src/Source.hpp"

using namespace hyprtail;

namespace {
    constexpr std::string_view COMMON_START = "# --- common:start";
    constexpr std::string_view COMMON_END   = "# --- common:end";

    std::string_view           trim(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    }

    std::string_view trimRight(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        return s;
    }

    std::optional<std::string> readFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::vector<std::string> splitLines(const std::string& text) {
        std::vector<std::string> out;
        std::istringstream       in{text};
        for (std::string line; std::getline(in, line);)
            out.push_back(line);
        return out;
    }

    // The template's lines between its markers, or the whole file if it
    // has none.
    std::expected<std::vector<std::string>, std::string> commonBlock(const std::string& path) {
        const auto text = readFile(path);
        if (!text)
            return std::unexpected(std::format("can't read {}", path));
        auto lines = splitLines(*text);
        while (!lines.empty() && trim(lines.back()).empty())
            lines.pop_back();
        for (const auto& l : lines)
            if (!l.starts_with('#') || l.starts_with(COMMON_START) || l.starts_with(COMMON_END))
                return std::unexpected(std::format("{}: every line must be a comment, and no marker: \"{}\"", path, l));
        lines.insert(lines.begin(), std::string{COMMON_START});
        lines.emplace_back(COMMON_END);
        return lines;
    }

    // Replaces the preset's common block, or inserts one at the top.
    std::expected<std::vector<std::string>, std::string> withCommon(std::vector<std::string> lines, const std::vector<std::string>& block) {
        const auto find = [&](std::string_view marker) {
            std::vector<size_t> at;
            for (size_t i = 0; i < lines.size(); ++i)
                if (trimRight(lines[i]) == marker)
                    at.push_back(i);
            return at;
        };
        const auto start = find(COMMON_START);
        const auto end   = find(COMMON_END);
        if (start.empty() && end.empty()) {
            std::vector<std::string> out = block;
            out.emplace_back();
            out.insert(out.end(), lines.begin(), lines.end());
            return out;
        }
        if (start.size() != 1 || end.size() != 1 || start[0] > end[0])
            return std::unexpected(std::format("needs one \"{}\" line followed by one \"{}\" line, or neither", COMMON_START, COMMON_END));
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(start[0]), lines.begin() + static_cast<std::ptrdiff_t>(end[0]) + 1);
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start[0]), block.begin(), block.end());
        return lines;
    }

    // "layer" -> key -> doc, for every key the preset could set: each
    // layer's reserved params and its program's, and the source's settings
    // under "source".
    std::expected<std::map<std::string, std::map<std::string, std::string>>, std::string> paramDocs(const trail::SManifest& manifest) {
        std::map<std::string, std::map<std::string, std::string>> out;
        for (const auto& d : source::decls(manifest.sourceKind))
            out[std::string{source::KEY_PREFIX}][d.name] = d.doc;

        for (const auto& layer : manifest.layers) {
            auto&      docs  = out[layer];
            const auto keys  = manifest.layerKeys.find(layer);
            const auto stage = [&](const char* key, shader::eStage st) -> std::expected<shader::SSource, std::string> {
                if (keys == manifest.layerKeys.end() || !keys->second.contains(key))
                    return std::unexpected(std::format("layer {} has no {}", layer, key));
                const auto& value = keys->second.at(key);
                const auto  ref   = shader::builtinStageKey(value);
                if (!ref || shader::builtin(*ref).empty())
                    return std::unexpected(std::format("layer {}: {} isn't builtin:<file> of a shipped shader", layer, value));
                return shader::preprocess(shader::builtin(*ref), *ref, {}, st);
            };
            auto vert = stage("vertex", shader::eStage::VERTEX);
            if (!vert)
                return std::unexpected(vert.error());
            auto frag = stage("fragment", shader::eStage::FRAGMENT);
            if (!frag)
                return std::unexpected(frag.error());
            const auto info = programInfo(SShaderPair{.vert = std::move(*vert), .frag = std::move(*frag)});
            if (!info)
                return std::unexpected(std::format("layer {}: {}", layer, info.error()));

            for (const auto& r : shader::reservedParams())
                docs[r.decl.name] = r.doc(info->topology);
            for (const auto& d : info->params)
                docs[d.name] = d.doc;
        }
        return out;
    }

    // A "<key> = <value>" line, split at its first '#'.
    struct SKeyLine {
        std::string key, code, comment; // code: trailing space trimmed; comment from its '#', or empty
    };

    std::optional<SKeyLine> keyLine(const std::string& line) {
        const auto hash = line.find('#');
        const auto code = trimRight(std::string_view{line}.substr(0, hash));
        const auto eq   = code.find('=');
        if (trim(code).empty() || eq == std::string_view::npos)
            return std::nullopt;
        return SKeyLine{.key = std::string{trim(code.substr(0, eq))}, .code = std::string{code}, .comment = hash == std::string::npos ? "" : line.substr(hash)};
    }

    std::expected<std::string, std::string> refresh(const std::string& text, const std::vector<std::string>& block) {
        auto lines = withCommon(splitLines(text), block);
        if (!lines)
            return std::unexpected(lines.error());

        const auto manifest = trail::parse(text);
        if (!manifest)
            return std::unexpected(manifest.error());
        const auto docs = paramDocs(*manifest);
        if (!docs)
            return std::unexpected(docs.error());

        // The documented keys, each with its doc.
        static const std::regex            DOC_LINE{R"(^#[ \t]*doc:(.*)$)"};
        std::map<std::string, std::string> documented;
        for (const auto& line : *lines) {
            std::smatch m;
            if (!std::regex_match(line, m, DOC_LINE))
                continue;
            std::istringstream words{m[1].str()};
            for (std::string word; words >> word;) {
                const auto colon = word.find(':');
                if (colon == std::string::npos)
                    return std::unexpected(std::format("doc: \"{}\" isn't <layer>:<key>", word));
                const auto layer = docs->find(word.substr(0, colon));
                if (layer == docs->end())
                    return std::unexpected(std::format("doc: \"{}\" names no layer of this preset (or source)", word));
                const auto key = layer->second.find(word.substr(colon + 1));
                if (key == layer->second.end() || key->second.empty())
                    return std::unexpected(std::format("doc: \"{}\" has no param doc (// <doc> on its pragma)", word));
                if (!documented.emplace(word, key->second).second)
                    return std::unexpected(std::format("doc: \"{}\" listed twice", word));
            }
        }

        // Each key line's new trailing comment, then the column per run of
        // consecutive key lines.
        std::set<std::string>                written;
        std::vector<std::optional<SKeyLine>> keys(lines->size());
        for (size_t i = 0; i < lines->size(); ++i) {
            auto k = keyLine((*lines)[i]);
            if (!k)
                continue;
            const auto doc = documented.find(k->key);
            if (doc != documented.end())
                written.insert(k->key);
            if (k->comment.starts_with("#!"))
                k->comment = trimRight(k->comment);
            else
                k->comment = doc == documented.end() ? "" : "# " + doc->second;
            keys[i] = std::move(*k);
        }
        for (const auto& [key, doc] : documented)
            if (!written.contains(key))
                return std::unexpected(std::format("doc: \"{}\" isn't set in this preset", key));

        std::string out;
        for (size_t i = 0; i < lines->size();) {
            if (!keys[i]) {
                out += (*lines)[i++] + '\n';
                continue;
            }
            size_t end = i;
            size_t col = 0;
            for (; end < lines->size() && keys[end]; ++end)
                if (!keys[end]->comment.empty())
                    col = std::max(col, keys[end]->code.size() + 2);
            for (; i < end; ++i) {
                const auto& k = *keys[i];
                if (k.comment.empty())
                    out += k.code + '\n';
                else
                    out += k.code + std::string(col - k.code.size(), ' ') + k.comment + '\n';
            }
        }
        return out;
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const bool               check = !args.empty() && args.front() == "--check";
    if (check)
        args.erase(args.begin());
    if (args.size() < 2) {
        std::cerr << "usage: preset-docs [--check] <template> <preset.conf>...\n";
        return 2;
    }

    const auto block = commonBlock(args.front());
    if (!block) {
        std::cerr << "error: " << block.error() << '\n';
        return 1;
    }

    int status = 0;
    for (size_t i = 1; i < args.size(); ++i) {
        const auto& path = args[i];
        const auto  text = readFile(path);
        if (!text) {
            std::cerr << std::format("error: can't read {}\n", path);
            status = 1;
            continue;
        }
        const auto next = refresh(*text, *block);
        if (!next) {
            std::cerr << std::format("error: {}: {}\n", path, next.error());
            status = 1;
            continue;
        }
        if (*next == *text)
            continue;
        if (check) {
            std::cerr << std::format("error: {} is stale, run make preset-docs\n", path);
            status = 1;
            continue;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << *next;
        if (!out) {
            std::cerr << std::format("error: can't write {}\n", path);
            status = 1;
        } else
            std::cout << std::format("updated {}\n", path);
    }
    return status;
}
