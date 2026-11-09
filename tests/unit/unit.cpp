// Unit tests for the Hyprland-free parts: parameters and the `params` setting, shader preprocessing and
// pair checks, trail manifests and the embedded built-ins, layer values,
// the crash guard, sources (ring, spring chain, pointer gate), the pin's
// RENDER_POST monitor, warp paths and the emit settings. `make test-unit` builds and runs this, then
// links every shipped shader pair with glslangValidator.
//
// Rules for every test in tests/ (unit, compat, smoke):
//  1. Test contracts and behavior, never shipped values. What a shipped
//     preset or shader contains (names, layer names, widths, colors,
//     parameter defaults and ranges) is not a test subject: renaming,
//     retuning or adding shipped content never requires a test edit.
//  2. Check shipped assets with one generic pass over their directory
//     (hyprtail/presets, hyprtail/shaders), never by name. Each is embedded,
//     byte-identical to its file, and accepted by the code the plugin runs
//     on it (preprocessing, manifest parsing, layer key checks, linking).
//  3. No copies of production code in tests. Logic that needs Hyprland
//     headers moves to a Hyprland-free file that the test links.
//  4. Each check names the failure it catches. A check against a replaced
//     implementation is deleted once the replacement has landed.
//  5. Tests that need a trail or shader write their own fixture. Shipped
//     content is used only where the shipped path itself is under test.
//
// No compositor, no GL: runs anywhere.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../src/ConfigParse.hpp"
#include "../../src/CrashGuard.hpp"
#include "../../src/LayerValues.hpp"
#include "../../src/Manifest.hpp"
#include "../../src/Params.hpp"
#include "../../src/PointerGate.hpp"
#include "../../src/PostStageMonitor.hpp"
#include "../../src/ShaderPair.hpp"
#include "../../src/ShaderSource.hpp"
#include "../../src/Source.hpp"
#include "../../src/SpringChain.hpp"
#include "../../src/TrailBuffer.hpp"
#include "../../src/TrailResolve.hpp"
#include "../../src/WarpPath.hpp"

#include <hyprutils/animation/BezierCurve.hpp>

#include <sys/wait.h>
#include <unistd.h>

using namespace hyprtail;

static int s_failed = 0;
static int s_passed = 0;

