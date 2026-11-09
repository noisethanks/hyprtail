#pragma once

// The one place hyprtail adapts to differences between Hyprland checkouts
// (the pinned commit and main). Each divergence is detected by what the
// headers offer, never by a commit or version number, and everything else
// calls the wrapper here instead of the Hyprland API directly.
//
// Detection is a `requires` expression on a template parameter wherever the
// name exists in both trees. Two things can't be: a header that moved and a
// type that was replaced under a new name. Naming an undeclared type is a hard
// error outside a template, so those use __has_include.
//
// A discarded `if constexpr` branch is only skipped when it depends on a
// template parameter, so each wrapper takes the Hyprland type as one.
//
// Divergences handled (pin efb5099 vs main e2ad066):
//   - Log::CLogger::log() takes a location string on main (Logger.hpp:41,45).
//     Its 3-argument form with runtime strings still compiles when called the
//     old way and then prints the format string as the location, so nothing
//     outside this file may call Log::logger->log() (`make check-log`).
//   - CHyprOpenGLImpl::bindArrayBuffer() caches the binding on main
//     (OpenGL.hpp:229); a raw glBindBuffer desyncs it.
//   - CWindow::m_class / m_title became metadata().appID() / title().
//   - Window.hpp moved to desktop/view/window/.
//   - hyprctl commands: SHyprCtlCommand became IPC::Socket1::SCommand.
//   - Per-render state (Hyprland #16458). 969b4b11 moved IHyprRenderer's
//     m_renderData and m_renderPass into Render::CRenderContext
//     (render/Context.hpp); ba616caa then passed that context explicitly to
//     the IPassElement virtuals, renderSoftwareCursorsFor,
//     saveBufferForMirror, scissor, projectBoxToTarget, getConvertedColor and
//     the render.stage event. Plugin code takes a RenderCtx& from the hook or
//     event it runs in and hands it to the wrappers below (the "render
//     context" section). On the pin RenderCtx is an empty token and the
//     wrappers use the globals. Nothing outside this file names
//     m_renderData, m_renderPass or IHyprRenderer::context()
//     (`make check-render-context`): on main the context is reset before
//     RENDER_POST (Renderer.cpp:1928-1932 and :2442 there), so a monitor read
//     from it at RENDER_POST is null.
//
// Also here: compile-time checks that each hooked function still has one of
// the signatures its detour is written for, and the detours themselves for the
// two hooks whose signature differs (the `hooks` namespace at the end).

#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <plugins/PluginAPI.hpp>
#include <plugins/HookSystem.hpp>
#include <debug/log/Logger.hpp>
#include <event/EventBus.hpp>
#include <render/OpenGL.hpp>
#include <render/Renderer.hpp>
#include <render/pass/PassElement.hpp>
#include <pointer/PointerManager.hpp>
#include <pointer/PointerController.hpp>

#include "PostStageMonitor.hpp"

#if __has_include(<desktop/view/window/Window.hpp>)
#include <desktop/view/window/Window.hpp>
#include <desktop/view/window/WindowMetadata.hpp>
#else
#include <desktop/view/Window.hpp>
#endif

#if __has_include(<ipc/s1/S1.hpp>)
#include <ipc/s1/S1.hpp>
#define HYPRTAIL_COMPAT_IPC_S1 1
#endif

// Added by Hyprland 969b4b11 (#16458). A new type, so it can't be probed with
// `requires`; everything that takes it is probed on its own below, and
// asserted to agree with this.
#if __has_include(<render/Context.hpp>)
#include <render/Context.hpp>
#define HYPRTAIL_COMPAT_RENDER_CONTEXT 1
#endif

// Named by the main-shaped signatures the hook probes compare against, so it
// must be declared on the pin too, where it stays incomplete.
namespace Render {
    class CRenderContext;
}

namespace hyprtail::compat {

    // ---------------------------------------------------------------- logging

    namespace detail {
        template <typename L>
        inline constexpr bool logTakesLoc = requires { static_cast<void (L::*)(Hyprutils::CLI::eLogLevel, std::string_view, std::string_view)>(&L::log); };

