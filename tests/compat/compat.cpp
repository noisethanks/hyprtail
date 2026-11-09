// Compile-only check of src/compat.hpp against whichever Hyprland headers the
// build selects (`make test-compat`, see the Makefile). Passing means: every
// wrapper instantiates, and the assumptions compat.hpp relies on about
// Hyprland hold in this checkout. Nothing here is ever run or linked.

#include <filesystem>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "../../src/compat.hpp"

using Desktop::View::CWindow;

// Detection is by what a type offers, so it must not be able to see both or
// neither: the wrapper would then silently pick one.
template <typename W>
constexpr bool hasMetadata = requires(const W& w) {
    w.metadata().appID();
    w.metadata().title();
};
template <typename W>
constexpr bool hasMembers = requires(const W& w) {
    w.m_class;
    w.m_title;
};
static_assert(hasMetadata<CWindow> != hasMembers<CWindow>, "CWindow must offer exactly one of metadata() and m_class/m_title");

// compat probes the location overload of CLogger::log(). The LOG macro is the
// other half of the same Hyprland change; if they ever separate, the probe is
// what to trust and this is the reminder to look.
#ifdef LOG
static_assert(hyprtail::compat::detail::logTakesLoc<Log::CLogger>, "LOG exists but CLogger::log() has no location overload");
#else
static_assert(!hyprtail::compat::detail::logTakesLoc<Log::CLogger>, "CLogger::log() has a location overload but there is no LOG");
#endif

// The hook guards in compat.hpp (namespace hooks) pass for the selected
// checkout's real classes, or including compat.hpp would not compile. This
// checks each probe also says no, and that every stand-in is matched by
// exactly one probe of its function. Stand-ins have the pin's signatures, the
// ones Hyprland main has since ba616caa (#16458: a leading
// Render::CRenderContext& on renderSoftwareCursorsFor and saveBufferForMirror,
// PointerManager.hpp:67 and OpenGL.hpp:228 at main e2ad066), or a changed
// qualifier. compat.hpp declares Render::CRenderContext on both checkouts.
namespace {
    struct SStandInCursorWithContext {
        void renderSoftwareCursorsFor(Render::CRenderContext&, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D> = {}, bool = false, bool = false);
    };
    struct SStandInSaveWithContext {
        bool saveBufferForMirror(Render::CRenderContext&, const CBox&);
    };
    struct SStandInWarpNotConst {
        void warpTo(const Vector2D&, bool = false);
    };
    struct SStandInSaveMatching {
        bool saveBufferForMirror(const CBox&);
    };
    struct SStandInCursorMatching {
        void renderSoftwareCursorsFor(PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D> = {}, bool = false, bool = false);
    };
    struct SStandInWarpMatching {
        void warpTo(const Vector2D&, bool = false) const;
    };
    // Neither known signature: both probes must say no.
    struct SStandInCursorOther {
        void renderSoftwareCursorsFor(PHLMONITOR, const Time::steady_tp&, CRegion&);
    };
    struct SStandInSaveOther {
        bool saveBufferForMirror(const CBox&, bool);
    };
}
namespace hooks = hyprtail::compat::hooks;
static_assert(!hooks::renderSoftwareCursorsForMatches<SStandInCursorWithContext>, "the pin guard accepted a renderSoftwareCursorsFor with a leading context");
static_assert(hooks::renderSoftwareCursorsForCtxMatches<SStandInCursorWithContext>, "the main guard rejected a renderSoftwareCursorsFor with a leading context");
static_assert(hooks::renderSoftwareCursorsForMatches<SStandInCursorMatching>, "the pin guard rejected a renderSoftwareCursorsFor with the pin's signature");
static_assert(!hooks::renderSoftwareCursorsForCtxMatches<SStandInCursorMatching>, "the main guard accepted a renderSoftwareCursorsFor without a context");
static_assert(!hooks::renderSoftwareCursorsForMatches<SStandInCursorOther> && !hooks::renderSoftwareCursorsForCtxMatches<SStandInCursorOther>,
              "a guard accepted a renderSoftwareCursorsFor with an unknown signature");

static_assert(!hooks::saveBufferForMirrorMatches<SStandInSaveWithContext>, "the pin guard accepted a saveBufferForMirror with a leading context");
static_assert(hooks::saveBufferForMirrorCtxMatches<SStandInSaveWithContext>, "the main guard rejected a saveBufferForMirror with a leading context");
static_assert(hooks::saveBufferForMirrorMatches<SStandInSaveMatching>, "the pin guard rejected a saveBufferForMirror with the pin's signature");
static_assert(!hooks::saveBufferForMirrorCtxMatches<SStandInSaveMatching>, "the main guard accepted a saveBufferForMirror without a context");
static_assert(!hooks::saveBufferForMirrorMatches<SStandInSaveOther> && !hooks::saveBufferForMirrorCtxMatches<SStandInSaveOther>,
              "a guard accepted a saveBufferForMirror with an unknown signature");

static_assert(!hooks::controllerWarpToMatches<SStandInWarpNotConst>, "the guard accepted a non-const warpTo");
static_assert(hooks::controllerWarpToMatches<SStandInWarpMatching>, "the guard rejected a warpTo with the expected signature");