#define CHECK(cond)                                                                                                                                                                \
    do {                                                                                                                                                                           \
        if (cond)                                                                                                                                                                  \
            ++s_passed;                                                                                                                                                            \
        else {                                                                                                                                                                     \
            ++s_failed;                                                                                                                                                            \
            std::cerr << std::format("FAIL {}:{}: {}\n", __FILE__, __LINE__, #cond);                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static void testParams() {
    using namespace params;

    auto f = parseDecl("float width 8 0 512");
    CHECK(f && f->type == eType::FLOAT && f->name == "width" && f->def.x == 8.0 && f->min == 0.0 && f->max == 512.0);
    CHECK(!parseDecl("float width 600 0 512"));      // default out of range
    CHECK(!parseDecl("float ht_width 1"));           // reserved prefix
    CHECK(!parseDecl("float Width 1"));              // uppercase first
    CHECK(!parseDecl("colour c 0xff000000"));        // unknown type
    CHECK(!parseDecl("color c rgba(ff0000ff) 0 1")); // colors take no range
    CHECK(!parseDecl("float a"));                    // missing default

    // A trailing "// <doc>" is the param's doc, trimmed; it doesn't count as
    // an argument, and without one the doc is empty.
    CHECK(f && f->doc.empty());
    auto documented = parseDecl("float width 8 0 512   //  full width, px  ");
    CHECK(documented && documented->doc == "full width, px" && documented->max == 512.0);
    auto unranged = parseDecl("color tint rgba(11223344) // tint");
    CHECK(unranged && unranged->doc == "tint" && unranged->def.argb == 0x44112233u);
    CHECK(parseDecl("float a 1 //") && parseDecl("float a 1 //")->doc.empty());
    CHECK(!parseDecl("float a // 1")); // the doc doesn't supply arguments

    auto c = parseDecl("color tint rgba(11223344)");
    CHECK(c && c->def.argb == 0x44112233u);
    CHECK(parseValue(eType::COLOR, "0x80ff0000")->argb == 0x80ff0000u);
    CHECK(parseValue(eType::COLOR, "rgb(00ff00)")->argb == 0xff00ff00u);
    CHECK(!parseValue(eType::COLOR, "red"));

    CHECK(parseValue(eType::INT, "3") && !parseValue(eType::INT, "3.5"));
    CHECK(parseValue(eType::BOOL, "true")->x == 1.0 && parseValue(eType::BOOL, "no")->x == 0.0 && !parseValue(eType::BOOL, "maybe"));

    // ruleTruthy: true/1/yes/on (case-insensitive) suppress; everything else,
    // including false/empty/garbage, doesn't -- and never fails.
    CHECK(ruleTruthy("true") && ruleTruthy("1") && ruleTruthy("yes") && ruleTruthy("on"));
    CHECK(ruleTruthy("TRUE") && ruleTruthy("Yes") && ruleTruthy("ON") && ruleTruthy("  true  "));
    CHECK(!ruleTruthy("false") && !ruleTruthy("0") && !ruleTruthy("no") && !ruleTruthy("off"));
    CHECK(!ruleTruthy("") && !ruleTruthy("maybe") && !ruleTruthy("truee"));
    auto v = parseValue(eType::VEC2, "1.5,-2");
    CHECK(v && v->x == 1.5 && v->y == -2.0);
    CHECK(!parseValue(eType::VEC2, "1.5"));

    // Formatting round-trips.
    CHECK(parseValue(eType::COLOR, format(*parseValue(eType::COLOR, "rgba(1a66ffff)")))->argb == 0xff1a66ffu);

    // Expressions.
    const auto lookup = [](std::string_view n) -> std::optional<double> {
        if (n == "width")
            return 8.0;
        if (n == "miter_limit")
            return 2.0;
        return std::nullopt;
    };
    auto e = CExpr::parse("width * 0.5 * miter_limit + 1");
    CHECK(e && e->eval(lookup) && *e->eval(lookup) == 9.0);
    CHECK(e && e->names().size() == 2);
    CHECK(*CExpr::parse("(1 + 2) * 3")->eval(lookup) == 9.0);
    CHECK(*CExpr::parse("1 + 2 * 3")->eval(lookup) == 7.0);
    CHECK(*CExpr::parse("8 - 2 - 1")->eval(lookup) == 5.0); // left-associative
    CHECK(*CExpr::parse("8 / 4 / 2")->eval(lookup) == 1.0);
    CHECK(!CExpr::parse("-1"));        // no unary minus
    CHECK(!CExpr::parse("max(1, 2)")); // no functions
    CHECK(!CExpr::parse("(1 + 2"));
    CHECK(!CExpr::parse("1 +"));
    CHECK(!CExpr::parse(""));
    CHECK(!CExpr::parse("1 2"));
    CHECK(!CExpr::parse("unknown")->eval(lookup)); // unknown name fails at eval
    CHECK(!CExpr::parse("1 / 0")->eval(lookup));

    // applyOverrides: each value parsed as its declaration's type and
    // range-checked. An unknown name or a bad value is reported, naming it
    // and the owner, and the value before it stays.
    {
        std::vector<std::pair<SDecl, SValue>> all;
        for (const auto* text : {"float width 8 0 32", "int count 4 1 64", "color tint rgba(ff0000ff)"})
            if (const auto d = parseDecl(text))
                all.emplace_back(*d, d->def);
        CHECK(all.size() == 3);

        std::string problems;
        auto        set = all;
        applyOverrides(set, {{"width", "12.5"}, {"count", "8"}, {"tint", "rgb(00ff00)"}}, "layer", problems);
        CHECK(problems.empty() && set[0].second.x == 12.5 && set[1].second.x == 8.0 && set[2].second.argb == 0xff00ff00u);

        const auto refused = [&](const char* name, const char* text) {
            auto        copy = all;
            std::string why;
            applyOverrides(copy, {{name, text}}, "layer", why);
            const bool unchanged = std::ranges::equal(copy, all, [](const auto& a, const auto& b) { return format(a.second) == format(b.second); });
            return unchanged && why.contains(name);
        };
        CHECK(refused("width", "33"));   // above max
        CHECK(refused("width", "-1"));   // below min
        CHECK(refused("width", "wide")); // not a number
        CHECK(refused("count", "2.5"));  // not an int
        CHECK(refused("tint", "red"));   // not a color
        CHECK(refused("widht", "1"));    // unknown
        CHECK(refused("width_a", "1"));  // only starts like a name

        std::string why;
        auto        copy = all;
        applyOverrides(copy, {{"bogus", "1"}}, "source", why);
        CHECK(why.contains("not a parameter of this source"));
    }

    // The `params` setting: whitespace-separated <layer>:<name>=<value>
    // tokens. A malformed token is reported and the rest are kept.
    {
        const auto p = parseParamsString("  trail:width=12\tidle:color_a=rgba(ff0000ff)  sparks:fade_ms=300 a:b=c=d ");
        CHECK(p.problems.empty() && p.entries.size() == 4);
        if (p.entries.size() == 4) {
            CHECK(p.entries[0].layer == "trail" && p.entries[0].name == "width" && p.entries[0].value == "12");
            CHECK(p.entries[1].layer == "idle" && p.entries[1].name == "color_a" && p.entries[1].value == "rgba(ff0000ff)");
            CHECK(p.entries[3].layer == "a" && p.entries[3].name == "b" && p.entries[3].value == "c=d"); // value: all after the first '='
        }
        const auto bad = parseParamsString("width=12 trail:width 12 =x :a=1 a:=1 a=b:c trail:width=12");
        CHECK(bad.problems.size() == 7 && bad.entries.size() == 1 && bad.entries[0].name == "width");
        const auto none = parseParamsString("   ");
        CHECK(none.entries.empty() && none.problems.empty());

        // An empty value is a typo (no type takes one), said as such.
        const auto empty = parseParamsString("trail:width= trail:fade_ms=300");
        CHECK(empty.entries.size() == 1 && empty.entries[0].name == "fade_ms");
        CHECK(empty.problems.size() == 1 && empty.problems[0].contains("\"trail:width=\" has no value"));
    }
}

static std::expected<shader::SSource, std::string> pp(const std::string& text, shader::eStage stage) {
    return shader::preprocess(text, "test", {}, stage);
}

static void testShaderSource() {
    using shader::eStage;
    using shader::eTopology;

    // A stage value "builtin:<file>" (preset stage or layerN override) names
    // the embedded "shaders/<file>"; anything else is a path.
    CHECK(shader::builtinStageKey("builtin:x.vert") == "shaders/x.vert");
    CHECK(!shader::builtinStageKey("shaders/x.vert"));
    CHECK(!shader::builtinStageKey("~/builtin:x.vert"));

    const std::string vert = "#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\n#pragma hyprtail param float width 8 0 512\n"
                             "#pragma hyprtail padding width + 1\nvoid main() {}\n";
    auto              v    = pp(vert, eStage::VERTEX);
    CHECK(v.has_value());
    if (v) {
        CHECK(v->topology == eTopology::PATH);
        CHECK(v->params.size() == 1 && v->params[0].decl.name == "width");
        CHECK(v->padding.size() == 1);
        CHECK(v->text.contains("uniform float width;"));
        CHECK(v->text.contains("ht_a_p0Pos")); // path prelude injected
        CHECK(v->sourceNames.size() == 2);     // main + prelude
    }

    // A param pragma's "// <doc>" stays out of the GLSL: the line becomes
    // the bare uniform, and the doc is kept on the declaration.
    const auto documented = pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\n#pragma hyprtail param float width 8 0 512 // full width, px\n"
                               "void main() {}\n",
                               eStage::VERTEX);
    CHECK(documented && documented->params.size() == 1 && documented->params[0].decl.doc == "full width, px");
    CHECK(documented && documented->text.contains("uniform float width;\n") && !documented->text.contains("full width"));

    // Missing contract, contract not first, wrong version.
    CHECK(!pp("#version 300 es\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail param float a 1\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 1\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::VERTEX));

    // Topology: required in vertex, forbidden in fragment, one only, known.
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology path\n#pragma hyprtail topology quad\nvoid main() {}\n", eStage::VERTEX));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology ribbon\nvoid main() {}\n", eStage::VERTEX));

    // Quad prelude.
    auto q = pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology quad\nvoid main() {}\n", eStage::VERTEX);
    CHECK(q && q->topology == eTopology::QUAD && q->text.contains("ht_corner") && !q->text.contains("ht_a_p0Pos"));

    // Instanced topology: K is a literal 1..64 or a param name.
    const std::string instHead = "#version 300 es\n#pragma hyprtail contract 2\n";
    const auto        inst     = [&](const std::string& topology, const std::string& rest = "") {
        return pp(instHead + "#pragma hyprtail topology " + topology + "\n" + rest + "void main() {}\n", eStage::VERTEX);
    };

    auto i8 = inst("instanced 8");
    CHECK(i8 && i8->topology == eTopology::INSTANCED && i8->instances.literal == 8 && i8->instances.param.empty());
    CHECK(i8 && i8->text.contains("ht_a_pos") && i8->text.contains("ht_instance") && i8->text.contains("uniform int ht_K;") && !i8->text.contains("ht_a_p0Pos"));
    CHECK(inst("instanced 1") && inst("instanced 64"));
    CHECK(!inst("instanced 0") && !inst("instanced 65") && !inst("instanced 99999999999999999999")); // outside 1..64
    CHECK(!inst("instanced"));                                                                       // K is required
    CHECK(!inst("instanced -1") && !inst("instanced 1.5") && !inst("instanced Copies"));             // neither a count nor a param name
    CHECK(!inst("instanced 8 9"));
    CHECK(!inst("path 4") && !inst("quad 1")); // no options
    auto ip = inst("instanced copies", "#pragma hyprtail param int copies 4 1 64\n");
    CHECK(ip && ip->instances.param == "copies" && ip->instances.literal == 0);
    CHECK(shader::topologyText(eTopology::INSTANCED, i8->instances) == "instanced 8");
    CHECK(ip && shader::topologyText(eTopology::INSTANCED, ip->instances) == "instanced copies");
    CHECK(shader::topologyText(eTopology::PATH, {}) == "path" && shader::topologyText(eTopology::QUAD, {}) == "quad");

    // K param: declared, int, with a range inside 1..64.
    const auto kProblem = [&](const std::string& decl) {
        auto                       v = inst("instanced copies", decl);
        std::vector<params::SDecl> declared;
        if (v)
            for (const auto& p : v->params)
                declared.push_back(p.decl);
        return v ? shader::instanceCountProblem(*v, declared) : std::optional<std::string>{"preprocess failed"};
    };
    CHECK(!kProblem("#pragma hyprtail param int copies 4 1 64\n"));
    CHECK(!kProblem("#pragma hyprtail param int copies 4 2 16\n"));
    CHECK(kProblem(""));                                             // not declared
    CHECK(kProblem("#pragma hyprtail param float copies 4 1 64\n")); // not an int
    CHECK(kProblem("#pragma hyprtail param int copies 4\n"));        // no range
    CHECK(kProblem("#pragma hyprtail param int copies 4 0 64\n"));   // min below 1
    CHECK(kProblem("#pragma hyprtail param int copies 4 1 65\n"));   // max above 64
    CHECK(!shader::instanceCountProblem(*i8, {}));                   // a literal needs no param

    // expects: kinds path, quad, instanced; checked against the vertex shader.
    const auto frag = [&](const std::string& expects) { return pp(instHead + "#pragma hyprtail expects " + expects + "\nvoid main() {}\n", eStage::FRAGMENT); };
    CHECK(frag("instanced") && frag("quad,instanced") && frag("path,quad,instanced"));
    CHECK(!frag("instanced 8") && !frag("particles"));
    const auto fInst = frag("quad,instanced"), fPath = frag("path");
    CHECK(fInst && fPath && i8 && ip);
    if (fInst && fPath && i8 && ip) {
        CHECK(!shader::expectsMismatch(*i8, *fInst) && !shader::expectsMismatch(*ip, *fInst));
        const auto bad = shader::expectsMismatch(*i8, *fPath);
        CHECK(bad && bad->contains("expects topology path") && bad->contains("declares instanced 8"));
        const auto badParam = shader::expectsMismatch(*ip, *fPath);
        CHECK(badParam && badParam->contains("declares instanced copies"));
        const auto quad = pp(instHead + "#pragma hyprtail topology quad\nvoid main() {}\n", eStage::VERTEX);
        CHECK(quad && shader::expectsMismatch(*quad, *fInst).has_value() == false);
        CHECK(quad && shader::expectsMismatch(*quad, *fPath).has_value());
    }
    CHECK(!shader::expectsMismatch(*i8, *pp(instHead + "void main() {}\n", eStage::FRAGMENT))); // no expects: any topology

    // The prelude's attributes are exactly the locations the loader says it
    // feeds (the program contract check refuses anything else).
    for (const auto t : {eTopology::PATH, eTopology::INSTANCED}) {
        const auto       src = pp(instHead + "#pragma hyprtail topology " + (t == eTopology::PATH ? "path" : "instanced 2") + "\nvoid main() {}\n", eStage::VERTEX);
        std::vector<int> found;
        if (src) {
            static const std::regex RE{R"(layout\(location = (\d+)\) in )"};
            for (auto it = std::sregex_iterator(src->text.begin(), src->text.end(), RE); it != std::sregex_iterator(); ++it)
                found.push_back(std::stoi((*it)[1]));
        }
        std::ranges::sort(found);
        CHECK(!found.empty() && found == shader::preludeAttribLocations(t));
    }
    CHECK(shader::preludeAttribLocations(eTopology::INSTANCED).size() == 5 && shader::preludeAttribLocations(eTopology::QUAD).empty());
    CHECK(std::ranges::find(shader::preludeUniforms(), std::string{"ht_K"}) != shader::preludeUniforms().end());

    // Reserved and duplicate params, bad padding, unknown pragma.
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail param float fade_ms 1\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail param float a 1\n#pragma hyprtail param float a 1\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail padding max(1, 2)\nvoid main() {}\n", eStage::FRAGMENT));
    CHECK(!pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail glow 1\nvoid main() {}\n", eStage::FRAGMENT));
}

namespace {
    const std::string HEAD = "#version 300 es\n#pragma hyprtail contract 2\n";

    // A pair from two made-up stages, preprocessed as the loader does.
    std::optional<SShaderPair> makePair(const std::string& vertBody, const std::string& fragBody) {
        auto vert = pp(HEAD + vertBody + "void main() {}\n", shader::eStage::VERTEX);
        auto frag = pp(HEAD + fragBody + "void main() {}\n", shader::eStage::FRAGMENT);
        if (!vert || !frag)
            return std::nullopt;
        return SShaderPair{.vert = std::move(*vert), .frag = std::move(*frag)};
    }

    std::optional<std::string> pairProblem(const std::string& vertBody, const std::string& fragBody) {
        const auto pair = makePair(vertBody, fragBody);
        if (!pair)
            return "preprocess failed";
        const auto info = programInfo(*pair);
        return info ? std::nullopt : std::optional{info.error()};
    }
}

// The pair checks that need no GL (ShaderPair.cpp): the two stages'
// declarations fit together, and the varying scan that explains a failed
// link.
static void testShaderPair() {
    const std::string path = "#pragma hyprtail topology path\n";

    // Params merge by name; one declared in both stages must be declared
    // identically.
    {
        const auto pair =
            makePair(path + "#pragma hyprtail param float width 8 0 512\n", "#pragma hyprtail param float width 8 0 512\n#pragma hyprtail param color tint 0xffffffff\n");
        const auto info = pair ? programInfo(*pair) : std::unexpected(std::string{"preprocess failed"});
        CHECK(info && info->topology == shader::eTopology::PATH && info->params.size() == 2);
    }
    for (const auto* frag : {"#pragma hyprtail param float width 8 0 256\n", "#pragma hyprtail param float width 4 0 512\n", "#pragma hyprtail param int width 8 0 512\n"}) {
        const auto problem = pairProblem(path + "#pragma hyprtail param float width 8 0 512\n", frag);
        CHECK(problem && problem->contains("width is declared differently"));
    }

    // `expects` against the vertex shader's topology.
    CHECK(pairProblem(path, "#pragma hyprtail expects quad\n"));
    CHECK(!pairProblem(path, "#pragma hyprtail expects path,quad\n"));

    // An instanced K param may be declared by either stage, and must fit.
    const std::string inst = "#pragma hyprtail topology instanced copies\n";
    CHECK(!pairProblem(inst, "#pragma hyprtail param int copies 4 1 64\n"));
    CHECK(pairProblem(inst, ""));
    CHECK(pairProblem(inst, "#pragma hyprtail param int copies 4 1 65\n"));

    // Padding: float, int or bool params of either stage, or reserved ones.
    CHECK(!pairProblem(path + "#pragma hyprtail padding width * 2 + fade_ms\n", "#pragma hyprtail param float width 8 0 512\n"));
    const auto unknown = pairProblem(path + "#pragma hyprtail padding width * 2\n", "");
    CHECK(unknown && unknown->contains("padding uses \"width\""));
    CHECK(pairProblem(path + "#pragma hyprtail param color tint 0xffffffff\n#pragma hyprtail padding tint\n", ""));
    CHECK(pairProblem(path + "#pragma hyprtail param vec2 dir 0,1\n#pragma hyprtail padding dir\n", ""));

    // varyingCheck: nothing for stages that only use the standard varyings;
    // a fragment input no vertex output matches, and one of another type,
    // named in the message.
    {
        const auto fits = makePair(path, "");
        CHECK(fits && !varyingCheck(*fits));
        const auto bad = makePair(path + "out float v_a;\nout vec2 v_c;\n", "in float v_a;\nin float v_b;\nin float v_c;\n");
        const auto msg = bad ? varyingCheck(*bad) : std::nullopt;
        CHECK(msg && msg->contains("reads `v_b`") && msg->contains("declares `v_c` as float") && !msg->contains("`v_a`") && !msg->contains("ht_vAge`"));
    }
}

// Trail manifest grammar (Manifest.cpp).
static void testManifestParse() {
    const auto ok = trail::parse("# comment\n\ncontract = 2\ndescription = a test\nlayers = a, b\nsource = spring\n"
                                 "a:vertex = shaders/x.vert   # trailing comment\nb:width = 3\nsource:freq_hz = 2\n");
    CHECK(ok && ok->description == "a test" && ok->layers == std::vector<std::string>({"a", "b"}) && ok->sourceKind == "spring");
    CHECK(ok && ok->layerKeys.at("a").at("vertex") == "shaders/x.vert" && ok->layerKeys.at("b").at("width") == "3" && !ok->layerKeys.contains("source"));
    CHECK(ok && ok->sourceKeys.at("freq_hz") == "2");

    // A '#' ends any value, so a trailing comment (preset-docs' generated
    // "# <doc>" and hand-written "#! ...") never leaks into it: on top-level
    // keys, layer keys and source keys, with or without a space before it.
    const auto commented = trail::parse("contract = 2 # c\ndescription = a test  #! note\nlayers = a, b# c\n"
                                        "a:width = 3       # px\nb:color = 0xff00ff00#!x\nsource:min_spacing = 4 # px\n");
    CHECK(commented && commented->description == "a test" && commented->layers == std::vector<std::string>({"a", "b"}));
    CHECK(commented && commented->layerKeys.at("a").at("width") == "3" && commented->layerKeys.at("b").at("color") == "0xff00ff00" &&
          commented->sourceKeys.at("min_spacing") == "4");

    const auto minimal = trail::parse("contract = 2\nlayers = a\n");
    CHECK(minimal && minimal->sourceKind == source::DEFAULT_KIND && minimal->layerKeys.empty());

    for (const auto* bad : {
             "layers = a\n",                                 // no contract
             "contract = 3\nlayers = a\n",                   // another contract
             "contract = 2\n",                               // no layers
             "contract = 2\nlayers = ,\n",                   // empty layers
             "contract = 2\nlayers = a, b, c, d, e\n",       // over 4
             "contract = 2\nlayers = a, a\n",                // listed twice
             "contract = 2\nlayers = source\n",              // reserved name
             "contract = 2\nlayers = a\ncolor = red\n",      // unknown key
             "contract = 2\nlayers = a\nb:width = 1\n",      // layer not listed
             "contract = 2\nlayers = a\na:w = 1\na:w = 2\n", // duplicate key
             "contract = 2\ncontract = 2\nlayers = a\n",     // duplicate contract
             "contract = 2\nlayers = a\nsource = rope\n",    // unknown source
             "contract = 2\nlayers = a\nnot a line\n",       // no '='
             "contract = 2\nlayers = a\n:w = 1\n",           // empty layer
         })
        CHECK(!trail::parse(bad));
}

// The built-in trails (rule 2), by directory, never by name: every
// hyprtail/presets/*.conf is embedded under its stem with identical bytes
// and nothing else is; each parses with the real parser; each layer names
// embedded shaders (a plain path also identical to hyprtail/<path>, so a
// copied folder draws the same) that fit together as a program; and every
// key resolves cleanly for that program and the trail's source, as the
// plugin resolves them. So does the params string of each
// "# variant: <params string> | <short description>" comment line (the
// README's suggested tweaks, scripts/readme-presets.sh): no malformed token,
// unknown layer or name, or out-of-range value, applied over the preset as
// the plugin applies `params` (main.cpp). Run from the repo root.
static void testBuiltinManifests() {
    namespace fs = std::filesystem;
    if (!fs::is_directory("hyprtail/presets"))
        return;

    const auto readFile = [](const fs::path& p) -> std::optional<std::string> {
        std::ifstream in(p, std::ios::binary);
        if (!in)
            return std::nullopt;
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };

    std::set<std::string> files;
    for (const auto& entry : fs::directory_iterator("hyprtail/presets"))
        if (entry.path().extension() == ".conf")
            files.insert(entry.path().stem().string());
    const std::set<std::string> embedded(trail::builtinNames().begin(), trail::builtinNames().end());
    for (const auto& name : files)
        if (!embedded.contains(name))
            std::cerr << std::format("hyprtail/presets/{}.conf isn't embedded (src/Manifest.cpp)\n", name);
    CHECK(files == embedded);

    // One check per preset, pass or fail, so the count doesn't move when a
    // preset is fixed. fail() reports each problem; the preset's check runs
    // once at the end of its pass (or where the pass stops early).
    for (const auto name : trail::builtinNames()) {
        bool       clean = true;
        const auto fail  = [&](const std::string& why) {
            std::cerr << std::format("builtin:{}: {}\n", name, why);
            clean = false;
        };
        const auto text = trail::builtinManifest(name);
        if (readFile(std::format("hyprtail/presets/{}.conf", name)) != std::string{text})
            fail("embedded copy differs from hyprtail/presets/" + std::string{name} + ".conf");

        const auto manifest = trail::parse(text);
        if (!manifest) {
            fail(manifest.error());
            CHECK(clean);
            continue;
        }

        std::string problems;
        source::resolve(manifest->sourceKind, manifest->sourceKeys, {}, problems);
        if (!problems.empty())
            fail("source settings:" + problems);

        // Variant params strings, split the way main.cpp splits `params`:
        // "source:" entries to the source, the rest per layer.
        struct SVariant {
            std::string                                               params;
            std::map<std::string, std::string>                        source;
            std::map<std::string, std::map<std::string, std::string>> layers;
        };
        std::vector<SVariant> variants;
        {
            static const std::regex variantLine{R"(^#[ \t]*variant:(.*)$)"};
            const auto              trim = [](std::string_view v) {
                const auto b = v.find_first_not_of(" \t");
                return b == std::string_view::npos ? std::string{} : std::string{v.substr(b, v.find_last_not_of(" \t") - b + 1)};
            };
            std::istringstream lines{std::string{text}};
            for (std::string line; std::getline(lines, line);) {
                std::smatch m;
                if (!std::regex_match(line, m, variantLine))
                    continue;
                const std::string body = m[1];
                const auto        bar  = body.find('|');
                if (bar == std::string::npos || trim(body.substr(0, bar)).empty() || trim(body.substr(bar + 1)).empty()) {
                    fail("variant line isn't \"<params string> | <short description>\": " + line);
                    continue;
                }
                SVariant   v{.params = trim(body.substr(0, bar))};
                const auto parsed = params::parseParamsString(v.params);
                for (const auto& prob : parsed.problems)
                    fail(std::format("variant \"{}\": {}", v.params, prob));
                for (const auto& e : parsed.entries) {
                    if (e.layer == source::KEY_PREFIX)
                        v.source[e.name] = e.value;
                    else if (std::ranges::find(manifest->layers, e.layer) == manifest->layers.end())
                        fail(std::format(R"(variant "{}": unknown layer "{}")", v.params, e.layer));
                    else
                        v.layers[e.layer][e.name] = e.value;
                }
                problems.clear();
                source::resolve(manifest->sourceKind, manifest->sourceKeys, v.source, problems);
                if (!problems.empty())
                    fail(std::format("variant \"{}\": source settings:{}", v.params, problems));
                variants.push_back(std::move(v));
            }
        }

        for (const auto& layer : manifest->layers) {
            const auto it   = manifest->layerKeys.find(layer);
            const auto keys = it == manifest->layerKeys.end() ? std::map<std::string, std::string>{} : it->second;

            // A stage's embedded key, as the loader resolves it in a
            // built-in trail (Trail.cpp resolveStage).
            const auto stage = [&](const char* key, shader::eStage st) -> std::optional<shader::SSource> {
                const auto v = keys.find(key);
                if (v == keys.end()) {
                    fail(std::format("layer {} has no {}", layer, key));
                    return std::nullopt;
                }
                const auto embedded = shader::builtinStageKey(v->second);
                const bool prefixed = embedded.has_value();
                const auto ref      = embedded.value_or(v->second);
                const auto src      = shader::builtin(ref);
                if (src.empty()) {
                    fail(std::format("layer {}: {} isn't an embedded shader", layer, v->second));
                    return std::nullopt;
                }
                if (!prefixed && readFile(fs::path{"hyprtail"} / ref) != std::string{src})
                    fail(std::format("layer {}: hyprtail/{} is missing or differs from the embedded copy", layer, ref));
                auto out = shader::preprocess(src, ref, {}, st);
                if (!out) {
                    fail(std::format("layer {}: {}", layer, out.error()));
                    return std::nullopt;
                }
                return std::move(*out);
            };
            auto vert = stage("vertex", shader::eStage::VERTEX);
            auto frag = stage("fragment", shader::eStage::FRAGMENT);
            if (!vert || !frag)
                continue;

            const auto info = programInfo(SShaderPair{.vert = std::move(*vert), .frag = std::move(*frag)});
            if (!info) {
                fail(std::format("layer {}: {}", layer, info.error()));
                continue;
            }
            auto values = keys;
            values.erase("vertex");
            values.erase("fragment");
            problems.clear();
            resolveValues(*info, values, {}, problems);
            if (!problems.empty())
                fail(std::format("layer {}:{}", layer, problems));
            for (const auto& v : variants) {
                const auto overrides = v.layers.find(layer);
                if (overrides == v.layers.end())
                    continue;
                problems.clear();
                resolveValues(*info, values, overrides->second, problems);
                if (!problems.empty())
                    fail(std::format("variant \"{}\": layer {}:{}", v.params, layer, problems));
            }
        }
        CHECK(clean);
    }
}

// A layer's values (LayerValues.cpp): reserved defaults,
// then the program's, then the manifest's, then the `params` setting's.
// A shipped preset copied into a config root with no shaders folder loads
// as a user preset (trail::resolve, the plugin's own loader) without
// error, and every stage resolves to an embedded shader: no file path, so
// nothing missing. Found by glob, one check per preset.
static void testPresetCopies() {
    namespace fs = std::filesystem;
    if (!fs::is_directory("hyprtail/presets"))
        return;

    char        tmpl[] = "/tmp/hyprtail-unit-XXXXXX";
    const char* dir    = mkdtemp(tmpl);
    if (!dir) {
        CHECK(false); // mkdtemp failing is an environment problem worth flagging
        return;
    }
    const char*                      saved    = std::getenv("XDG_CONFIG_HOME");
    const std::optional<std::string> previous = saved ? std::optional<std::string>{saved} : std::nullopt;
    setenv("XDG_CONFIG_HOME", dir, 1);
    const fs::path presets = fs::path{dir} / "hypr" / "hyprtail" / "presets";
    fs::create_directories(presets);

    for (const auto& entry : fs::directory_iterator("hyprtail/presets")) {
        if (entry.path().extension() != ".conf")
            continue;
        fs::copy_file(entry.path(), presets / entry.path().filename());
        const auto               name = "presets/" + entry.path().filename().string();
        std::vector<std::string> missing;
        const auto               r = trail::resolve(name, missing);
        std::string              why;
        if (!r)
            why = r.error();
        for (const auto& m : missing)
            why += " missing " + m;
        if (r)
            for (const auto& l : r->layers)
                if (!l.vertPath.empty() || !l.fragPath.empty())
                    why += std::format(" layer {} names a shader file, not builtin:<file>", l.name);
        if (!why.empty())
            std::cerr << std::format("copied {}: {}\n", name, why);
        CHECK(why.empty());
    }

    if (previous)
        setenv("XDG_CONFIG_HOME", previous->c_str(), 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(fs::path{dir});
}

static void testLayerValues() {
    const auto decl = [](const char* text) { return *params::parseDecl(text); };
    const auto pad  = [](const char* text) { return shader::SPadding{.expr = *params::CExpr::parse(text), .where = "test:1"}; };
    const auto res  = [](const SProgramInfo& info, const std::map<std::string, std::string>& manifest, const std::map<std::string, std::string>& paramString) {
        std::string problems;
        auto        r = resolveValues(info, manifest, paramString, problems);
        return std::pair{r, problems};
    };
    const auto value = [](const SResolved& r, const char* name) {
        const auto it = std::ranges::find_if(r.values, [&](const auto& v) { return v.first.name == name; });
        return it == r.values.end() ? -1.0 : it->second.x;
    };

    SProgramInfo pathInfo{.topology = shader::eTopology::PATH, .params = {decl("float width 8 0 32")}, .padding = {pad("width * 2")}};

    // Declared defaults; reserved values are not in `values`.
    {
        const auto [r, problems] = res(pathInfo, {}, {});
        CHECK(problems.empty() && r.values.size() == 1 && value(r, "width") == 8.0 && r.paddingPx == 16.F && r.instances == 0);
        CHECK(r.enabled && r.drawWhenHidden);
        for (const auto& reserved : shader::reservedParams())
            if (reserved.decl.name == "fade_ms")
                CHECK(r.fadeMs == reserved.decl.def.x);
    }

    // The manifest over the defaults, `params` over the manifest; a bad or
    // unknown entry is reported and the value before it stays.
    {
        const auto [r, problems] = res(pathInfo, {{"width", "10"}, {"fade_ms", "300"}}, {{"width", "12"}});
        CHECK(problems.empty() && value(r, "width") == 12.0 && r.fadeMs == 300.0 && r.paddingPx == 24.F);
        const auto [kept, why] = res(pathInfo, {{"width", "10"}}, {{"width", "99"}, {"widht", "1"}});
        CHECK(value(kept, "width") == 10.0 && why.contains("width") && why.contains("widht"));
        const auto [off, none] = res(pathInfo, {}, {{"enabled", "false"}, {"draw_when_cursor_hidden", "no"}});
        CHECK(none.empty() && !off.enabled && !off.drawWhenHidden);
    }

    // A quad layer doesn't draw while the cursor is hidden unless told to.
    {
        SProgramInfo quad{.topology = shader::eTopology::QUAD};
        CHECK(!res(quad, {}, {}).first.drawWhenHidden);
        CHECK(res(quad, {{"draw_when_cursor_hidden", "true"}}, {}).first.drawWhenHidden);
    }

    // Padding: the largest expression, clamped to 0..4096; one that can't be
    // evaluated is reported and ignored.
    {
        SProgramInfo info        = pathInfo;
        info.padding             = {pad("width * 1000"), pad("width / 0"), pad("3")};
        const auto [r, problems] = res(info, {}, {});
        CHECK(r.paddingPx == 4096.F && problems.contains("clamped") && problems.contains("ignoring it"));
    }

    // damage_padding: reserved, per layer, 0 by default, kept apart from the
    // padding expression; `params` over the manifest; out of range keeps the
    // earlier value and is reported.
    {
        CHECK(res(pathInfo, {}, {}).first.damagePaddingPx == 0.F);
        const auto [r, problems] = res(pathInfo, {{"damage_padding", "6"}}, {{"damage_padding", "9"}});
        CHECK(r.damagePaddingPx == 9.F && r.paddingPx == res(pathInfo, {}, {}).first.paddingPx && problems.empty());
        const auto [bad, badProblems] = res(pathInfo, {{"damage_padding", "5000"}}, {});
        CHECK(bad.damagePaddingPx == 0.F && badProblems.contains("damage_padding"));
        CHECK(std::ranges::none_of(r.values, [](const auto& v) { return v.first.name == "damage_padding"; }));
    }

    // Instanced K: a literal, or the param's resolved value.
    {
        SProgramInfo literal{.topology = shader::eTopology::INSTANCED, .instances = {.literal = 8}};
        CHECK(res(literal, {}, {}).first.instances == 8);
        SProgramInfo named{.topology = shader::eTopology::INSTANCED, .instances = {.param = "copies"}, .params = {decl("int copies 4 1 64")}};
        CHECK(res(named, {}, {}).first.instances == 4 && res(named, {{"copies", "10"}}, {{"copies", "16"}}).first.instances == 16);
    }

    // `enabled` before a program exists: `params` over the manifest; unset
    // or unreadable means enabled.
    CHECK(enabledSetting({}, {}));
    CHECK(!enabledSetting({{"enabled", "false"}}, {}));
    CHECK(enabledSetting({{"enabled", "false"}}, {{"enabled", "true"}}));
    CHECK(enabledSetting({}, {{"enabled", "maybe"}}));
}

static void testCrashGuard() {
    using namespace hyprtail::crashguard;

    // parse: round-trips through format(), and rejects malformed/incomplete
    // text; unknown extra fields are tolerated.
    const SKey key{"abc1234", "deadbeef"};
    const auto text = format(key, "sig-1", 4242);
    const auto m    = parse(text);
    CHECK(m && m->revision == key.revision && m->hyprlandHash == key.hyprlandHash && m->instanceSignature == "sig-1" && m->pid == 4242);

    CHECK(!parse(""));
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\n"));          // missing pid
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\npid=abc\n")); // pid not a number
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\npid=-1\n"));  // pid not positive
    CHECK(!parse("rev=a\nhyprland=b\ninstance=c\npid=0\n"));
    CHECK(parse("rev=a\nhyprland=b\ninstance=c\npid=5\nextra=ignored\n").has_value());

    // Stale pid detection: a reaped child's pid is guaranteed dead; our own
    // pid is guaranteed alive.
    const pid_t child = fork();
    if (child == 0)
        _exit(0);
    CHECK(child > 0);
    int status = 0;
    if (child > 0)
        waitpid(child, &status, 0);
    const long long deadPid = child;
    const long long livePid = static_cast<long long>(::getpid());

    CHECK(pidIsDead(deadPid));
    CHECK(!pidIsDead(livePid));
    CHECK(!pidIsDead(0));
    CHECK(!pidIsDead(-1));

    // Key match: only revision+hash+dead-pid together indicate an early
    // death; any single mismatch keeps the load going.
    const SMarker deadSameKey{key.revision, key.hyprlandHash, "sig-1", deadPid};
    const SMarker liveSameKey{key.revision, key.hyprlandHash, "sig-1", livePid};
    const SMarker deadOtherRev{"different", key.hyprlandHash, "sig-1", deadPid};
    const SMarker deadOtherHash{key.revision, "different", "sig-1", deadPid};

    CHECK(indicatesEarlyDeath(deadSameKey, key));
    CHECK(!indicatesEarlyDeath(liveSameKey, key));   // live pid: another instance, not a crash
    CHECK(!indicatesEarlyDeath(deadOtherRev, key));  // different build
    CHECK(!indicatesEarlyDeath(deadOtherHash, key)); // different running Hyprland

    // Real file round trip, redirected to a scratch dir so this never
    // touches the real state directory.
    char tmpl[] = "/tmp/hyprtail-unit-XXXXXX";
    if (const char* dir = mkdtemp(tmpl)) {
        setenv("XDG_STATE_HOME", dir, 1);
        CHECK(!readMarker()); // nothing written yet
        CHECK(writeMarker(key, "sig-1", livePid));
        const auto readBack = readMarker();
        CHECK(readBack && readBack->revision == key.revision && readBack->pid == livePid);
        removeMarker();
        CHECK(!readMarker());
        std::filesystem::remove_all(std::filesystem::path{dir});
    } else
        CHECK(false); // mkdtemp failing is an environment problem worth flagging
}

// Per-app suppression state: the enter and exit transitions of
// CPointerGate and what they do to the point buffer, for both sources.
static void testPointerGate() {
    for (const char* kind : {"pointer", "spring"}) {
        auto                  src = source::make(kind, 8, 7);
        CPointerGate          gate;
        const SVec2f          a{10, 10}, b{20, 10}, c{500, 400}, d{510, 400};
        std::vector<SGpuNode> out;

        src->insert(a, 0.0, false);
        src->insert(b, 10.0, false);

        // Not excluded and staying so: no edge, nothing touched.
        const auto gen = src->generation();
        const auto n   = src->size();
        CHECK(n > 0);
        CHECK(!gate.excluded());
        CHECK(gate.update(false, *src) == eGateEdge::NONE);
        CHECK(src->size() == n && src->generation() == gen);

        // Enter: the buffer is dropped, nothing stays to be drawn.
        CHECK(gate.update(true, *src) == eGateEdge::ENTER);
        CHECK(gate.excluded());
        CHECK(src->empty() && src->generation() != gen);
        CHECK(!src->visibleBounds(10.0, 1000.0));

        // Excluded and staying so: no edge, and the buffer is left alone
        // (the caller inserts nothing, but update() must not churn it).
        const auto emptyGen = src->generation();
        CHECK(gate.update(true, *src) == eGateEdge::NONE);
        CHECK(gate.excluded() && src->empty() && src->generation() == emptyGen);

        // Exit: an empty buffer, so the first new node is joined to nothing
        // from before the excluded window.
        CHECK(gate.update(false, *src) == eGateEdge::EXIT);
        CHECK(!gate.excluded() && src->empty());
        src->insert(c, 500.0, false);
        src->orderedCopy(out, 500.0);
        CHECK(!out.empty() && (out[0].bits & GPU_BIT_SEGMENT_START));
        CHECK(std::ranges::all_of(out, [&](const SGpuNode& node) { return node.posPx == c; })); // nothing from before

        // Back to normal: a not-excluded update keeps what was inserted.
        src->insert(d, 510.0, false);
        const auto before = src->generation();
        const auto m      = src->size();
        CHECK(gate.update(false, *src) == eGateEdge::NONE);
        CHECK(src->size() == m && src->generation() == before);

        // Straight from exit to enter again, and an enter on an empty buffer.
        CHECK(gate.update(true, *src) == eGateEdge::ENTER && src->empty());
        CHECK(gate.update(false, *src) == eGateEdge::EXIT && src->empty());
        CHECK(gate.update(true, *src) == eGateEdge::ENTER && src->empty());
    }
}

// The pin's RENDER_POST monitor (CPostStageMonitor, with std::weak_ptr as
// the weak monitor reference): handed only to the RENDER_POST of the render
// whose RENDER_LAST_MOMENT saw it, at most once, so a render that bails out
// between its stages can't pass a stale monitor to the next RENDER_POST.
static void testPostStageMonitor() {
    const auto                            a = std::make_shared<int>(1), b = std::make_shared<int>(2);
    CPostStageMonitor<std::weak_ptr<int>> post;

    // No render yet: nothing.
    CHECK(post.post().expired());

    // A full render: BEGIN, LAST_MOMENT, POST gets that render's monitor.
    post.begin();
    post.lastMoment(a);
    CHECK(post.post().lock() == a);
    // Handed out once: a second POST without a render gets nothing.
    CHECK(post.post().expired());

    // A render that bails out after LAST_MOMENT, before POST, then a new
    // render that bails out before LAST_MOMENT: its POST must not get a.
    post.begin();
    post.lastMoment(a);
    post.begin();
    CHECK(post.post().expired());

    // The latest LAST_MOMENT wins.
    post.begin();
    post.lastMoment(a);
    post.lastMoment(b);
    CHECK(post.post().lock() == b);

    // A monitor gone by POST reads as none.
    {
        const auto gone = std::make_shared<int>(3);
        post.begin();
        post.lastMoment(gone);
    }
    CHECK(post.post().expired());
}

static void testRing() {
    CTrailRing ring(8, 42);
    ring.insert({0, 0}, 0.0, false);     // first node: segment start
    ring.insert({3, 4}, 10.0, false);    // +5
    ring.insert({6, 8}, 20.0, false);    // +5
    ring.insert({100, 100}, 30.0, true); // break: distance restarts
    ring.insert({100, 110}, 40.0, false);

    std::vector<SGpuNode> out;
    ring.orderedCopy(out, 40.0);
    CHECK(out.size() == 5);
    CHECK(out[0].distPx == 0.F && (out[0].bits & GPU_BIT_SEGMENT_START));
    CHECK(out[2].distPx == 10.F && !(out[2].bits & GPU_BIT_SEGMENT_START));
    CHECK(out[3].distPx == 0.F && (out[3].bits & GPU_BIT_SEGMENT_START));
    CHECK(out[4].distPx == 10.F);
    CHECK(out[4].birthMs == 0.F && out[0].birthMs == -40.F);
    // Seeds differ between nodes, and between rings with other bases.
    CHECK((out[1].bits >> 1) != (out[2].bits >> 1));
    CTrailRing other(8, 43);
    other.insert({0, 0}, 0.0, false);
    std::vector<SGpuNode> out2;
    other.orderedCopy(out2, 0.0);
    CHECK((out2[0].bits >> 1) != (out[0].bits >> 1));

    // Visible range: the newest nodes with age < fade. Nodes
    // are at t = 0, 10, 20, 30 (a segment start), 40; now = 45.
    CHECK(ring.visibleCount(45.0, 100.0) == 5);
    CHECK(ring.visibleCount(45.0, 30.0) == 3); // ages 5, 15, 25 (35 is faded)
    CHECK(ring.visibleCount(45.0, 25.0) == 2); // age 25 is not < 25
    CHECK(ring.visibleCount(45.0, 4.0) == 0);
    CHECK(CTrailRing(4, 1).visibleCount(0.0, 100.0) == 0); // empty

    // Bounds: nodes at (6,8), (100,100), (100,110) are visible at fade 30.
    // Path layers add the next older node (3,4), connected to (6,8); an
    // instanced layer draws visible nodes only.
    const auto withOlder = ring.visibleBounds(45.0, 30.0);
    const auto nodesOnly = ring.visibleBounds(45.0, 30.0, false);
    CHECK(withOlder && withOlder->x1 == 3.F && withOlder->y1 == 4.F && withOlder->x2 == 100.F && withOlder->y2 == 110.F);
    CHECK(nodesOnly && nodesOnly->x1 == 6.F && nodesOnly->y1 == 8.F && nodesOnly->x2 == 100.F && nodesOnly->y2 == 110.F);
    // Fade 20: (100,100) and (100,110) visible; the older node (6,8) is
    // separated by the segment start, so neither variant includes it.
    const auto brk     = ring.visibleBounds(45.0, 20.0);
    const auto brkOnly = ring.visibleBounds(45.0, 20.0, false);
    CHECK(brk && brkOnly && brk->x1 == 100.F && brk->y1 == 100.F && brkOnly->x1 == 100.F && brkOnly->y1 == 100.F);
    CHECK(!ring.visibleBounds(45.0, 4.0, false));

    // After the ring wraps (capacity 4, six inserts keep t = 20..50) and
    // after a resize.
    CTrailRing wrapped(4, 7);
    for (int i = 0; i <= 5; ++i)
        wrapped.insert({static_cast<float>(i), 0.F}, i * 10.0, false);
    CHECK(wrapped.size() == 4 && wrapped.visibleCount(55.0, 100.0) == 4);
    CHECK(wrapped.visibleCount(55.0, 25.0) == 2); // ages 5, 15
    const auto wb = wrapped.visibleBounds(55.0, 25.0, false);
    CHECK(wb && wb->x1 == 4.F && wb->x2 == 5.F);
    wrapped.resize(2);
    CHECK(wrapped.size() == 2 && wrapped.visibleCount(55.0, 100.0) == 2 && wrapped.visibleCount(55.0, 10.0) == 1);
}

// Which nodes an instanced layer draws (forEachInstancedRun): not one alone
// in its segment, the node a path layer draws no span to.
static void testInstancedRuns() {
    using SRun      = std::pair<size_t, size_t>; // from, n
    const auto runs = [](const ISource& s, size_t first, size_t count) {
        std::vector<SGpuNode> nodes;
        s.orderedCopy(nodes, 0.0);
        std::vector<SRun> out;
        forEachInstancedRun(nodes, first, count, [&out](size_t from, size_t n) { out.emplace_back(from, n); });
        return out;
    };

    // The first node of a trail is alone until a second one comes.
    CTrailRing ring(16, 5);
    ring.insert({0, 0}, 0.0, false);
    CHECK(runs(ring, 0, 1).empty());
    ring.insert({1, 0}, 1.0, false);
    CHECK((runs(ring, 0, 2) == std::vector<SRun>{{0, 2}}));

    // A break's destination (a break warp) draws nothing while it is the
    // newest node (instanced particles puffed at a break target).
    ring.insert({2, 0}, 2.0, false);
    ring.insert({500, 500}, 3.0, true);
    CHECK((runs(ring, 0, 4) == std::vector<SRun>{{0, 3}}));
    // Nor when it is the only visible node.
    CHECK(runs(ring, 3, 1).empty());

    // Once a node follows it, it is part of a segment and draws.
    ring.insert({501, 500}, 4.0, false);
    CHECK((runs(ring, 0, 5) == std::vector<SRun>{{0, 5}}));

    // Two breaks in a row: the first target stays alone for good and is cut
    // out of the middle, splitting the draw; the second has a node after it.
    ring.insert({900, 0}, 5.0, true);
    ring.insert({0, 900}, 6.0, true);
    ring.insert({1, 900}, 7.0, false);
    CHECK((runs(ring, 0, 8) == std::vector<SRun>{{0, 5}, {6, 2}}));
    // Only within the visible window: a run never starts before `first`.
    CHECK((runs(ring, 4, 4) == std::vector<SRun>{{4, 1}, {6, 2}}));
    CHECK((runs(ring, 5, 3) == std::vector<SRun>{{6, 2}}));

    // A spring chain starts one segment at its oldest node: every node draws.
    CSpringChainSource chain(6, 9);
    chain.insert({0, 0}, 0.0, true);
    CHECK((runs(chain, 0, chain.size()) == std::vector<SRun>{{0, chain.size()}}));
}

namespace {
    // A Hyprland bezier (hl.curve points, i.e. the two inner control points)
    // through the evaluator the warp curve (warp_p1/warp_p2) uses at runtime.
    warp::FEase bezier(float x1, float y1, float x2, float y2) {
        auto curve = std::make_shared<Hyprutils::Animation::CBezierCurve>();
        curve->setup({Hyprutils::Math::Vector2D{x1, y1}, Hyprutils::Math::Vector2D{x2, y2}});
        return [curve](float x) { return curve->getYForPoint(x); };
    }

    bool sameNodes(const std::vector<warp::SNode>& a, const std::vector<warp::SNode>& b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!(a[i].posPx == b[i].posPx) || a[i].birthMs != b[i].birthMs)
                return false;
        }
        return true;
    }

    // Positions only.
    bool samePositions(const std::vector<warp::SNode>& a, const std::vector<warp::SNode>& b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!(a[i].posPx == b[i].posPx))
                return false;
        }
        return true;
    }

    bool allFinite(const std::vector<warp::SNode>& v) {
        return std::ranges::all_of(v, [](const auto& n) { return std::isfinite(n.posPx.x) && std::isfinite(n.posPx.y) && std::isfinite(n.birthMs); });
    }

    // Progress of p along the chord p0 -> p2: 0 at p0, 1 at p2.
    float along(const SVec2f& p, const SVec2f& p0, const SVec2f& p2) {
        const float dx = p2.x - p0.x, dy = p2.y - p0.y;
        return ((p.x - p0.x) * dx + (p.y - p0.y) * dy) / (dx * dx + dy * dy);
    }

    float gap(const std::vector<warp::SNode>& v, size_t i) { // between node i-1 and i
        return std::hypot(v[i].posPx.x - v[i - 1].posPx.x, v[i].posPx.y - v[i - 1].posPx.y);
    }

    bool near(const SVec2f& a, const SVec2f& b, float tol = 1e-3F) {
        return std::hypot(a.x - b.x, a.y - b.y) <= tol;
    }
}

// Warp easing and landing (WarpPath.*), with plain functions
// and hyprutils' CBezierCurve standing in for the warp curve.
static void testWarpPath() {
    using warp::eShape;
    const warp::FEase identity = [](float x) { return x; };

    // Pre-warp node moving right, 40 ms old; the target down and to the
    // right. SLOW is a warp_speed so low that every warp here takes its whole
    // warp_max_ms, W, to draw.
    const SCursorNode from{.posPx = {100.F, 200.F}, .birthTimeMs = 1000.0, .velocity = {0.5F, 0.F}, .distPx = 0.0, .seed = 1, .segmentStart = false};
    const SVec2f      to{900.F, 650.F};
    const double      now  = 1040.0;
    constexpr double  W    = 120.0;
    constexpr double  SLOW = 1e-6;

    // No curve and the identity as the curve give the same nodes, and
    // positions don't depend on time (a long rest).
    {
        SCursorNode rested = from;
        rested.birthTimeMs = now - 5000.0;
        const auto plain   = warp::nodes(eShape::CURVE, from, to, now, SLOW, W, 2.F, {});
        CHECK(sameNodes(plain, warp::nodes(eShape::CURVE, from, to, now, SLOW, W, 2.F, identity)));
        CHECK(samePositions(plain, warp::nodes(eShape::CURVE, rested, to, now, SLOW, W, 2.F, {})));

        // Hyprland's own "linear" (0,0 / 1,1, AnimationManager.cpp:39) through
        // hyprutils: equal up to float rounding of its baked table.
        const auto lin   = warp::nodes(eShape::CURVE, from, to, now, SLOW, W, 2.F, bezier(0.F, 0.F, 1.F, 1.F));
        float      worst = 0.F;
        for (size_t i = 0; i < std::min(lin.size(), plain.size()); ++i)
            worst = std::max(worst, std::hypot(lin[i].posPx.x - plain[i].posPx.x, lin[i].posPx.y - plain[i].posPx.y));
        CHECK(lin.size() == plain.size() && worst < 1e-3F);
    }

    // The curve is the quadratic Bezier through p1 = p0 + chord/2 along the
    // velocity: at t = 0.5 it is (p0 + 2 p1 + p2) / 4. From
    // rest p1 is the chord's midpoint, so the path is the straight line,
    // evenly spaced. Chord 1000 at min_spacing 2: 500 nodes, node 250 is
    // t = 0.5.
    {
        const SVec2f target{900.F, 800.F};
        const auto   curve = warp::nodes(eShape::CURVE, from, target, now, SLOW, W, 2.F, {});
        CHECK(curve.size() == 500 && near(curve[249].posPx, {550.F, 350.F}, 1e-2F)); // p1 = (600, 200)

        SCursorNode still = from;
        still.velocity    = {0.F, 0.F};
        const auto line   = warp::nodes(eShape::CURVE, still, target, now, SLOW, W, 2.F, {});
        CHECK(line.size() == 500 && near(line[249].posPx, {500.F, 500.F}, 1e-2F));
        CHECK(std::abs(gap(line, 1) - gap(line, line.size() - 1)) < 1e-2F && std::abs(gap(line, 1) - 2.F) < 1e-2F);
    }

    // Ease in/out (CSS ease-in-out, 0.42,0 / 0.58,1): progress never goes
    // back, nodes bunch at both ends and spread in the middle, and birth
    // times stay the real-time ones (only positions are eased).
    {
        const auto ease = bezier(0.42F, 0.F, 0.58F, 1.F);
        const auto line = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, ease);
        const auto lin  = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, identity);
        bool       mono = true;
        for (size_t i = 1; i < line.size(); ++i)
            mono = mono && along(line[i].posPx, from.posPx, to) >= along(line[i - 1].posPx, from.posPx, to);
        CHECK(mono && allFinite(line));
        CHECK(line.size() > 8);
        const size_t mid = line.size() / 2;
        CHECK(gap(line, 1) < gap(line, mid) && gap(line, line.size() - 1) < gap(line, mid));
        bool sameBirths = line.size() == lin.size();
        for (size_t i = 0; sameBirths && i < line.size(); ++i)
            sameBirths = line[i].birthMs == lin[i].birthMs;
        CHECK(sameBirths);

        // Curve path, starting perpendicular to the chord: x = s^2 * 800 is
        // monotonic in the Bezier parameter, so it has to be in time too.
        const SCursorNode up{.posPx = {0.F, 0.F}, .birthTimeMs = 0.0, .velocity = {0.F, 1.F}, .distPx = 0.0, .seed = 1, .segmentStart = false};
        const auto        curve = warp::nodes(eShape::CURVE, up, {800.F, 0.F}, 50.0, SLOW, W, 4.F, ease);
        bool              monoX = true;
        for (size_t i = 1; i < curve.size(); ++i)
            monoX = monoX && curve[i].posPx.x >= curve[i - 1].posPx.x;
        CHECK(monoX && allFinite(curve) && curve.back().posPx == (SVec2f{800.F, 0.F}));
    }

    // Ease out (0,0 / 0.2,1): fast start, slow arrival, so the spacing
    // shrinks toward the target, and so does the speed the ring records
    // (ht_vSpeed), while ages stay real time.
    {
        const auto line = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, bezier(0.F, 0.F, 0.2F, 1.F));
        CHECK(gap(line, 1) > gap(line, line.size() - 1));
        CTrailRing ring(4096, 3);
        ring.insert(from.posPx, from.birthTimeMs, false);
        for (const auto& n : line)
            ring.insert(n.posPx, n.birthMs, false);
        std::vector<SGpuNode> out;
        ring.orderedCopy(out, line.back().birthMs);
        const auto speed = [](const SGpuNode& g) { return std::hypot(g.velocity.x, g.velocity.y); };
        CHECK(out.size() == line.size() + 1 && speed(out[1]) > speed(out.back()) && out.back().birthMs == 0.F);
    }

    // Overshoot: kept on both shapes, landing still exact. easeOutBack
    // (0.34,1.56 / 0.64,1) passes the target, easeInBack (0.36,0 /
    // 0.66,-0.56) backs up behind the start first.
    {
        const auto outBack = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, bezier(0.34F, 1.56F, 0.64F, 1.F));
        const auto inBack  = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, bezier(0.36F, 0.F, 0.66F, -0.56F));
        float      hi = 0.F, lo = 1.F;
        for (const auto& n : outBack)
            hi = std::max(hi, along(n.posPx, from.posPx, to));
        for (const auto& n : inBack)
            lo = std::min(lo, along(n.posPx, from.posPx, to));
        CHECK(hi > 1.05F && lo < -0.05F);
        CHECK(allFinite(outBack) && allFinite(inBack) && outBack.back().posPx == to && inBack.back().posPx == to);
        CHECK(warp::nodes(eShape::CURVE, from, to, now, SLOW, W, 2.F, bezier(0.34F, 1.56F, 0.64F, 1.F)).back().posPx == to);

        // Outside 0..1 the curve continues along its end tangents: control
        // point p1 = p0 + chord/2 along the velocity (+x here).
        const float  chord = std::hypot(to.x - from.posPx.x, to.y - from.posPx.y);
        const SVec2f p1{from.posPx.x + chord * 0.5F, from.posPx.y};
        const auto   past   = warp::nodes(eShape::CURVE, from, to, now, SLOW, W, 2.F, [](float) { return 1.25F; });
        const auto   behind = warp::nodes(eShape::CURVE, from, to, now, SLOW, W, 2.F, [](float) { return -0.5F; });
        CHECK(near(past.front().posPx, {to.x + (to.x - p1.x) * 0.5F, to.y + (to.y - p1.y) * 0.5F}, 0.01F));
        CHECK(near(behind.front().posPx, {from.posPx.x - (p1.x - from.posPx.x), from.posPx.y - (p1.y - from.posPx.y)}, 0.01F));
        const auto pastLine = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, [](float) { return 1.25F; });
        CHECK(near(pastLine.front().posPx, {from.posPx.x + (to.x - from.posPx.x) * 1.25F, from.posPx.y + (to.y - from.posPx.y) * 1.25F}, 0.01F));
    }

    // Exact landing and no NaN whatever the curve returns. Non-finite
    // results fall back to linear for that node; huge ones stop at 4 path
    // lengths past the target.
    {
        const float inf  = std::numeric_limits<float>::infinity();
        const float qnan = std::numeric_limits<float>::quiet_NaN();
        const auto  lin  = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, {});
        for (const auto shape : {eShape::LINE, eShape::CURVE}) {
            for (const float v : {qnan, inf, -inf, 1e30F, -1e30F, 0.F, 1.F}) {
                const auto got = warp::nodes(shape, from, to, now, SLOW, W, 2.F, [v](float) { return v; });
                CHECK(allFinite(got) && !got.empty() && got.back().posPx == to);
            }
        }
        CHECK(sameNodes(warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, [qnan](float) { return qnan; }), lin));
        const auto huge = warp::nodes(eShape::LINE, from, to, now, SLOW, W, 2.F, [](float) { return 1e30F; });
        CHECK(near(huge.front().posPx, {from.posPx.x + (to.x - from.posPx.x) * 5.F, from.posPx.y + (to.y - from.posPx.y) * 5.F}, 0.1F));
    }

    // Zero duration (warp_max_ms 0, below the setting's range), now or
    // with a clock that went backwards: every birth is the newest node's,
    // positions are still eased, and the ring computes no velocity across
    // dt = 0.
    {
        for (const double at : {from.birthTimeMs, from.birthTimeMs - 5.0}) {
            const auto got = warp::nodes(eShape::CURVE, from, to, at, SLOW, 0.0, 2.F, bezier(0.F, 0.F, 0.2F, 1.F));
            CHECK(allFinite(got) && got.back().posPx == to);
            CHECK(std::ranges::all_of(got, [&](const auto& n) { return n.birthMs == from.birthTimeMs; }));
            CTrailRing ring(4096, 5);
            ring.insert(from.posPx, from.birthTimeMs, false);
            for (const auto& n : got)
                ring.insert(n.posPx, n.birthMs, false);
            std::vector<SGpuNode> out;
            ring.orderedCopy(out, from.birthTimeMs);
            CHECK(std::ranges::all_of(out, [](const auto& g) { return g.velocity.x == 0.F && g.velocity.y == 0.F && std::isfinite(g.distPx); }));
        }
    }

    // Node count: length / max_spacing rounded up, at least 1 (zero chord),
    // whatever the capacity: a 1500 px warp at max_spacing 8 is 188 nodes,
    // under 8 px apart.
    CHECK(warp::nodes(eShape::LINE, from, from.posPx, now, SLOW, W, 2.F, identity).size() == 1);
    CHECK(warp::nodes(eShape::LINE, from, {500.F, 200.F}, now, SLOW, W, 10.F, identity).size() == 40);
    {
        const auto got = warp::nodes(eShape::LINE, from, {from.posPx.x + 1500.F, from.posPx.y}, now, SLOW, W, 8.F, {});
        CHECK(got.size() == 188 && std::abs(gap(got, 1) - 1500.F / 188.F) < 1e-3F);
    }

    // Duration: chord / warp_speed, at most warp_max_ms; a speed that isn't
    // positive takes all of warp_max_ms.
    CHECK(warp::durationMs(400.F, 4.0, 250.0) == 100.0 && warp::durationMs(4000.F, 4.0, 250.0) == 250.0);
    CHECK(warp::durationMs(400.F, 0.0, 250.0) == 250.0 && warp::durationMs(400.F, 4.0, -1.0) == 0.0);

    // Birth times: the window is the next W ms, (now, now +
    // W], evenly, whenever the pointer last moved: a warp right after motion
    // (newest node born now) takes as long to draw as one after a long rest.
    // Births never decrease and never precede the newest node; the last is
    // now + W exactly; landing stays exact.
    {
        const auto births = [&](double idleMs, double maxMs) {
            SCursorNode f = from;
            f.birthTimeMs = now - idleMs;
            return warp::nodes(eShape::CURVE, f, to, now, SLOW, maxMs, 2.F, bezier(0.34F, 1.56F, 0.64F, 1.F));
        };
        const size_t n    = births(0.0, W).size();
        bool         same = n > 1;
        for (const double idle : {0.0, 0.25, 50.0, 119.9, 120.0, 500.0, 5000.0, 1e7}) {
            const auto got = births(idle, W);
            bool       ok  = got.size() == n && got.back().birthMs == now + W && got.back().posPx == to && allFinite(got);
            for (size_t i = 0; i < got.size(); ++i) {
                ok                  = ok && got[i].birthMs > now && got[i].birthMs <= now + W && (i == 0 || got[i].birthMs >= got[i - 1].birthMs);
                const double expect = now + W * static_cast<double>(static_cast<float>(i + 1) / static_cast<float>(n));
                same                = same && std::abs(got[i].birthMs - expect) < 1e-6;
            }
            CHECK(ok);
        }
        CHECK(same);

        // A clock that went backwards (newest node born after now): the
        // window starts at that node instead.
        const auto ahead = births(-30.0, W);
        CHECK(ahead.front().birthMs > now + 30.0 && ahead.back().birthMs == now + 30.0 + W);

        // Duration minimum (warp_max_ms 1, the setting's lower bound): births
        // still spread, over the next ms. Below it (0, negative: not
        // reachable through the setting) they collapse to now.
        const auto minimum = births(5000.0, 1.0);
        CHECK(minimum.front().birthMs > now && minimum.front().birthMs < now + 1.0 && minimum.back().birthMs == now + 1.0);
        CHECK(std::ranges::all_of(births(5000.0, 0.0), [&](const auto& b) { return b.birthMs == now; }));
        CHECK(std::ranges::all_of(births(5000.0, -10.0), [&](const auto& b) { return b.birthMs == now; }));
    }

    // After a rest longer than a layer's fade, every warp node is visible
    // once the warp has drawn, for any fade that covers its duration.
    {
        const double rest = 20000.0;
        CTrailRing   ring(4096, 21);
        ring.insert(from.posPx, now - rest, false);
        const auto got = warp::nodes(eShape::LINE, ring.newest(), to, now, SLOW, W, 2.F, {});
        for (const auto& b : got)
            ring.insert(b.posPx, b.birthMs, false);
        CHECK(got.size() > 16 && ring.visibleCount(now + W, 500.0) == got.size());
    }

    // A warp starting before the previous one's fade is over: it starts where
    // the previous one landed (no jump, even after an overshoot), births never
    // go backwards across both, every velocity is finite, and it lands on its
    // own target. Nothing is carried between warps but the ring itself.
    {
        CTrailRing ring(4096, 9);
        ring.insert(from.posPx, from.birthTimeMs, false);
        const auto back = bezier(0.34F, 1.56F, 0.64F, 1.F);
        for (const auto& n : warp::nodes(eShape::CURVE, ring.newest(), to, now, SLOW, W, 2.F, back))
            ring.insert(n.posPx, n.birthMs, false);
        CHECK(ring.newest().posPx == to);

        const SCursorNode landed = ring.newest();
        const SVec2f      to2{300.F, 900.F};
        const auto        second = warp::nodes(eShape::CURVE, landed, to2, now + 5.0, SLOW, W, 2.F, back);
        CHECK(!second.empty() && second.front().birthMs > landed.birthTimeMs);
        for (const auto& n : second)
            ring.insert(n.posPx, n.birthMs, false);
        CHECK(ring.newest().posPx == to2);

        std::vector<SGpuNode> out;
        ring.orderedCopy(out, now + 5.0);
        bool ordered = true, finite = true;
        for (size_t i = 0; i < out.size(); ++i) {
            ordered = ordered && (i == 0 || out[i].birthMs >= out[i - 1].birthMs);
            finite  = finite && std::isfinite(out[i].velocity.x) && std::isfinite(out[i].velocity.y) && std::isfinite(out[i].distPx);
        }
        CHECK(ordered && finite);

        // No jump: the second warp's first node is near where the first one
        // landed (within 5% of its path, the ease's first step), not anywhere
        // else.
        const float chord2 = std::hypot(to2.x - to.x, to2.y - to.y);
        CHECK(std::hypot(second.front().posPx.x - to.x, second.front().posPx.y - to.y) < 0.05F * chord2);
    }

    // Emit offset: the warp's target is the emit point
    // (cfg::emitPoint, rounded to float as main.cpp does), the same point
    // the next sample computes. The last node is that point exactly, so the
    // next sample's spacing gate (sampleInserts) adds nothing. A raw-pointer
    // target is the control: there the sample appends a segment as long as
    // the offset. Both sources, both shapes, with and without an
    // overshooting curve.
    {
        const cfg::SPair ptr{1600.0, 900.0}; // post-warp pointer
        const auto       e = cfg::emitPoint(ptr, std::nullopt, std::nullopt, {12.0, -20.0});
        const SVec2f     raw{static_cast<float>(ptr.x), static_cast<float>(ptr.y)};
        const SVec2f     emit{static_cast<float>(e.x), static_cast<float>(e.y)};
        const float      minSpacing = 2.F;

        for (const auto* kind : {"pointer", "spring"}) {
            for (const auto shape : {eShape::LINE, eShape::CURVE}) {
                for (const auto& ease : {warp::FEase{}, bezier(0.34F, 1.56F, 0.64F, 1.F)}) {
                    for (const bool toEmit : {true, false}) {
                        const auto src = source::make(kind, 512, 11);
                        src->insert(from.posPx, from.birthTimeMs, false); // the trail's end, itself a sampled emit point
                        const SVec2f target = toEmit ? emit : raw;
                        const auto   got    = warp::nodes(shape, src->newest(), target, now, SLOW, W, minSpacing, ease);
                        for (const auto& n : got)
                            src->insert(n.posPx, n.birthMs, false);
                        CHECK(allFinite(got) && got.back().posPx == target && src->newest().posPx == target);
                        CHECK(sampleInserts(*src, emit, minSpacing) == !toEmit);
                    }
                }
            }
        }
    }
}