        template <typename L = Log::CLogger>
        void logString(Hyprutils::CLI::eLogLevel level, const std::string& msg) {
            L& logger = *Log::logger;
            if constexpr (logTakesLoc<L>)
                logger.log(level, std::string_view{"hyprtail"}, std::string_view{msg});
            else
                logger.log(level, std::string_view{"[hyprtail] " + msg});
        }
    }

    // The "[hyprtail] " prefix is added here, callers don't write it.
    template <typename... Args>
    void log(Hyprutils::CLI::eLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        detail::logString(level, std::format(fmt, std::forward<Args>(args)...));
    }

    // -------------------------------------------------------------------- GL

    // Array-buffer binding through Hyprland's cache when it has one, raw
    // otherwise. Plugin code must bind 0 again before deleting a buffer.
    template <typename GL = Render::GL::CHyprOpenGLImpl>
    void bindArrayBuffer(GLuint buffer) {
        GL& gl = *Render::GL::g_pHyprOpenGL;
        if constexpr (requires { gl.bindArrayBuffer(buffer); })
            gl.bindArrayBuffer(buffer);
        else
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
    }

    // ---------------------------------------------------------------- windows

    template <typename W>
    const std::string& windowClass(const W& w) {
        if constexpr (requires { w.metadata().appID(); })
            return w.metadata().appID();
        else
            return w.m_class;
    }

    template <typename W>
    const std::string& windowTitle(const W& w) {
        if constexpr (requires { w.metadata().title(); })
            return w.metadata().title();
        else
            return w.m_title;
    }

    // ---------------------------------------------------------------- hyprctl

    // Registers an exact-match `hyprctl <name>` command. `reply` gets true for
    // -j. Unregister with HyprlandAPI::unregisterHyprCtlCommand.
#ifdef HYPRTAIL_COMPAT_IPC_S1
    using StatusCommand = SP<IPC::Socket1::SCommand>;

    inline StatusCommand registerStatusCommand(HANDLE handle, const std::string& name, std::function<std::string(bool json)> reply) {
        using namespace IPC::Socket1;
        return HyprlandAPI::registerHyprCtlCommand(handle, SCommand{.name = name, .match = COMMAND_MATCH_EXACT, .handler = [reply = std::move(reply)](const SRequest& req) {
                                                                        return SResponse{reply(req.format == FORMAT_JSON)};
                                                                    }});
    }
#else
    using StatusCommand = SP<SHyprCtlCommand>;

    inline StatusCommand registerStatusCommand(HANDLE handle, const std::string& name, std::function<std::string(bool json)> reply) {
        return HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{.name = name, .exact = true, .fn = [reply = std::move(reply)](eHyprCtlOutputFormat format, std::string) {
                                                                               return reply(format == eHyprCtlOutputFormat::FORMAT_JSON);
                                                                           }});
    }
#endif

    // --------------------------------------------------------- render context

    // The render being drawn. On main, Hyprland's own Render::CRenderContext,
    // passed to hooks and render-stage listeners. On the pin an empty token:
    // the state it stands for is the renderer's globals. Only ever a
    // reference to what a hook or event handed over, or to a token; plugin
    // code never looks inside it, only passes it to the wrappers here.
#ifdef HYPRTAIL_COMPAT_RENDER_CONTEXT
    using RenderCtx                        = Render::CRenderContext;
    inline constexpr bool hasRenderContext = true;
#else
    struct RenderCtx {};
    inline constexpr bool hasRenderContext = false;