// The render-context wrappers' probes see the same checkout compat's
// RenderCtx was chosen for: the renderer offers the globals exactly when
// there is no context type, and each context-taking call exactly when there is.
template <typename R>
constexpr bool hasRenderGlobals = requires(R& r) {
    r.m_renderData;
    r.m_renderPass;
};
static_assert(hasRenderGlobals<Render::IHyprRenderer> != hyprtail::compat::hasRenderContext,
              "IHyprRenderer must offer m_renderData/m_renderPass exactly when there is no CRenderContext");
// removePassElementsOfType() does nothing when its probe fails, so a probe
// that stopped matching on the pin would silently drop the unload cleanup.
template <typename R>
constexpr bool canRemoveFromPass = requires(R& r, const std::string& type) { r.m_renderPass.removeAllOfType(type); };
static_assert(canRemoveFromPass<Render::IHyprRenderer> != hyprtail::compat::hasRenderContext, "the renderer's own pass must be clearable exactly when there is no CRenderContext");
template <typename R, typename C>
constexpr bool takesCtx = requires(R& r, C& c, const CBox& box, const CHyprColor& color) {
    r.projectBoxToTarget(c, box, HYPRUTILS_TRANSFORM_NORMAL);
    r.getConvertedColor(c, color);
};
template <typename GL, typename C>
constexpr bool scissorTakesCtx = requires(GL& gl, C& c, const pixman_box32* box) { gl.scissor(c, box, true); };
static_assert(takesCtx<Render::IHyprRenderer, hyprtail::compat::RenderCtx> == hyprtail::compat::hasRenderContext,
              "projectBoxToTarget/getConvertedColor take a context exactly when there is a CRenderContext");
static_assert(scissorTakesCtx<Render::GL::CHyprOpenGLImpl, hyprtail::compat::RenderCtx> == hyprtail::compat::hasRenderContext,
              "CHyprOpenGLImpl::scissor takes a context exactly when there is a CRenderContext");

// A pass element built on compat's adapter base is complete (not abstract)
// for the selected checkout.
namespace {
    class CStandInPassElement : public hyprtail::compat::CPassElement {
      public:
        std::vector<UP<IPassElement>> drawWith(hyprtail::compat::RenderCtx& /*ctx*/) override {
            return {};
        }
        std::optional<CBox> boundingBoxWith(hyprtail::compat::RenderCtx& /*ctx*/) override {
            return std::nullopt;
        }
        const char* passName() override {
            return "CStandInPassElement";
        }
        ePassElementType type() override {
            return EK_CUSTOM;
        }
    };
    static_assert(!std::is_abstract_v<CStandInPassElement>, "compat::CPassElement leaves a Hyprland IPassElement virtual unimplemented");

    void           cursorHandler(hyprtail::compat::RenderCtx& /*ctx*/, const PHLMONITOR& /*monitor*/, bool /*screencopy*/) {}
    void           mirrorHandler(hyprtail::compat::RenderCtx& /*ctx*/) {}
    CFunctionHook* s_standInHook = nullptr;
}

// Instantiates every wrapper with the argument shapes the plugin uses.
[[maybe_unused]] static void instantiateAll(const CWindow& window, HANDLE handle, hyprtail::compat::RenderCtx& ctx) {
    hyprtail::compat::log(Log::INFO, "no arguments");
    hyprtail::compat::log(Log::INFO, "{} {}", 1, std::string{"two"});
    // The call that compiles unchecked against Hyprland's own logger on main.
    hyprtail::compat::log(Log::ERR, "can't write {}", std::filesystem::path{"x"}.string());

    hyprtail::compat::bindArrayBuffer(0);

    [[maybe_unused]] const std::string& cls   = hyprtail::compat::windowClass(window);
    [[maybe_unused]] const std::string& title = hyprtail::compat::windowTitle(window);

    hyprtail::compat::StatusCommand     command = hyprtail::compat::registerStatusCommand(handle, "x", [](bool json) { return std::string{json ? "{}" : ""}; });
    HyprlandAPI::unregisterHyprCtlCommand(handle, command);

    [[maybe_unused]] Render::SRenderData& data = hyprtail::compat::renderData(ctx);
    hyprtail::compat::addPassElement(ctx, makeUnique<CStandInPassElement>());
    hyprtail::compat::removePassElementsOfType("CStandInPassElement");
    hyprtail::compat::scissor(ctx, nullptr, true);
    [[maybe_unused]] const Mat3x3     proj  = hyprtail::compat::projectBoxToTarget(ctx, CBox{}, HYPRUTILS_TRANSFORM_NORMAL);
    [[maybe_unused]] const CHyprColor color = hyprtail::compat::convertedColor(ctx, CHyprColor{});
    [[maybe_unused]] const auto listener = hyprtail::compat::listenRenderStage([](eRenderStage /*stage*/, const PHLMONITOR& /*monitor*/, hyprtail::compat::RenderCtx* /*ctx*/) {});

    [[maybe_unused]] void*      cursor = hooks::cursorDetour<&cursorHandler, s_standInHook>();
    [[maybe_unused]] void*      mirror = hooks::mirrorDetour<&mirrorHandler, s_standInHook>();
}