// Warp playback (WarpPath.* CPlayback, sample, start): what the plugin does
// per warp and per render, with the pointer history as the source and
// renders simulated at a fixed rate.
static void testWarpPlayback() {
    using warp::eShape;
    const SVec2f     origin{100.F, 200.F}, to{900.F, 200.F};
    const SVec2f     offset{-12.F, 20.F}; // raw pointer minus emit point
    constexpr double W     = 120.0;       // warp_max_ms; SLOW makes every warp take all of it
    constexpr double SLOW  = 1e-6;
    constexpr float  MIN   = 2.F;
    constexpr float  MAXS  = 8.F;
    constexpr double FRAME = 1000.0 / 60.0;

    // A ring whose newest node is `origin`, born `idleMs` before `now`.
    const auto ringAt = [&](double now, double idleMs) {
        auto ring = std::make_unique<CTrailRing>(4096, 31);
        ring->insert({origin.x - 10.F, origin.y}, now - idleMs - 5.0, false);
        ring->insert(origin, now - idleMs, false);
        return ring;
    };
    const auto monotonic = [](const ISource& src, double refMs) {
        std::vector<SGpuNode> out;
        src.orderedCopy(out, refMs);
        bool ok = true;
        for (size_t i = 1; i < out.size(); ++i)
            ok = ok && out[i].birthMs >= out[i - 1].birthMs;
        return ok;
    };

    // Drawn from origin to target over the warp's duration, the same after a
    // rest and right after motion: per render, only nodes already born are
    // in the source, the source's newest node is the playback head, nothing
    // lands on the target before its time (spacing gate: the sample point is
    // the target), and the pending area (bounds) holds the head and the
    // target until the queue is empty. Anchor rule: while queued, the anchor
    // is the head carried into pointer space, never the pointer itself.
    for (const double idle : {5000.0, 0.0}) {
        const double    now  = 1000.0;
        auto            ring = ringAt(now, idle);
        warp::CPlayback pb;
        bool            brk = false;
        warp::start(*ring, pb, eShape::LINE, to, offset, now, SLOW, W, MIN, MAXS, {});
        const size_t queued = pb.pending();
        CHECK(queued == 100 && pb.head() == origin && pb.target() == to && pb.endMs() == now + W);

        bool   ok = true, anchored = true, gated = true, pending = true;
        double t       = now;
        size_t renders = 0;
        while (!pb.empty() && renders < 1000) {
            t += FRAME;
            ++renders;
            warp::sample(*ring, pb, to, t - FRAME, t, MIN, MAXS, brk);
            ok = ok && ring->newest().birthTimeMs <= t && ring->newest().posPx == pb.head();
            if (!pb.empty()) {
                const auto a = pb.anchor();
                const auto b = pb.bounds();
                anchored     = anchored && a && near(*a, {pb.head().x + offset.x, pb.head().y + offset.y});
                gated        = gated && !(ring->newest().posPx == to);
                pending      = pending && b && b->x1 <= pb.head().x && b->x2 >= to.x;
            }
        }
        // 120 ms at 60 Hz: the last node comes due at the 8th render.
        CHECK(ok && anchored && gated && pending && renders == 8 && t >= now + W && t - FRAME < now + W);
        CHECK(ring->newest().posPx == to && ring->newest().birthTimeMs == now + W && !pb.anchor() && !pb.bounds());
        CHECK(ring->size() == queued + 2 && monotonic(*ring, t));
    }

    // Real motion mid-playback (the pointer at least min_spacing from the
    // target) flushes the rest, born now, then the motion goes in as usual
    // (filled every max_spacing). A jitter below min_spacing doesn't: the
    // gate holds.
    {
        const double    now  = 1000.0;
        auto            ring = ringAt(now, 5000.0);
        warp::CPlayback pb;
        bool            brk = false;
        warp::start(*ring, pb, eShape::LINE, to, offset, now, SLOW, W, MIN, MAXS, {});
        warp::sample(*ring, pb, to, now, now + 30.0, MIN, MAXS, brk);
        const size_t drawn = ring->size();
        warp::sample(*ring, pb, {to.x + 1.F, to.y}, now + 30.0, now + 40.0, MIN, MAXS, brk);
        CHECK(!pb.empty() && ring->size() > drawn && !(ring->newest().posPx == to)); // only what came due
        const SVec2f moved{to.x + 30.F, to.y + 5.F};                                 // 30.4 px: filled at 8, 16, 24, then itself
        warp::sample(*ring, pb, moved, now + 40.0, now + 50.0, MIN, MAXS, brk);
        CHECK(pb.empty() && ring->size() == 102 + 4 && ring->newest().posPx == moved && ring->newest().birthTimeMs == now + 50.0);
        std::vector<SGpuNode> out; // oldest first, births relative to now + 50
        ring->orderedCopy(out, now + 50.0);
        bool flushedNow = out.size() == 106;
        for (size_t i = out.size() - 10; flushedNow && i < out.size(); ++i)
            flushedNow = out[i].birthMs == 0.F;
        CHECK(flushedNow && out[out.size() - 5].posPx == to && monotonic(*ring, now + 50.0));
    }

    // A break mid-playback drops the rest: none of it is inserted, and the
    // next sample starts a new segment at the pointer.
    {
        const double    now  = 1000.0;
        auto            ring = ringAt(now, 5000.0);
        warp::CPlayback pb;
        bool            brk = false;
        warp::start(*ring, pb, eShape::LINE, to, offset, now, SLOW, W, MIN, MAXS, {});
        warp::sample(*ring, pb, to, now, now + 30.0, MIN, MAXS, brk);
        const size_t drawn = ring->size();
        brk                = true;
        const SVec2f elsewhere{50.F, 50.F};
        warp::sample(*ring, pb, elsewhere, now + 30.0, now + 40.0, MIN, MAXS, brk);
        CHECK(pb.empty() && !brk && ring->size() == drawn + 1 && ring->newest().segmentStart && ring->newest().posPx == elsewhere);
    }

    // A new warp mid-playback: the rest of the first is flushed (born now,
    // ending on its target), and the second is queued from there, taking
    // its full duration from now.
    {
        const double    now  = 1000.0;
        auto            ring = ringAt(now, 5000.0);
        warp::CPlayback pb;
        bool            brk = false;
        warp::start(*ring, pb, eShape::LINE, to, offset, now, SLOW, W, MIN, MAXS, {});
        warp::sample(*ring, pb, to, now, now + 30.0, MIN, MAXS, brk);
        const SVec2f to2{900.F, 600.F};
        warp::start(*ring, pb, eShape::LINE, to2, offset, now + 40.0, SLOW, W, MIN, MAXS, {});
        CHECK(ring->newest().posPx == to && ring->newest().birthTimeMs == now + 40.0 && ring->size() == 102);
        CHECK(pb.head() == to && pb.target() == to2 && pb.pending() == 50 && pb.endMs() == now + 40.0 + W);
    }

    // CPlayback alone: drain() inserts what is born, oldest first, and moves
    // the head; flush() caps births at now; clear() drops everything.
    {
        CTrailRing ring(64, 3);
        ring.insert({0.F, 0.F}, 0.0, false);
        warp::CPlayback pb;
        CHECK(pb.empty() && !pb.anchor() && !pb.bounds());
        pb.start({{{8.F, 0.F}, 10.0}, {{16.F, 0.F}, 20.0}, {{24.F, 0.F}, 30.0}}, {0.F, 0.F}, 0.0, {0.5F, 0.F});
        pb.drain(ring, 5.0, MIN, MAXS);
        const auto anchor = pb.anchor();
        CHECK(ring.size() == 1 && pb.head() == (SVec2f{0.F, 0.F}) && anchor && near(*anchor, {0.5F, 0.F}));
        pb.drain(ring, 20.0, MIN, MAXS);
        CHECK(ring.size() == 3 && ring.newest().posPx == (SVec2f{16.F, 0.F}) && pb.head() == (SVec2f{16.F, 0.F}) && pb.pending() == 1);
        pb.flush(ring, 25.0, MIN, MAXS);
        CHECK(ring.size() == 4 && ring.newest().birthTimeMs == 25.0 && pb.empty() && pb.head() == (SVec2f{24.F, 0.F}));
        pb.start({{{1.F, 0.F}, 10.0}}, {0.F, 0.F}, 0.0, {});
        pb.clear();
        pb.flush(ring, 0.0, MIN, MAXS);
        CHECK(pb.empty() && ring.size() == 4);
    }

    // The spring chain queues nothing: it moves across the jump itself.
    {
        auto src = source::make("spring", 64, 7);
        src->insert(origin, 1000.0, false);
        warp::CPlayback pb;
        warp::start(*src, pb, eShape::CURVE, to, offset, 1000.0, SLOW, W, MIN, MAXS, {});
        CHECK(pb.empty() && !pb.anchor());
    }

    // Warps as motion: a 200 px and a 2000 px warp at warp_speed 4 (well
    // under warp_max_ms) draw at the same average speed, chord / duration,
    // with the same node spacing, max_spacing. Played back at 144 Hz.
    {
        const auto play = [&](float chord) {
            CTrailRing ring(4096, 13);
            ring.insert(origin, 0.0, false);
            warp::CPlayback pb;
            bool            brk = false;
            const SVec2f    target{origin.x + chord, origin.y};
            warp::start(ring, pb, eShape::LINE, target, {}, 1000.0, 4.0, 5000.0, MIN, MAXS, {});
            const double end = pb.endMs();
            for (int i = 0; !pb.empty(); ++i) {
                const double t = 1000.0 + (i * 1000.0 / 144.0);
                warp::sample(ring, pb, target, t - (1000.0 / 144.0), t, MIN, MAXS, brk);
            }
            std::vector<SGpuNode> out;
            ring.orderedCopy(out, 0.0);
            float lo = 1e9F, hi = 0.F;
            for (size_t i = 1; i < out.size(); ++i) {
                const float g = std::hypot(out[i].posPx.x - out[i - 1].posPx.x, out[i].posPx.y - out[i - 1].posPx.y);
                lo            = std::min(lo, g);
                hi            = std::max(hi, g);
            }
            return std::tuple{chord / (end - 1000.0), lo, hi, out.size()};
        };
        const auto [speedShort, loShort, hiShort, nShort] = play(200.F);
        const auto [speedLong, loLong, hiLong, nLong]     = play(2000.F);
        CHECK(std::abs(speedShort - 4.0) < 1e-9 && std::abs(speedLong - 4.0) < 1e-9);
        CHECK(nShort == 26 && nLong == 251 && std::abs(loShort - MAXS) < 1e-3F && std::abs(hiShort - MAXS) < 1e-3F && std::abs(loLong - MAXS) < 1e-3F &&
              std::abs(hiLong - MAXS) < 1e-3F);
    }
}