#endif

    // The render's SRenderData: ctx.m_data on main, the renderer's
    // m_renderData on the pin.
    template <typename C, typename R = Render::IHyprRenderer>
    auto& renderData(C& ctx) {
        if constexpr (requires(R& r) { r.m_renderData; }) {
            R& renderer = *g_pHyprRenderer;
            return renderer.m_renderData;
        } else
            return ctx.m_data;
    }

    // Adds a pass element to the render's pass. On main through
    // IHyprRenderer::addPassElement, which honours a redirected pass
    // (Renderer.cpp:571-577 there), never ctx.m_pass directly.
    template <typename C, typename R = Render::IHyprRenderer>
    void addPassElement(C& ctx, UP<IPassElement>&& element) {
        if constexpr (requires(C& c, UP<IPassElement>&& e) { R::addPassElement(c, std::move(e)); })
            R::addPassElement(ctx, std::move(element));
        else {
            R& renderer = *g_pHyprRenderer;
            renderer.m_renderPass.add(std::move(element));
        }
    }

    // Outside a render: drops queued pass elements of this type, so none can
    // draw after the plugin released its GL objects. Main has no pass that
    // outlives a render: every reset() of the context clears it
    // (Context.cpp:62-77 there), so there is nothing to drop.
    template <typename R = Render::IHyprRenderer>
    void removePassElementsOfType(const std::string& type) {
        if constexpr (requires(R& r) { r.m_renderPass.removeAllOfType(type); }) {
            R& renderer = *g_pHyprRenderer;
            renderer.m_renderPass.removeAllOfType(type);
        }
    }

    // Scissor through Hyprland's cached state; nullptr turns it off.
    template <typename C, typename GL = Render::GL::CHyprOpenGLImpl>
    void scissor(C& ctx, const pixman_box32* box, bool transform) {
        GL& gl = *Render::GL::g_pHyprOpenGL;
        if constexpr (requires { gl.scissor(ctx, box, transform); })
            gl.scissor(ctx, box, transform);
        else
            gl.scissor(box, transform);
    }

    template <typename C, typename R = Render::IHyprRenderer>
    Mat3x3 projectBoxToTarget(C& ctx, const CBox& box, eTransform transform) {
        R& renderer = *g_pHyprRenderer;
        if constexpr (requires { renderer.projectBoxToTarget(ctx, box, transform); })
            return renderer.projectBoxToTarget(ctx, box, transform);
        else
            return renderer.projectBoxToTarget(box, transform);
    }

    // The color converted to the render target's image description.
    template <typename C, typename R = Render::IHyprRenderer>
    CHyprColor convertedColor(C& ctx, const CHyprColor& color) {
        R& renderer = *g_pHyprRenderer;
        if constexpr (requires { renderer.getConvertedColor(ctx, color); })
            return renderer.getConvertedColor(ctx, color);
        else
            return renderer.getConvertedColor(color);
    }

    // render.stage listener. The handler gets the stage, the monitor being
    // rendered and the render's context. On main the monitor is the event's
    // own and the context is null at RENDER_PRE and RENDER_POST, where main
    // has none (EventBus.hpp:29-34 there); the monitor must never be read
    // from the context. On the pin the event is only the stage and the
    // context is always the token. The monitor is m_renderData's, except at
    // RENDER_POST: end() has reset m_renderData.pMonitor by then (OpenGL.cpp:
    // 857, from endRender() at Renderer.cpp:2229, before RENDER_POST at
    // :2250), so RENDER_POST gets the one the same render's RENDER_LAST_MOMENT
    // (:2227) saw, or none (CPostStageMonitor). A null monitor means skip.
    using RenderStageHandler = std::function<void(eRenderStage, const PHLMONITOR&, RenderCtx*)>;

    inline CHyprSignalListener listenRenderStage(RenderStageHandler handler) {
        return Event::bus()->m_events.render.stage.listen([handler = std::move(handler)](const auto& event) -> void {
            if constexpr (requires {
                              event.stage;
                              event.monitor;
                              event.context;
                          }) {
                RenderCtx* ctx = event.context ? &event.context->get() : nullptr;
                handler(event.stage, event.monitor, ctx);
            } else {
                static RenderCtx                        token;
                static CPostStageMonitor<PHLMONITORREF> post;
                PHLMONITOR                              monitor;
                if (event == RENDER_POST)
                    monitor = post.post().lock();
                else {
                    monitor = renderData(token).pMonitor.lock();
                    if (event == RENDER_BEGIN)
                        post.begin();
                    else if (event == RENDER_LAST_MOMENT)
                        post.lastMoment(monitor);
                }
                handler(event, monitor, &token);
            }
        });
    }

    // ---------------------------------------------------------- pass element

    namespace detail {
        template <typename E>
        inline constexpr bool passTakesCtx = requires(E& e, Render::CRenderContext& c) { e.draw(c); };

        // Hyprland's IPassElement virtuals, implemented once for whichever
        // set the headers declare and forwarded to the *With(RenderCtx&)
        // functions a hyprtail element implements. An override with a
        // different parameter list is a hard error rather than a failed
        // probe, so each set lives in its own class template and only the
        // one picked below is ever instantiated. hyprtail elements never
        // blur.
        template <typename Base>
        class CPassElementWithCtx : public Base {
          public:
            virtual std::vector<UP<IPassElement>> drawWith(RenderCtx& ctx)        = 0;
            virtual std::optional<CBox>           boundingBoxWith(RenderCtx& ctx) = 0;

            std::vector<UP<IPassElement>>         draw(Render::CRenderContext& ctx) final {
                return drawWith(ctx);
            }
            std::optional<CBox> boundingBox(Render::CRenderContext& ctx) final {
                return boundingBoxWith(ctx);
            }
            bool needsLiveBlur(Render::CRenderContext& /*ctx*/) final {
                return false;
            }
            bool needsPrecomputeBlur(Render::CRenderContext& /*ctx*/) final {
                return false;
            }
        };

        template <typename Base>
        class CPassElementNoCtx : public Base {
          public:
            virtual std::vector<UP<IPassElement>> drawWith(RenderCtx& ctx)        = 0;
            virtual std::optional<CBox>           boundingBoxWith(RenderCtx& ctx) = 0;

            std::vector<UP<IPassElement>>         draw() final {
                RenderCtx token;
                return drawWith(token);
            }
            std::optional<CBox> boundingBox() final {
                RenderCtx token;
                return boundingBoxWith(token);
            }
            bool needsLiveBlur() final {
                return false;
            }
            bool needsPrecomputeBlur() final {
                return false;
            }
        };
    }

    // Base of every hyprtail pass element: implement drawWith(),
    // boundingBoxWith(), passName() and type().
    using CPassElement = std::conditional_t<detail::passTakesCtx<IPassElement>, detail::CPassElementWithCtx<IPassElement>, detail::CPassElementNoCtx<IPassElement>>;

    static_assert(detail::passTakesCtx<IPassElement> == hasRenderContext,
                  "hyprtail: IPassElement::draw takes a Render::CRenderContext& exactly when <render/Context.hpp> exists, or RenderCtx would be the wrong type for it. "
                  "Hyprland split the two apart: port compat::CPassElement and RenderCtx in src/compat.hpp together.");

    // ------------------------------------------------------------------ hooks

    // Hooked functions (main.cpp installHooks). A detour is installed through
    // reinterpret_cast<void*>, so the compiler never compares its signature with
    // the target's: if Hyprland adds or changes a parameter, the plugin still
    // compiles and the detour then misreads every argument. These are the
    // signatures the detours are written for, `this` aside, and the checks
    // below fail the build when a target no longer has exactly one of them.
    //
    // Hyprland ba616caa (#16458) added a leading Render::CRenderContext& to
    // renderSoftwareCursorsFor and saveBufferForMirror (PointerManager.hpp:67
    // and OpenGL.hpp:228 at main e2ad066). For those two the detours are here:
    // cursorDetour() and mirrorDetour() pick the one written for the target's
    // signature, which calls hyprtail's single handler with the render
    // context (the token on the pin) and calls the original with the
    // arguments unchanged. warpTo is the same on both, so its detour is in
    // main.cpp, which asserts it matches DetourT<ControllerWarpTo>.
    namespace hooks {
        using RenderSoftwareCursorsForSig    = void(PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool, bool);
        using RenderSoftwareCursorsForCtxSig = void(Render::CRenderContext&, PHLMONITOR, const Time::steady_tp&, CRegion&, std::optional<Vector2D>, bool, bool);
        using ControllerWarpToSig            = void(const Vector2D&, bool) const;
        using SaveBufferForMirrorSig         = bool(const CBox&);
        using SaveBufferForMirrorCtxSig      = bool(Render::CRenderContext&, const CBox&);

        // Pointer to a member of C with signature Sig. A named alias because
        // clang-format mangles the bare `Sig C::*` spelling in declarations.
        template <typename C, typename Sig>
        using MemberPtr = Sig C::*;

        // The member function pointer types of the real targets, per signature.
        using RenderSoftwareCursorsFor    = MemberPtr<Pointer::CPointerManager, RenderSoftwareCursorsForSig>;
        using RenderSoftwareCursorsForCtx = MemberPtr<Pointer::CPointerManager, RenderSoftwareCursorsForCtxSig>;
        using ControllerWarpTo            = MemberPtr<Pointer::CPointerController, ControllerWarpToSig>;
        using SaveBufferForMirror         = MemberPtr<Render::GL::CHyprOpenGLImpl, SaveBufferForMirrorSig>;
        using SaveBufferForMirrorCtx      = MemberPtr<Render::GL::CHyprOpenGLImpl, SaveBufferForMirrorCtxSig>;

        // The free-function type of the detour, and of the original called
        // through CFunctionHook::m_original: `this` becomes a void pointer.
        template <typename Pmf>
        struct Detour;
        template <typename R, typename C, typename... A>
        struct Detour<R (C::*)(A...)> {
            using type = R (*)(void*, A...);
        };
        template <typename R, typename C, typename... A>
        struct Detour<R (C::*)(A...) const> {
            using type = R (*)(const void*, A...);
        };
        template <typename Pmf>
        using DetourT = typename Detour<Pmf>::type;

        // static_cast of the address picks the overload with exactly this
        // type, and is ill-formed (so false here) when there is none: a changed
        // parameter list, a changed constness, or an overload set without it.
        // Templates on the class so tests/compat can check them against
        // stand-ins with other signatures.
        template <typename C>
        inline constexpr bool renderSoftwareCursorsForMatches = requires { static_cast<MemberPtr<C, RenderSoftwareCursorsForSig>>(&C::renderSoftwareCursorsFor); };
        template <typename C>
        inline constexpr bool renderSoftwareCursorsForCtxMatches = requires { static_cast<MemberPtr<C, RenderSoftwareCursorsForCtxSig>>(&C::renderSoftwareCursorsFor); };
        template <typename C>
        inline constexpr bool controllerWarpToMatches = requires { static_cast<MemberPtr<C, ControllerWarpToSig>>(&C::warpTo); };
        template <typename C>
        inline constexpr bool saveBufferForMirrorMatches = requires { static_cast<MemberPtr<C, SaveBufferForMirrorSig>>(&C::saveBufferForMirror); };
        template <typename C>
        inline constexpr bool saveBufferForMirrorCtxMatches = requires { static_cast<MemberPtr<C, SaveBufferForMirrorCtxSig>>(&C::saveBufferForMirror); };

        // Exactly one known signature: neither means Hyprland changed it
        // again, both an overload set the detour can't be matched to.
        static_assert(renderSoftwareCursorsForMatches<Pointer::CPointerManager> != renderSoftwareCursorsForCtxMatches<Pointer::CPointerManager>,
                      "hyprtail: CPointerManager::renderSoftwareCursorsFor no longer has exactly one of the signatures cursorDetour() is written for. The detour is "
                      "installed through reinterpret_cast, so the compiler cannot check it and it would misread its arguments. Port hooks::RenderSoftwareCursorsFor* and "
                      "cursorDetour() in src/compat.hpp together.");
        static_assert(controllerWarpToMatches<Pointer::CPointerController>,
                      "hyprtail: CPointerController::warpTo no longer has the signature hkControllerWarpTo (main.cpp) is written for. The detour is installed through "
                      "reinterpret_cast, so the compiler cannot check it and it would misread its arguments. Port the detour and hooks::ControllerWarpTo in src/compat.hpp "
                      "together.");
        static_assert(saveBufferForMirrorMatches<Render::GL::CHyprOpenGLImpl> != saveBufferForMirrorCtxMatches<Render::GL::CHyprOpenGLImpl>,
                      "hyprtail: CHyprOpenGLImpl::saveBufferForMirror no longer has exactly one of the signatures mirrorDetour() is written for. The detour is installed "
                      "through reinterpret_cast, so the compiler cannot check it and it would misread its arguments. Port hooks::SaveBufferForMirror* and mirrorDetour() in "
                      "src/compat.hpp together.");
        // The pin-shaped detours make a token for the handler, which only
        // works while RenderCtx is the token.
        static_assert(renderSoftwareCursorsForCtxMatches<Pointer::CPointerManager> == hasRenderContext &&
                          saveBufferForMirrorCtxMatches<Render::GL::CHyprOpenGLImpl> == hasRenderContext,
                      "hyprtail: the hooked functions take a Render::CRenderContext& exactly when <render/Context.hpp> exists. Hyprland split the two apart: port the "
                      "detours and RenderCtx in src/compat.hpp together.");

        // Cursor hook. Handler: void(RenderCtx&, const PHLMONITOR&, bool
        // screencopy), run before the original. Hook: the CFunctionHook* the
        // detour is installed with, read for the original at every call.
        template <auto Handler, CFunctionHook*& Hook>
        struct CursorShim {
            static void detour(void* self, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos, bool screencopy,
                               bool forceRender) {
                RenderCtx token;
                Handler(token, pMonitor, screencopy);
                reinterpret_cast<DetourT<RenderSoftwareCursorsFor>>(Hook->m_original)(self, pMonitor, now, damage, overridePos, screencopy, forceRender);
            }
            static void detourCtx(void* self, Render::CRenderContext& ctx, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos,
                                  bool screencopy, bool forceRender) {
                Handler(ctx, pMonitor, screencopy);
                reinterpret_cast<DetourT<RenderSoftwareCursorsForCtx>>(Hook->m_original)(self, ctx, pMonitor, now, damage, overridePos, screencopy, forceRender);
            }
        };

        // Mirror-copy hook. Handler: void(RenderCtx&), run after the original;
        // the original's result is returned unchanged.
        template <auto Handler, CFunctionHook*& Hook>
        struct MirrorShim {
            static bool detour(void* self, const CBox& box) {
                const bool ok = reinterpret_cast<DetourT<SaveBufferForMirror>>(Hook->m_original)(self, box);
                RenderCtx  token;
                Handler(token);
                return ok;
            }
            static bool detourCtx(void* self, Render::CRenderContext& ctx, const CBox& box) {
                const bool ok = reinterpret_cast<DetourT<SaveBufferForMirrorCtx>>(Hook->m_original)(self, ctx, box);
                Handler(ctx);
                return ok;
            }
        };

        // The detour for the target's actual signature, for installHook().
        template <auto Handler, CFunctionHook*& Hook, typename C = Pointer::CPointerManager>
        void* cursorDetour() {
            using Shim = CursorShim<Handler, Hook>;
            if constexpr (renderSoftwareCursorsForCtxMatches<C>) {
                static_assert(std::is_same_v<decltype(&Shim::detourCtx), DetourT<MemberPtr<C, RenderSoftwareCursorsForCtxSig>>>);
                return reinterpret_cast<void*>(&Shim::detourCtx);
            } else {
                static_assert(std::is_same_v<decltype(&Shim::detour), DetourT<MemberPtr<C, RenderSoftwareCursorsForSig>>>);
                return reinterpret_cast<void*>(&Shim::detour);
            }
        }

        template <auto Handler, CFunctionHook*& Hook, typename C = Render::GL::CHyprOpenGLImpl>
        void* mirrorDetour() {
            using Shim = MirrorShim<Handler, Hook>;
            if constexpr (saveBufferForMirrorCtxMatches<C>) {
                static_assert(std::is_same_v<decltype(&Shim::detourCtx), DetourT<MemberPtr<C, SaveBufferForMirrorCtxSig>>>);
                return reinterpret_cast<void*>(&Shim::detourCtx);
            } else {
                static_assert(std::is_same_v<decltype(&Shim::detour), DetourT<MemberPtr<C, SaveBufferForMirrorSig>>>);
                return reinterpret_cast<void*>(&Shim::detour);
            }
        }
    }

}

#undef HYPRTAIL_COMPAT_IPC_S1
#undef HYPRTAIL_COMPAT_RENDER_CONTEXT