// Real motion through the emission rule (emitTo, via warp::sample with no
// warp playing): the pointer at a steady speed along a line and around a
// circle, sampled at 30, 60 and 144 Hz. Consecutive nodes are never more
// than max_spacing apart, births never decrease and stay within each
// step's sample interval, and at a fast speed (3 px/ms: 100 px per frame at
// 30 Hz, 21 at 144) the node density barely moves with the rate (within
// 15%; before, one node per frame made it 4.8x).
static void testEmitSpacing() {
    constexpr float MIN  = 2.F;
    constexpr float MAXS = 8.F;
    const auto      run  = [&](double hz, double speed, bool circle) {
        CTrailRing      ring(4096, 17);
        warp::CPlayback pb;
        bool            brk   = false;
        const double    frame = 1000.0 / hz;
        const auto      at    = [&](double t) {
            const double d = speed * t;
            if (!circle)
                return SVec2f{static_cast<float>(d), 0.F};
            const double r = 150.0, a = d / r;
            return SVec2f{static_cast<float>(r * std::cos(a)), static_cast<float>(r * std::sin(a))};
        };
        bool inTime = true;
        for (int i = 0; i * frame <= 1000.0; ++i) {
            const double t      = i * frame;
            const auto   before = ring.size();
            warp::sample(ring, pb, at(t), t - frame, t, MIN, MAXS, brk);
            if (ring.size() > before)
                inTime = inTime && ring.newest().birthTimeMs <= t;
        }
        std::vector<SGpuNode> out;
        ring.orderedCopy(out, 0.0);
        float  worst = 0.F;
        double path  = 0.0;
        bool   mono  = true;
        for (size_t i = 1; i < out.size(); ++i) {
            const float g = std::hypot(out[i].posPx.x - out[i - 1].posPx.x, out[i].posPx.y - out[i - 1].posPx.y);
            worst         = std::max(worst, g);
            path += g;
            mono = mono && out[i].birthMs >= out[i - 1].birthMs;
        }
        return std::tuple{worst, path / static_cast<double>(out.size() - 1), mono && inTime};
    };
    for (const bool circle : {false, true}) {
        for (const double speed : {0.1, 1.0, 3.0, 10.0}) {
            double lo = 1e9, hi = 0.0;
            for (const double hz : {30.0, 60.0, 144.0}) {
                const auto [worst, mean, ok] = run(hz, speed, circle);
                CHECK(worst <= MAXS + 1e-3F && ok);
                lo = std::min(lo, mean);
                hi = std::max(hi, mean);
            }
            if (speed >= 3.0)
                CHECK(hi / lo < 1.15);
        }
    }

    // Births interpolate between the two samples: a 40 px step sampled at
    // t = 100 after one at t = 60 fills 8, 16, 24, 32 born 68, 76, 84, 92.
    {
        CTrailRing ring(64, 2);
        ring.insert({0.F, 0.F}, 0.0, false);
        CHECK(emitTo(ring, {40.F, 0.F}, 60.0, 100.0, MIN, MAXS, false));
        std::vector<SGpuNode> out;
        ring.orderedCopy(out, 0.0);
        CHECK(out.size() == 6 && out[1].birthMs == 68.F && out[4].birthMs == 92.F && out[5].birthMs == 100.F && out[4].posPx == (SVec2f{32.F, 0.F}));

        // Under min_spacing nothing, unless landing; a break fills nothing;
        // a step longer than the capacity keeps only the last capacity
        // points' worth.
        CHECK(!emitTo(ring, {41.F, 0.F}, 100.0, 110.0, MIN, MAXS, false) && emitTo(ring, {41.F, 0.F}, 100.0, 110.0, MIN, MAXS, false, true));
        const auto n = ring.size();
        CHECK(emitTo(ring, {141.F, 0.F}, 110.0, 120.0, MIN, MAXS, true) && ring.size() == n + 1 && ring.newest().segmentStart);
        CTrailRing small(4, 2);
        small.insert({0.F, 0.F}, 0.0, false);
        CHECK(emitTo(small, {800.F, 0.F}, 0.0, 10.0, MIN, MAXS, false) && small.newest().posPx == (SVec2f{800.F, 0.F}) && small.size() == 4);
    }
}

// A source that changes between inserts: stands in for a future animated one
// to exercise the continuous-upload half of the gate.
namespace {
    struct SContinuousStub final : ISource {
        std::string_view kind() const override {
            return "stub";
        }
        void insert(const SVec2f&, double, bool) override {}
        void tick(double, double) override {}
        bool warp(const SVec2f&, double) override {
            return false;
        }
        void   configure(const std::map<std::string, double>&) override {}
        void   resize(size_t) override {}
        void   clear() override {}
        void   expire(double, double) override {}
        size_t size() const override {
            return 0;
        }
        size_t capacity() const override {
            return 0;
        }
        const SCursorNode& newest() const override {
            return node_;
        }
        void orderedCopy(std::vector<SGpuNode>&, double) const override {}
        bool needsContinuousUpload() const override {
            return true;
        }
        size_t visibleCount(double, double) const override {
            return 0;
        }
        std::optional<STrailBounds> visibleBounds(double, double, bool) const override {
            return std::nullopt;
        }
        bool isSettled(double, double) const override {
            return false;
        }
        uint64_t generation() const override {
            return 7;
        }
        bool empty() const override {
            return empty_;
        }
        double newestBirthMs() const override {
            return 0.0;
        }
        bool        empty_ = false;
        SCursorNode node_{};
    };
}

// Pointer retention by age (CTrailRing::expire, RING_CEILING): a 3 s drag at
// 10 px/ms sampled at 144 Hz with a 1450 ms window, one expire per frame as
// sampleSource does. Every node younger than the window is still there, in
// one connected run, with exactly one older node behind it (a path layer
// draws the segment to it); the buffer stays at the ceiling's size
// throughout, so the GPU buffer (CNodeBuffer::ensure, sized from
// capacity()) is never reallocated. Idle leaves only the newest node, which
// the next motion continues from. At the ceiling the oldest is overwritten
// and counted.
static void testRetention() {
    constexpr float  MIN = 2.F, MAXS = 8.F;
    constexpr double KEEP = 1450.0, FRAME = 1000.0 / 144.0;

    // The pointer source as the plugin builds it: RING_CEILING nodes.
    std::string problems;
    const auto  src = source::make("pointer", source::resolve("pointer", {}, {}, problems).capacity, 3);
    CHECK(src->capacity() == RING_CEILING && problems.empty());
    auto&      ring = dynamic_cast<CTrailRing&>(*src);

    const auto drag = [](CTrailRing& r, double t0, double t1, double speed, float maxS, double keep, bool& capacityFixed) {
        double prev = t0;
        for (int64_t i = 1; t0 + (static_cast<double>(i) * FRAME) <= t1; ++i) {
            const double t = t0 + (static_cast<double>(i) * FRAME);
            r.expire(t, keep);
            // Around a 400 px circle, so the drag stays on screen.
            const double a = speed * (t - t0) / 400.0;
            emitTo(r, {static_cast<float>(400.0 * std::cos(a)), static_cast<float>(400.0 * std::sin(a))}, prev, t, MIN, maxS, false);
            capacityFixed = capacityFixed && r.capacity() == RING_CEILING;
            prev          = t;
        }
        return prev;
    };

    bool                  fixed = true;
    const double          end   = drag(ring, 0.0, 3000.0, 10.0, MAXS, KEEP, fixed);
    std::vector<SGpuNode> v;
    ring.orderedCopy(v, end);
    // Ages are -birthMs. Oldest: past the window; the next one: inside it.
    CHECK(fixed && ring.drops() == 0);
    CHECK(v.size() > 1000 && -v[0].birthMs >= KEEP && -v[1].birthMs < KEEP);
    float worst = 0.F;
    bool  run   = true;
    for (size_t i = 1; i < v.size(); ++i) {
        worst = std::max(worst, std::hypot(v[i].posPx.x - v[i - 1].posPx.x, v[i].posPx.y - v[i - 1].posPx.y));
        run   = run && !(v[i].bits & GPU_BIT_SEGMENT_START) && v[i].birthMs >= v[i - 1].birthMs;
    }
    CHECK(run && worst <= MAXS + 1e-3F);

    // Idle: only the newest node is left, faded; nothing draws.
    const SVec2f rest = ring.newest().posPx;
    ring.expire(end + 5000.0, KEEP);
    CHECK(ring.size() == 1 && ring.newest().posPx == rest && ring.isSettled(end + 5000.0, KEEP) && ring.capacity() == RING_CEILING);
    // The next motion continues from it, connected, filled along the step.
    emitTo(ring, {rest.x + 40.F, rest.y}, end + 4990.0, end + 5000.0, MIN, MAXS, false);
    ring.orderedCopy(v, end + 5000.0);
    CHECK(v.size() == 6 && v[0].posPx == rest && !(v[1].bits & GPU_BIT_SEGMENT_START));

    // The ceiling: 100 px/ms with points every 0.5 px is 200 nodes/ms, far
    // more than 4096 in the window. The ring stays full, keeps the newest
    // in order, and counts what it overwrote.
    CTrailRing   full(RING_CEILING, 5);
    bool         fullFixed = true;
    const double fullEnd   = drag(full, 0.0, 200.0, 100.0, 0.5F, KEEP, fullFixed);
    full.orderedCopy(v, fullEnd);
    bool ordered = true;
    for (size_t i = 1; i < v.size(); ++i)
        ordered = ordered && v[i].birthMs >= v[i - 1].birthMs;
    // Newest: the last step's end, within one 0.5 px fill of it in time.
    CHECK(fullFixed && full.size() == RING_CEILING && full.drops() > 0 && ordered && v.back().birthMs > -0.01F);

    // A warp longer than RING_CEILING * max_spacing queues no more nodes
    // than the ring holds.
    const SCursorNode from{.posPx = {0.F, 0.F}, .birthTimeMs = 0.0, .velocity = {}, .distPx = 0.0, .seed = 0, .segmentStart = true};
    CHECK(warp::nodes(warp::eShape::LINE, from, {50000.F, 0.F}, 0.0, 4.0, 250.0, MAXS, {}).size() == RING_CEILING);
}

static void testSource() {
    CTrailRing ring(8, 42);

    // The real-history ring never asks for continuous upload, and is settled
    // exactly when nothing is visible.
    CHECK(!ring.needsContinuousUpload());
    CHECK(ring.isSettled(0.0, 100.0)); // empty
    ring.tick(10.0, 10.0);             // no-op
    CHECK(ring.empty() && ring.generation() == 0);

    ring.insert({0, 0}, 0.0, false);
    ring.insert({3, 4}, 10.0, false);
    ring.insert({6, 8}, 20.0, false);
    CHECK(!ring.needsContinuousUpload());
    CHECK(ring.newestBirthMs() == 20.0);
    for (const double now : {0.0, 20.0, 25.0, 45.0, 1000.0}) {
        for (const double fade : {1.0, 10.0, 30.0, 100.0})
            CHECK(ring.isSettled(now, fade) == (ring.visibleCount(now, fade) == 0));
    }
    CHECK(!ring.isSettled(25.0, 30.0) && ring.isSettled(1000.0, 30.0));

    // Upload gate for the ring: upload unless empty or the generation is
    // unchanged.
    CTrailRing fresh(4, 1);
    CHECK(!sourceNeedsUpload(fresh, UINT64_MAX));       // empty: nothing to upload
    CHECK(sourceNeedsUpload(ring, UINT64_MAX));         // never uploaded
    CHECK(!sourceNeedsUpload(ring, ring.generation())); // up to date
    const auto uploaded = ring.generation();
    ring.insert({9, 12}, 30.0, false);
    CHECK(sourceNeedsUpload(ring, uploaded)); // insert
    const auto afterInsert = ring.generation();
    ring.resize(2);
    CHECK(sourceNeedsUpload(ring, afterInsert)); // resize
    const auto afterResize = ring.generation();
    ring.clear();
    CHECK(!sourceNeedsUpload(ring, afterResize)); // clear: empty, nothing to upload

    // A continuous source uploads every frame even at an unchanged
    // generation, but never when empty.
    SContinuousStub stub;
    CHECK(sourceNeedsUpload(stub, stub.generation()));
    stub.empty_ = true;
    CHECK(!sourceNeedsUpload(stub, stub.generation()));
}

// What the renderer does each frame with a source (CNodeBuffer::upload):
// upload if the gate says so, remembering what was uploaded.
namespace {
    struct SUploadSim {
        std::vector<SGpuNode> uploaded;
        uint64_t              gen     = UINT64_MAX;
        double                ref     = 0.0;
        int                   uploads = 0;

        void                  frame(const ISource& s) {
            if (!sourceNeedsUpload(s, gen))
                return;
            ref = s.newestBirthMs();
            s.orderedCopy(uploaded, ref);
            gen = s.generation();
            ++uploads;
        }

        // The GPU holds what the source would upload right now.
        bool current(const ISource& s) const {
            std::vector<SGpuNode> now;
            s.orderedCopy(now, ref);
            return ref == s.newestBirthMs() && now.size() == uploaded.size() && std::memcmp(now.data(), uploaded.data(), now.size() * sizeof(SGpuNode)) == 0;
        }
    };

    std::vector<SGpuNode> copyOf(const ISource& s) {
        std::vector<SGpuNode> out;
        s.orderedCopy(out, s.newestBirthMs());
        return out;
    }
}

static void testSpringChain() {
    // The step's transition (built from advanceSpring) against an
    // independent answer: critically damped at omega 50 rad/s from 100 px
    // away at rest has displacement d0 (1 + wt) e^-wt and velocity
    // -d0 w^2 t e^-wt. The head alone: a single point.
    const auto hz = [](double omega) { return omega / (2.0 * std::numbers::pi); };
    {
        CSpringChainSource c(1, 1);
        c.configure({{"head_freq_hz", hz(50)}, {"head_damping_ratio", 1}});
        CHECK(c.empty() && c.size() == 0 && !c.needsContinuousUpload()); // NOLINT(readability-container-size-empty): asserts size() agrees with empty()
        c.insert({0, 0}, 0.0, false);                                    // seeds the chain
        CHECK(!c.empty() && c.size() == 1 && !c.needsContinuousUpload());
        c.insert({100, 0}, 0.0, false);
        CHECK(c.needsContinuousUpload());
        c.tick(16.0, 16.0);
        const auto out = copyOf(c);
        CHECK(out.size() == 1 && std::abs(out[0].posPx.x - 19.121F) < 0.05F && out[0].posPx.y == 0.F);
        CHECK(std::abs(out[0].velocity.x - 1.797F) < 0.01F); // px/ms
    }

    // dt 0 moves nothing, and a long gap integrates at most MAX_DT (33 ms).
    {
        CSpringChainSource a(4, 1), b(4, 1), c(4, 1);
        for (auto* s : {&a, &b, &c}) {
            s->configure({{"freq_hz", hz(20)}, {"damping_ratio", 1.5}, {"head_freq_hz", hz(20)}, {"head_damping_ratio", 1.5}}); // slow: 33 ms is far from arriving
            s->insert({0, 0}, 0.0, false);
            s->insert({100, 40}, 0.0, false);
        }
        const auto before = copyOf(c);
        c.tick(5.0, 0.0);
        const auto after = copyOf(c);
        CHECK(before.size() == after.size() && std::memcmp(before.data(), after.data(), before.size() * sizeof(SGpuNode)) == 0 && c.needsContinuousUpload());
        a.tick(10000.0, 10000.0);
        b.tick(10000.0, 33.0);
        const auto ca = copyOf(a), cb = copyOf(b);
        CHECK(ca.size() == cb.size() && ca.back().posPx == cb.back().posPx && ca.back().posPx.x > 0.F && ca.back().posPx.x < 90.F);
    }

    // Overdamped: every point trails the one before it and none overshoots.
    {
        CSpringChainSource c(6, 1);
        c.configure({{"freq_hz", 10}, {"damping_ratio", 3}, {"head_freq_hz", 10}, {"head_damping_ratio", 3}});
        c.insert({0, 0}, 0.0, false);
        c.insert({300, 0}, 0.0, false);
        for (int i = 1; i <= 5; ++i)
            c.tick(i * 16.0, 16.0);
        const auto out     = copyOf(c); // tail first
        bool       ordered = out.size() == 6, inside = true;
        for (size_t i = 0; i + 1 < out.size(); ++i)
            ordered = ordered && out[i].posPx.x <= out[i + 1].posPx.x;
        for (const auto& n : out)
            inside = inside && n.posPx.x >= 0.F && n.posPx.x <= 300.F;
        CHECK(ordered && inside && out.back().posPx.x > out.front().posPx.x);
    }

    // Fixed steps: the motion depends on wall-clock time only, not on how it
    // is cut into ticks (60 Hz, 120 Hz, VRR-like uneven frames, a second
    // monitor ticking the same frame again). A bouncy head over a long chain,
    // still moving at the end.
    {
        const auto run = [](const std::vector<double>& ticks) {
            CSpringChainSource c(32, 1);
            c.configure({{"freq_hz", 30}, {"damping_ratio", 0.9}, {"head_freq_hz", 4}, {"head_damping_ratio", 0.3}});
            c.insert({0, 0}, 0.0, false);
            c.insert({300, -120}, 0.0, false);
            double now = 0.0;
            while (now < 400.0)
                for (const double dt : ticks) {
                    now += dt;
                    c.tick(now, dt);
                }
            CHECK(now == 400.0 && c.needsContinuousUpload());
            return copyOf(c);
        };
        const auto at16 = run({16.0}), at8 = run({8.0}), uneven = run({5.5, 10.5, 0.25, 0.75, 7.0, 16.0}), twice = run({16.0, 0.0});
        bool       same = at16.size() == 32;
        for (const auto* other : {&at8, &uneven, &twice})
            for (size_t i = 0; i < at16.size(); ++i)
                same = same && other->size() == at16.size() && (*other)[i].posPx == at16[i].posPx && (*other)[i].velocity == at16[i].velocity;
        CHECK(same && at16.front().posPx != at16.back().posPx);
    }

    // A line or curve warp moves the whole chain by the jump and keeps its
    // velocities; the pointer history leaves the warp to its path nodes.
    {
        CSpringChainSource c(8, 1);
        c.insert({0, 0}, 0.0, false);
        c.insert({200, 0}, 0.0, false);
        c.tick(30.0, 30.0);
        const auto before = copyOf(c);
        const auto gen    = c.generation();
        CHECK(c.warp({700, -300}, 40.0));
        const auto after = copyOf(c);
        bool       moved = after.size() == before.size();
        for (size_t i = 0; i < after.size() && moved; ++i)
            moved = after[i].posPx == SVec2f(before[i].posPx.x + 500.F, before[i].posPx.y - 300.F) && after[i].velocity == before[i].velocity;
        CHECK(moved && c.generation() != gen && c.needsContinuousUpload() && c.newest().posPx == SVec2f(700.F, -300.F) && c.newestBirthMs() == 40.0);

        CTrailRing ring(8, 1);
        ring.insert({0, 0}, 0.0, false);
        CHECK(!ring.warp({10, 10}, 1.0) && ring.size() == 1);
    }

    // A point farther than MAX_REACH_PX from the head re-seeds the chain on
    // the target, at rest. A connected jump that far leaves the tail behind.
    {
        CSpringChainSource c(4, 1);
        c.insert({0, 0}, 0.0, false);
        c.insert({CSpringChainSource::MAX_REACH_PX * 4.F, 0}, 0.0, false);
        c.tick(16.0, 16.0);
        bool seeded = true;
        for (const auto& n : copyOf(c))
            seeded = seeded && n.posPx == SVec2f(CSpringChainSource::MAX_REACH_PX * 4.F, 0.F) && n.velocity == SVec2f{};
        CHECK(seeded && !c.needsContinuousUpload());

        // Within reach it just follows.
        CSpringChainSource d(4, 1);
        d.insert({0, 0}, 0.0, false);
        d.insert({CSpringChainSource::MAX_REACH_PX / 2.F, 0}, 0.0, false);
        d.tick(16.0, 16.0);
        CHECK(d.needsContinuousUpload() && copyOf(d).front().posPx.x < CSpringChainSource::MAX_REACH_PX / 2.F);
    }

    // Moves, settles exactly on the target, and what the renderer uploaded
    // under the upload gate is the final state, however many frames it took.
    {
        CSpringChainSource c(8, 7);
        SUploadSim         sim;
        c.insert({0, 0}, 0.0, false);
        sim.frame(c);
        c.insert({200, 50}, 10.0, false);
        double now    = 10.0;
        int    frames = 0;
        for (; frames < 2000; ++frames) {
            now += 16.0;
            c.tick(now, 16.0);
            sim.frame(c);
            if (!c.needsContinuousUpload())
                break;
        }
        CHECK(frames < 2000);
        CHECK(sim.current(c));
        bool atTarget = true;
        for (const auto& n : copyOf(c))
            atTarget = atTarget && n.posPx == SVec2f{200.F, 50.F} && n.velocity == SVec2f{};
        CHECK(atTarget);

        // Settled and unchanged: no more uploads, no more generations.
        const auto gen     = c.generation();
        const int  uploads = sim.uploads;
        for (int i = 0; i < 10; ++i) {
            now += 16.0;
            c.tick(now, 16.0);
            sim.frame(c);
        }
        CHECK(c.generation() == gen && sim.uploads == uploads && sim.current(c));

        // The fade check: stopped is not settled until it has faded too.
        const double active = c.newestBirthMs();
        CHECK(!c.isSettled(active + 1.0, 100.0));
        CHECK(c.isSettled(active + 101.0, 100.0));
    }

    // Births: point k is (activeMs - k * age_step_ms); the tail is oldest and
    // the only segment start; distances grow from the tail; seeds are fixed.
    {
        CSpringChainSource c(4, 9);
        c.insert({0, 0}, 100.0, false);
        auto out = copyOf(c);
        CHECK(out.size() == 4 && out[3].birthMs == 0.F && out[2].birthMs == -10.F && out[0].birthMs == -30.F);
        CHECK((out[0].bits & GPU_BIT_SEGMENT_START) && !(out[1].bits & GPU_BIT_SEGMENT_START) && !(out[3].bits & GPU_BIT_SEGMENT_START));
        CHECK(c.newestBirthMs() == 100.0 && c.newest().posPx == SVec2f{} && c.newest().birthTimeMs == 100.0);
        CHECK((out[0].bits >> 1) != (out[1].bits >> 1));

        c.insert({30, 40}, 110.0, false);
        for (int i = 1; i <= 3; ++i)
            c.tick(110.0 + i * 16.0, 16.0);
        const auto moved = copyOf(c);
        CHECK(moved[0].distPx == 0.F && moved[0].distPx <= moved[1].distPx && moved[1].distPx <= moved[2].distPx && moved[2].distPx <= moved[3].distPx && moved[3].distPx > 0.F);
        for (size_t i = 0; i < 4; ++i)
            CHECK((moved[i].bits >> 1) == (out[i].bits >> 1));                        // seeds don't change
        CHECK(c.newest().posPx == SVec2f(30.F, 40.F) && c.newest().velocity.x > 0.F); // the last insert, not the head's position

        // Visibility follows the ages: the newest first, here once settled at t = 110.
        CSpringChainSource s(4, 9);
        s.insert({0, 0}, 100.0, false);
        CHECK(s.visibleCount(100.0, 35.0) == 4 && s.visibleCount(100.0, 25.0) == 3 && s.visibleCount(100.0, 5.0) == 1 && s.visibleCount(200.0, 35.0) == 0);
        CHECK(!s.isSettled(100.0, 35.0) && s.isSettled(200.0, 35.0));
        CHECK(!s.visibleBounds(200.0, 35.0) && s.visibleBounds(100.0, 35.0, false));

        // Unsettled is never settled, faded or not.
        s.insert({50, 0}, 100.0, false);
        CHECK(!s.isSettled(10000.0, 35.0));

        // age_step_ms is live; names the source doesn't have are ignored.
        const auto gen = s.generation();
        s.configure({{"bogus", 1.0}});
        CHECK(s.generation() == gen);
        s.configure({{"age_step_ms", 20.0}});
        CHECK(s.generation() != gen && copyOf(s).front().birthMs == -60.F);
    }

    // A break re-seeds the chain where the pointer is now, at rest.
    {
        CSpringChainSource c(5, 3);
        c.insert({0, 0}, 0.0, false);
        c.insert({200, 0}, 5.0, false);
        c.tick(20.0, 15.0);
        const auto gen = c.generation();
        c.insert({500, 600}, 30.0, true);
        CHECK(c.generation() != gen && !c.needsContinuousUpload() && c.newest().segmentStart);
        bool seeded = true;
        for (const auto& n : copyOf(c))
            seeded = seeded && n.posPx == SVec2f{500.F, 600.F} && n.velocity == SVec2f{};
        CHECK(seeded);
    }

    // resize keeps the head end, clear empties, both bump the generation.
    {
        CSpringChainSource c(4, 3);
        c.resize(6);                                            // not seeded yet: just the capacity
        CHECK(c.capacity() == 6 && c.size() == 0 && c.empty()); // NOLINT(readability-container-size-empty): asserts size() agrees with empty()
        c.insert({10, 20}, 0.0, false);
        c.insert({60, 20}, 5.0, false);
        c.tick(20.0, 15.0);
        const auto head = copyOf(c).back().posPx;
        auto       gen  = c.generation();
        c.resize(6);
        CHECK(c.generation() == gen); // no change
        c.resize(3);
        CHECK(c.size() == 3 && c.capacity() == 3 && c.generation() != gen && copyOf(c).back().posPx == head);
        gen = c.generation();
        c.resize(9);
        CHECK(c.size() == 9 && c.generation() != gen && copyOf(c).back().posPx == head);
        gen = c.generation();
        c.clear();
        // Asserts size() agrees with empty().
        // NOLINTNEXTLINE(readability-container-size-empty)
        CHECK(c.empty() && c.size() == 0 && c.capacity() == 9 && c.generation() != gen && !c.needsContinuousUpload());
        c.tick(100.0, 16.0); // nothing to advance
        c.insert({1, 1}, 100.0, false);
        CHECK(c.size() == 9 && c.newest().posPx == SVec2f(1.F, 1.F));
    }

    // The pointer source satisfies the same contract without moving.
    {
        CTrailRing ring(8, 1);
        CHECK(ring.kind() == "pointer" && ring.capacity() == 8 && ring.empty());
        ring.insert({1, 2}, 5.0, false);
        ring.configure({{"freq_hz", 3.0}}); // nothing to configure
        ring.tick(10.0, 5.0);
        CHECK(ring.size() == 1 && ring.newest().posPx == SVec2f(1.F, 2.F) && ring.newestBirthMs() == 5.0 && !ring.needsContinuousUpload());
    }

    // The factory and the settings a source declares.
    {
        CHECK(source::known("pointer") && source::known("spring") && !source::known("rope") && !source::known(""));
        CHECK(!source::decls("pointer").empty() && source::decls("rope").empty());

        const auto sp = source::make("spring", 12, 1);
        const auto pt = source::make("pointer", 12, 1);
        CHECK(sp->kind() == "spring" && sp->capacity() == 12 && pt->kind() == "pointer" && pt->capacity() == 12);

        // Declared defaults, then the manifest's, then the `params` string's.
        // The plugin-applied settings come out typed, the kind's own in
        // `values` for ISource::configure.
        std::string problems;
        auto r = source::resolve("spring", {{"freq_hz", "20"}, {"head_freq_hz", "3"}, {"capacity", "32"}}, {{"freq_hz", "25"}, {"damping_ratio", "0.5"}, {"bogus", "1"}}, problems);
        CHECK(r.values.size() == 5 && r.values["freq_hz"] == 25.0 && r.values["head_freq_hz"] == 3.0 && std::abs(r.values["damping_ratio"] - spring::DAMPING_RATIO) < 1e-6 &&
              r.values["head_damping_ratio"] == r.values["damping_ratio"] && r.values["age_step_ms"] == spring::AGE_STEP_MS && r.capacity == 32);
        CHECK(problems.contains("damping_ratio") && problems.contains("bogus") && !problems.contains("freq_hz"));

        problems.clear();
        const auto springDefaults = source::resolve("spring", {}, {}, problems);
        const auto expected       = source::SSettings{.capacity = source::CAPACITY, .maxSpacingPx = std::numeric_limits<float>::infinity(), .values = springDefaults.values};
        CHECK(springDefaults.values.size() == 5 && springDefaults == expected && problems.empty());

        // Unset head settings follow the chain's, wherever those came from;
        // set ones don't.
        problems.clear();
        auto follow = source::resolve("spring", {{"damping_ratio", "1.2"}}, {{"freq_hz", "30"}}, problems);
        CHECK(problems.empty() && follow.values["head_freq_hz"] == 30.0 && follow.values["head_damping_ratio"] == 1.2);
        follow = source::resolve("spring", {{"head_damping_ratio", "0.3"}}, {{"freq_hz", "30"}, {"head_freq_hz", "4"}}, problems);
        CHECK(problems.empty() && follow.values["head_freq_hz"] == 4.0 && follow.values["head_damping_ratio"] == 0.3 && follow.values["freq_hz"] == 30.0);

        // The cascade's stability edge: chain links from 0.71, the head
        // from 0.2.
        for (const auto& [key, value, ok] : {std::tuple{"damping_ratio", "0.71", true},
                                             {"damping_ratio", "0.7", false},
                                             {"head_damping_ratio", "0.2", true},
                                             {"head_damping_ratio", "0.19", false},
                                             {"freq_hz", "0.4", false},
                                             {"head_freq_hz", "101", false}}) {
            problems.clear();
            source::resolve("spring", {}, {{key, value}}, problems);
            CHECK(problems.contains(key) != ok);
        }
        CHECK(source::resolve("pointer", {}, {{"freq_hz", "1"}}, problems).values.empty() && problems.contains("freq_hz"));

        // The warp keys are pointer's: a spring chain only keeps the target.
        problems.clear();
        source::resolve("spring", {{"warp_speed", "5"}, {"max_spacing", "4"}}, {}, problems);
        CHECK(problems.contains("warp_speed") && problems.contains("max_spacing"));

        // pointer: spacing and warp timing, `params` over the manifest per
        // key; unset keys keep their defaults (linear warp).
        problems.clear();
        const auto ptr = source::resolve("pointer", {{"warp_speed", "2.5"}, {"warp_max_ms", "80"}, {"warp_p1", "0.16,1"}, {"min_spacing", "4"}, {"max_spacing", "6"}},
                                         {{"max_spacing", "12"}}, problems);
        CHECK(problems.empty() && ptr.capacity == RING_CEILING && ptr.warpSpeed == 2.5F && ptr.warpMaxMs == 80.F && ptr.minSpacingPx == 4.F && ptr.maxSpacingPx == 12.F &&
              ptr.warpP1 == SVec2f(0.16F, 1.F) && ptr.warpP2 == SVec2f(1.F, 1.F) && ptr.values.empty());

        // max_spacing below min_spacing counts as min_spacing; a spring
        // chain has none (unbounded).
        CHECK(source::resolve("pointer", {{"min_spacing", "6"}, {"max_spacing", "3"}}, {}, problems).maxSpacingPx == 6.F);
        CHECK(std::isinf(source::resolve("spring", {}, {}, problems).maxSpacingPx));
        CHECK(source::resolve("pointer", {}, {}, problems) == source::SSettings{});

        // Capacity is spring's: outside 2..4096 it is rejected, keeping the
        // earlier value. The pointer history has none (kept by age in
        // RING_CEILING nodes), so the key is unknown there.
        problems.clear();
        CHECK(source::resolve("spring", {{"capacity", "128"}}, {{"capacity", "1"}}, problems).capacity == 128 && problems.contains("capacity"));
        problems.clear();
        CHECK(source::resolve("pointer", {{"capacity", "128"}}, {}, problems).capacity == RING_CEILING && problems.contains("capacity"));

        // Control points: a coordinate outside -1..2 (hl.curve's range) is
        // rejected; x inside it is clamped to 0..1, y kept (overshoot).
        problems.clear();
        const auto cp = source::resolve("pointer", {{"warp_p1", "1.5,-0.5"}, {"warp_p2", "0.3,1"}}, {{"warp_p2", "-0.5,3"}}, problems);
        CHECK(cp.warpP1 == SVec2f(1.F, -0.5F) && cp.warpP2 == SVec2f(0.3F, 1.F) && problems.contains("warp_p2") && !problems.contains("warp_p1"));

        // With x clamped, the baked curve's x never decreases, which
        // getYForPoint's binary search needs. Unclamped (2, 0) and (-1, 1)
        // fold back on themselves.
        {
            problems.clear();
            const auto wide  = source::resolve("pointer", {{"warp_p1", "2,0"}, {"warp_p2", "-1,1"}}, {}, problems);
            const auto bakeX = [](SVec2f p1, SVec2f p2) {
                Hyprutils::Animation::CBezierCurve curve;
                curve.setup({Hyprutils::Math::Vector2D{p1.x, p1.y}, Hyprutils::Math::Vector2D{p2.x, p2.y}});
                std::vector<float> xs;
                for (int i = 0; i <= 64; ++i)
                    xs.push_back(curve.getXForT(static_cast<float>(i) / 64.F));
                return xs;
            };
            CHECK(problems.empty() && std::ranges::is_sorted(bakeX(wide.warpP1, wide.warpP2)) && !std::ranges::is_sorted(bakeX({2.F, 0.F}, {-1.F, 1.F})));
        }

        // A resolved set configures the source it was resolved for.
        CSpringChainSource c(2, 1);
        problems.clear();
        c.configure(source::resolve("spring", {}, {{"age_step_ms", "25"}}, problems).values);
        c.insert({0, 0}, 50.0, false);
        CHECK(problems.empty() && copyOf(c).front().birthMs == -25.F);
    }
}

// emit_from / emit_offset grammar and bounds (ConfigParse.hpp).
static void testEmitConfig() {
    using namespace hyprtail::cfg;

    // Grammar.
    CHECK(parseTwoFloats("0.5 0.5") == SPair({0.5, 0.5}));
    CHECK(parseTwoFloats("  0   1 ") == SPair({0.0, 1.0}));
    CHECK(!parseTwoFloats("hotspot"));
    CHECK(!parseTwoFloats("0.5"));
    CHECK(!parseTwoFloats("0.5 0.5 0.5"));
    CHECK(!parseTwoFloats("0.5 abc"));
    CHECK(!parseTwoFloats("nan 0"));
    CHECK(!parseTwoFloats("inf 0"));

    // emit_from range: 0 and 1 are in, anything past either edge is out.
    const auto inRange = [](const char* s) {
        const auto p = parseTwoFloats(s);
        return p && emitFromInRange(*p);
    };
    CHECK(inRange("0 0"));
    CHECK(inRange("1 1"));
    CHECK(inRange("0.5 0.25"));
    CHECK(!inRange("1.0001 0.5"));
    CHECK(!inRange("0.5 1.0001"));
    CHECK(!inRange("-0.0001 0.5"));
    CHECK(!inRange("0.5 -0.0001"));
    CHECK(!inRange("2 2"));

    // emit_offset: +-EMIT_OFFSET_MAX_PX per component, inclusive.
    CHECK(emitOffsetInRange(0, 0));
    CHECK(emitOffsetInRange(EMIT_OFFSET_MAX_PX, -EMIT_OFFSET_MAX_PX));
    CHECK(emitOffsetInRange(-3.5, 12));
    CHECK(!emitOffsetInRange(EMIT_OFFSET_MAX_PX + 0.01, 0));
    CHECK(!emitOffsetInRange(0, -EMIT_OFFSET_MAX_PX - 0.01));
    CHECK(!emitOffsetInRange(1e9, 0));
    CHECK(!emitOffsetInRange(std::nan(""), 0));
    CHECK(!emitOffsetInRange(0, INFINITY));

    // First parse: cfg::read() starts from the defaults (s_config = {}), so
    // an out-of-range first value resolves to the default {0, 0}, and a
    // later bad value keeps the last good one.
    const SPair dflt{0.0, 0.0};
    CHECK(resolveEmitOffset(500, 0, dflt) == dflt);
    CHECK(resolveEmitOffset(0, -129, dflt) == dflt);
    CHECK(resolveEmitOffset(std::nan(""), 0, dflt) == dflt);
    CHECK(resolveEmitOffset(5, -7, dflt) == SPair({5.0, -7.0}));
    CHECK(resolveEmitOffset(500, 0, SPair{5.0, -7.0}) == SPair({5.0, -7.0}));

    // The emit point: the pointer ("hotspot"), or emit_from's position in
    // the cursor image box when there is a cursor image, then the offset.
    const SPair ptr{100.0, 50.0};
    const SRect box{90.0, 44.0, 24.0, 32.0};                                                // hotspot at (10, 6) in a 24x32 image
    CHECK(emitPoint(ptr, box, std::nullopt, {}) == ptr);                                    // hotspot ignores the box
    CHECK(emitPoint(ptr, std::nullopt, SPair{0.5, 0.5}, {}) == ptr);                        // no cursor image: hotspot
    CHECK(emitPoint(ptr, box, SPair{0.0, 0.0}, {}) == SPair({90.0, 44.0}));                 // top-left
    CHECK(emitPoint(ptr, box, SPair{0.5, 0.5}, {}) == SPair({102.0, 60.0}));                // center
    CHECK(emitPoint(ptr, box, SPair{1.0, 0.25}, {3.0, -4.0}) == SPair({117.0, 48.0}));      // offset added after
    CHECK(emitPoint(ptr, std::nullopt, std::nullopt, {-12.5, 7.0}) == SPair({87.5, 57.0})); // hotspot plus offset
}

// Runs argv with stdout and stderr merged. Returns the exit status (127 if the
// program can't be started) and everything it printed.
static std::pair<int, std::string> runCapture(const std::vector<std::string>& argv) {
    int fds[2];
    if (pipe(fds) != 0)
        return {127, "pipe failed"};

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return {127, "fork failed"};
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> args;
        for (const auto& a : argv)
            args.push_back(const_cast<char*>(a.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast): execvp takes char*, never writes
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }

    close(fds[1]);
    std::string out;
    char        buf[4096];
    ssize_t     n = 0;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(n));
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, out};
}

namespace {
    // A shader as the loader assembles it, written to a file whose extension
    // tells glslangValidator the stage.
    struct SAssembled {
        std::string     label; // "taper.vert", "strands.frag"
        std::string     file;
        shader::SSource src;
    };

    // The shipped shaders are embedded (shader::builtin()).
    constexpr const char* SHIPPED_SHADERS = "hyprtail/shaders";

    // File names in dir with the given extension, sorted (run from the repo root).
    std::vector<std::string> globShaders(const char* dir, std::string_view ext) {
        std::vector<std::string> names;
        if (!std::filesystem::is_directory(dir))
            return names;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().extension() == ext)
                names.push_back(e.path().filename().string());
        }
        std::ranges::sort(names);
        return names;
    }

    // The validator's log for a failed pair, indented, with "<source-id>:<line>"
    // mapped back to file:line through the stage the log is currently about
    // (glslang prints each file's name before its messages).
    std::string readableLog(const std::string& log, const SAssembled& v, const SAssembled& f) {
        const shader::SSource* current = nullptr;
        std::string            out;
        std::istringstream     in{log};
        std::string            line;
        while (std::getline(in, line)) {
            if (line.empty())
                continue;
            if (line == v.file || line == f.file) {
                current = line == v.file ? &v.src : &f.src;
                continue;
            }
            out += "      " + (current ? shader::mapLog(line, *current) : line) + "\n";
        }
        return out;
    }

    enum class ePair : uint8_t {
        OK,          // links, and the loader accepts the pairing
        EXPECTS,     // links, but the fragment shader's `expects` refuses the vertex shader's topology
        MISFIT,      // links, but the declarations don't fit together (programInfo)
        LINK_FAILED, // glslangValidator -l fails
    };

    std::string expectsText(const shader::SSource& frag) {
        std::string out;
        for (const auto k : frag.expects)
            out += (out.empty() ? "" : ",") + std::string{shader::topologyName(k)};
        return out.empty() ? "any" : out;
    }

    void printPairTable(const std::vector<SAssembled>& verts, const std::vector<SAssembled>& frags, const std::vector<ePair>& result) {
        std::vector<std::string> rowLabels;
        size_t                   width = 0;
        for (const auto& v : verts) {
            rowLabels.push_back(std::format("{} ({})", v.label, shader::topologyText(v.src.topology.value_or(shader::eTopology::PATH), v.src.instances)));
            width = std::max(width, rowLabels.back().size());
        }

        std::cout << "shader pairs: ok = links and fits, expects = links but `expects` refuses the pair (not a failure), MISFIT = declarations don't fit, LINK = does not link\n";
        for (size_t j = 0; j < frags.size(); ++j)
            std::cout << std::format("  F{:<2} {} (expects {})\n", j + 1, frags[j].label, expectsText(frags[j].src));
        // Padded by hand: clang 23 cannot parse a dynamic width ("{:<{}}")
        // against libstdc++ 16, and lint parses this file with clang.
        std::cout << "  " << std::string(width, ' ');
        for (size_t j = 0; j < frags.size(); ++j)
            std::cout << std::format(" {:>7}", std::format("F{}", j + 1));
        std::cout << "\n";
        for (size_t i = 0; i < verts.size(); ++i) {
            std::cout << "  " << rowLabels[i] << std::string(width - rowLabels[i].size(), ' ');
            for (size_t j = 0; j < frags.size(); ++j) {
                const auto r = result[i * frags.size() + j];
                std::cout << std::format(" {:>7}", r == ePair::OK ? "ok" : r == ePair::EXPECTS ? "expects" : r == ePair::MISFIT ? "MISFIT" : "LINK");
            }
            std::cout << "\n";
        }
    }
}

// Every vertex shader linked with every fragment shader, shipped
// (hyprtail/shaders), found by glob so a new shader joins the matrix without
// editing this test. Each must be in the embedded table under its path with
// the file's exact bytes (a table entry embedding the wrong file fails
// here), is assembled by the loader's own preprocessor (from the embedded
// table, as CShaderSlot::builtin() does), then compiled and linked by
// glslangValidator -l in the shader's own dialect (`#version 300 es`). No
// skip list: a pair that doesn't link fails.
//
// For every pair that links, the loader's own pair checks run too
// (programInfo: `expects`, params declared identically in both stages, the
// instanced K, padding names), and the whole matrix is printed. An `expects`
// refusal is reported in the table and is not a failure: it is the loader
// declining a pairing that links. Any other refusal fails.
//
// Not covered, because it needs GL: the program contract check (uniforms
// and attributes, ShaderSlot.cpp) and the driver's own link.
static void testShaderLinkMatrix() {
    namespace fs = std::filesystem;
    if (!fs::is_directory(SHIPPED_SHADERS))
        return;

    const char*    outDir = std::getenv("OUT_DIR");
    const fs::path dir    = outDir ? fs::path{outDir} : fs::temp_directory_path() / std::format("hyprtail-glsl-{}", getpid());
    fs::create_directories(dir);

    const auto assemble = [&dir](const std::string& label, std::expected<shader::SSource, std::string> src, const std::string& stem) -> std::optional<SAssembled> {
        if (!src) {
            std::cerr << std::format("{} doesn't preprocess: {}\n", label, src.error());
            return std::nullopt;
        }
        const auto file = (dir / stem).string();
        std::ofstream(file) << src->text;
        return SAssembled{.label = label, .file = file, .src = std::move(*src)};
    };

    std::vector<SAssembled> vertShaders, fragShaders;
    for (const auto& [ext, stage, out] : {std::tuple{".vert", shader::eStage::VERTEX, &vertShaders}, std::tuple{".frag", shader::eStage::FRAGMENT, &fragShaders}}) {
        for (const auto& file : globShaders(SHIPPED_SHADERS, ext)) {
            const auto key  = "shaders/" + file;
            const auto text = shader::builtin(key);
            if (text.empty()) {
                std::cerr << std::format("hyprtail/{} exists but isn't in the embedded table (shader::builtin(), src/ShaderSource.cpp)\n", key);
                ++s_failed;
                continue;
            }
            std::ifstream     in(fs::path{SHIPPED_SHADERS} / file, std::ios::binary);
            std::stringstream disk;
            disk << in.rdbuf();
            if (disk.str() != text) {
                std::cerr << std::format("the embedded {} isn't hyprtail/{} (src/ShaderSource.cpp)\n", key, key);
                ++s_failed;
            }
            if (auto a = assemble(file, shader::preprocess(text, key, {}, stage), file))
                out->push_back(std::move(*a));
            else
                ++s_failed;
        }
    }
    CHECK(!vertShaders.empty() && !fragShaders.empty());

    if (runCapture({"glslangValidator", "--version"}).first == 127) {
        std::cerr << "glslangValidator not found, skipping the GLSL link matrix\n";
        if (!outDir)
            fs::remove_all(dir);
        return;
    }

    // Control, reported and not asserted: a fragment input the vertex stage
    // doesn't declare is the failure class the driver's link rejects (see
    // varyingCheck, ShaderSlot.cpp). If glslangValidator -l accepts it, this
    // matrix can't see that class, and says so.
    {
        const auto cv =
            assemble("canary.vert",
                     pp("#version 300 es\n#pragma hyprtail contract 2\n#pragma hyprtail topology quad\nvoid main() { ht_initVaryings(); gl_Position = ht_toClip(ht_anchor); }\n",
                        shader::eStage::VERTEX),
                     "canary.vert");
        const auto cf =
            assemble("canary.frag",
                     pp("#version 300 es\n#pragma hyprtail contract 2\nin float ht_notWritten;\nvoid main() { ht_fragColor = vec4(ht_notWritten); }\n", shader::eStage::FRAGMENT),
                     "canary.frag");
        CHECK(cv && cf);
        if (cv && cf) {
            const auto [status, log] = runCapture({"glslangValidator", "-l", cv->file, cf->file});
            if (status == 0)
                std::cerr << "note: glslangValidator -l links a fragment input the vertex shader never declares (control pair canary.vert + canary.frag): this matrix "
                             "cannot detect a fragment shader reading a varying that a vertex shader lacks\n";
            fs::remove(cv->file);
            fs::remove(cf->file);
        }
    }

    std::vector<ePair> result;
    size_t             refused = 0, failed = 0;
    for (const auto& v : vertShaders) {
        for (const auto& f : fragShaders) {
            const auto [status, log] = runCapture({"glslangValidator", "-l", v.file, f.file});
            if (status != 0) {
                ++s_failed;
                ++failed;
                result.push_back(ePair::LINK_FAILED);
                std::cerr << std::format("GLSL link FAILED: {} + {}\n{}", v.label, f.label, readableLog(log, v, f));
                continue;
            }
            ++s_passed;
            if (shader::expectsMismatch(v.src, f.src)) {
                ++refused;
                result.push_back(ePair::EXPECTS);
                continue;
            }
            const SShaderPair pair{.vert = v.src, .frag = f.src};
            if (const auto info = programInfo(pair); !info) {
                ++s_failed;
                ++failed;
                result.push_back(ePair::MISFIT);
                std::cerr << std::format("shader pair doesn't fit: {} + {}: {}\n", v.label, f.label, info.error());
                continue;
            }
            ++s_passed;
            result.push_back(ePair::OK);
            // The varying scan explains link failures; on a pair that links
            // it must find nothing, or it would blame the wrong varyings.
            CHECK(!varyingCheck(pair));
        }
    }

    printPairTable(vertShaders, fragShaders, result);
    const size_t pairs = vertShaders.size() * fragShaders.size();
    std::cout << std::format("{} vertex x {} fragment = {} pairs: {} ok, {} refused by expects, {} failed\n", vertShaders.size(), fragShaders.size(), pairs,
                             pairs - refused - failed, refused, failed);

    if (!outDir)
        fs::remove_all(dir);
}

// The smoke test's fixtures (tests/hyprtester/fixtures, rule 5
// above), checked here because smoke runs only in a compositor: every
// fixture preset parses; each layer's stages (fixture files as the loader
// reads them, or builtin: shaders) preprocess, fit together as a program
// and take the preset's keys; and, with glslangValidator, each pair links.
// Every key a shipped preset can set has a doc for make preset-docs to
// write (rule 2, by directory): each param of each shipped shader, each
// reserved param for every topology, and each source setting. One check per shader, plus one
// for the reserved params and the sources.
static void testParamDocs() {
    for (const auto& [ext, stage] : {std::pair{".vert", shader::eStage::VERTEX}, std::pair{".frag", shader::eStage::FRAGMENT}}) {
        for (const auto& file : globShaders(SHIPPED_SHADERS, ext)) {
            const auto               key = "shaders/" + file;
            const auto               src = shader::preprocess(shader::builtin(key), key, {}, stage);
            std::vector<std::string> undocumented;
            if (src)
                for (const auto& p : src->params)
                    if (p.decl.doc.empty())
                        undocumented.push_back(p.where);
            if (!src || !undocumented.empty())
                std::cerr << std::format("{}: param pragma without \"// <doc>\" at {}\n", file, src ? std::format("{}", undocumented) : src.error());
            CHECK(src && undocumented.empty());
        }
    }

    std::vector<std::string> undocumented;
    for (const auto& r : shader::reservedParams())
        for (const auto t : {shader::eTopology::PATH, shader::eTopology::INSTANCED, shader::eTopology::QUAD})
            if (r.doc(t).empty())
                undocumented.push_back(std::format("reserved {} ({})", r.decl.name, shader::topologyName(t)));
    for (const auto* kind : {"pointer", "spring"})
        for (const auto& d : source::decls(kind))
            if (d.doc.empty())
                undocumented.push_back(std::format("source {} {}", kind, d.name));
    if (!undocumented.empty())
        std::cerr << std::format("no doc: {}\n", undocumented);
    CHECK(undocumented.empty() && !source::decls("pointer").empty() && !source::decls("spring").empty());
}

static void testSmokeFixtures() {
    namespace fs        = std::filesystem;
    const fs::path root = "tests/hyprtester/fixtures";
    if (!fs::is_directory(root / "presets"))
        return;

    const bool     glslang = runCapture({"glslangValidator", "--version"}).first != 127;
    const fs::path dir     = fs::temp_directory_path() / std::format("hyprtail-fixtures-{}", getpid());
    fs::create_directories(dir);

    int presets = 0;
    for (const auto& entry : fs::directory_iterator(root / "presets")) {
        if (entry.path().extension() != ".conf")
            continue;
        ++presets;
        const auto stem = entry.path().stem().string();
        const auto fail = [&](const std::string& why) {
            std::cerr << std::format("fixture {}: {}\n", entry.path().string(), why);
            CHECK(false);
        };

        std::ifstream     in(entry.path());
        std::stringstream text;
        text << in.rdbuf();
        const auto manifest = trail::parse(text.str());
        if (!manifest) {
            fail(manifest.error());
            continue;
        }
        std::string problems;
        source::resolve(manifest->sourceKind, manifest->sourceKeys, {}, problems);
        if (!problems.empty())
            fail("source settings:" + problems);

        for (const auto& layer : manifest->layers) {
            const auto it    = manifest->layerKeys.find(layer);
            const auto keys  = it == manifest->layerKeys.end() ? std::map<std::string, std::string>{} : it->second;
            const auto stage = [&](const char* key, shader::eStage st) -> std::optional<shader::SSource> {
                const auto v = keys.find(key);
                if (v == keys.end()) {
                    fail(std::format("layer {} has no {}", layer, key));
                    return std::nullopt;
                }
                const auto embedded = shader::builtinStageKey(v->second);
                auto       src      = embedded ? shader::preprocess(shader::builtin(*embedded), v->second, {}, st) : shader::load(root / v->second, st);
                if (!src) {
                    fail(std::format("layer {}: {}", layer, src.error()));
                    return std::nullopt;
                }
                return std::move(*src);
            };
            auto vert = stage("vertex", shader::eStage::VERTEX);
            auto frag = stage("fragment", shader::eStage::FRAGMENT);
            if (!vert || !frag)
                continue;

            const SShaderPair pair{.vert = std::move(*vert), .frag = std::move(*frag)};
            const auto        info = programInfo(pair);
            if (!info) {
                fail(std::format("layer {}: {}", layer, info.error()));
                continue;
            }
            auto values = keys;
            values.erase("vertex");
            values.erase("fragment");
            problems.clear();
            resolveValues(*info, values, {}, problems);
            if (!problems.empty())
                fail(std::format("layer {}:{}", layer, problems));

            if (!glslang)
                continue;
            const SAssembled v{.label = keys.at("vertex"), .file = (dir / std::format("{}-{}.vert", stem, layer)).string(), .src = pair.vert};
            const SAssembled f{.label = keys.at("fragment"), .file = (dir / std::format("{}-{}.frag", stem, layer)).string(), .src = pair.frag};
            std::ofstream(v.file) << v.src.text;
            std::ofstream(f.file) << f.src.text;
            if (const auto [status, log] = runCapture({"glslangValidator", "-l", v.file, f.file}); status != 0)
                fail(std::format("layer {}: GLSL link failed: {} + {}\n{}", layer, v.label, f.label, readableLog(log, v, f)));
            else
                CHECK(!varyingCheck(pair));
        }
    }
    CHECK(presets > 0);
    fs::remove_all(dir);
}

// A test that throws aborts the run, which is the failure signal.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    testEmitConfig();
    testSpringChain();
    testSource();
    testParams();
    testShaderSource();
    testShaderPair();
    testManifestParse();
    testBuiltinManifests();
    testPresetCopies();
    testLayerValues();
    testCrashGuard();
    testPointerGate();
    testPostStageMonitor();
    testRing();
    testInstancedRuns();
    testWarpPath();
    testWarpPlayback();
    testEmitSpacing();
    testRetention();

    // Leaves the assembled built-ins in $OUT_DIR, if set, for inspection.
    testShaderLinkMatrix();
    testParamDocs();
    testSmokeFixtures();

    std::cout << std::format("unit: {} passed, {} failed\n", s_passed, s_failed);
    return s_failed ? 1 : 0;
}
