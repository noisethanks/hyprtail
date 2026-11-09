#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <render/pass/PassElement.hpp>
#include <render/Shader.hpp>
#include <helpers/memory/Memory.hpp>
#include <hyprutils/animation/BezierCurve.hpp>

#include "compat.hpp"
#include "Config.hpp"
#include "Layer.hpp"
#include "Source.hpp"
#include "Trail.hpp"
#include "TrailBuffer.hpp"
#include "WarpPath.hpp"

// GPU mirror of the pointer-history ring: one VAO + VBO of SGpuNode.
//
// VBO: [front pad, n0 .. n(count-1), back pad]. The front pad is a copy of n0
// flagged as a segment start, the back pad a copy of the newest node, so the
// ends have a zero-length neighbor ("no neighbor" in the shader).
//
// Four bindings of the same VBO, divisor 1, at consecutive node offsets, so
// instance i sees prev = n(i-1), p0 = n(i), p1 = n(i+1), next = n(i+2)
// (hyprtail/shaders/prelude/path.glsl):
//   locations 0..1    prev  (offset 0 nodes)  pos, bits
//   locations 2..6    p0    (1 node)          pos, birth, velocity, dist, bits
//   locations 7..11   p1    (2 nodes)         pos, birth, velocity, dist, bits
//   locations 12..13  next  (3 nodes)         pos, bits
// 14 locations: GLES 3.0 guarantees at least 16 vertex attributes. Path
// layers draw size() - 1 instances, one per segment p0 -> p1.
//
// A second VAO over the same VBO serves instanced layers: one
// node's fields at locations 0..4 (pos, birth, velocity, dist, bits), with a
// divisor of K so K consecutive instances read the same node. GLES 3.0 has
// no base-instance draw, so pointInstanced() re-points the five attributes
// at the first node to draw before every draw.
class CNodeBuffer {
  public:
    // No GL in the destructor: destroy() runs explicitly while the context is
    // current. Creates VAOs/VBO sized for ringCapacity, recreating them if
    // the capacity changed. On failure returns false with a description in
    // error, and leaves nothing allocated.
    bool ensure(size_t ringCapacity, std::string& error);
    // Uploads src when sourceNeedsUpload() says so: a new generation, or
    // every frame for a source that needs continuous upload.
    void upload(const ISource& src);
    void destroy();

    // Forget what was uploaded, so the next upload happens whatever the
    // source's generation is. A source's generation counts its own changes,
    // so a replacement source can show a number the old one already uploaded.
    void   invalidate();

    GLuint vao() const;          // path layers
    GLuint instancedVao() const; // instanced layers, see pointInstanced()
    double refMs() const;        // reference time of the uploaded birthMs values

    // The ring nodes of the last upload, oldest -> newest, without the pads.
    std::span<const SGpuNode> nodes() const;

    // Instanced layers: leaves the instanced VAO bound, its attributes
    // starting at ring node `firstNode` (0 = oldest, counted in the last
    // upload) with divisor `copies`. Draw copies x (nodes in the run from
    // firstNode, see forEachInstancedRun) instances.
    void pointInstanced(size_t firstNode, GLuint copies);

  private:
    GLuint                m_vao         = 0;
    GLuint                m_instVao     = 0;
    GLuint                m_vbo         = 0;
    size_t                m_vboNodes    = 0;
    uint64_t              m_uploadedGen = UINT64_MAX;
    double                m_refMs       = 0.0;
    std::vector<SGpuNode> m_ordered;
};

// A trail instance: the trail source, its GPU mirror, and the layers drawn
// over it, in order (first = bottom).
struct STrail {
    // Starts as the pointer history (RING_CEILING nodes); the first
    // trail's resolved source settings resize or replace it, see
    // applyPendingState().
    explicit STrail(uint64_t seedBase) : source(std::make_unique<CTrailRing>(RING_CEILING, seedBase)), seedBase(seedBase) {}

    std::unique_ptr<ISource>          source;
    uint64_t                          seedBase = 0; // for a replacement source
    CNodeBuffer                       gpu;
    std::vector<UP<hyprtail::CLayer>> layers;
    GLuint                            quadVao = 0; // empty: quad layers use gl_VertexID only

    // The active trail: which layers exist and their built-in/
    // path shader identity and defaults. `pendingTrail` is queued by
    // applyConfig() (main thread) on every config reload; prepareLayers()
    // (GL current) swaps `layers` to match it only if it actually differs
    // from `activeTrail`, so an unrelated reload doesn't recompile shaders
    // or interrupt a running fade.
    std::optional<hyprtail::trail::SResolved> pendingTrail;
    hyprtail::trail::SResolved                activeTrail; // default-empty until the first prepareLayers()

    // The source's `source:<name>` settings last applied, and whether they
    // must be resolved again (new source, new trail): applyPendingState()
    // redoes the work only when something changed.
    std::map<std::string, std::string> sourceOverrides;
    bool                               sourceDirty = true;

    // When the source was last ticked (ms since plugin load), and whether it
    // was still moving then. A source that was at rest has no elapsed time to
    // integrate: the gap since the last tick is idle time, not motion.
    double lastTickMs = 0.0;
    bool   animating  = false;

    // The source's settings as last resolved (capacity, min/max_spacing,
    // the warp keys), and the warp's timing curve built from warp_p1/warp_p2 at
    // the same time, never per warp. Null = linear.
    hyprtail::source::SSettings            settings;
    SP<Hyprutils::Animation::CBezierCurve> warpCurve;

    hyprtail::cfg::eWarpMode               warpMode = hyprtail::cfg::eWarpMode::BREAK;

    // A line or curve warp's nodes still to be drawn (warp::sample inserts
    // them as they come due). Cleared on a break and on a warp mode change.
    hyprtail::warp::CPlayback warpPlayback;

    // Emit offset: nullopt = hotspot. Applied at insert only;
    // quad layers don't use it (they anchor to lastPos below).
    std::optional<Vector2D> emitFromNorm;
    Vector2D                emitOffsetPx{0.0, 0.0};

    // Next insert starts a new segment: workspace changes, lock, pointer
    // constraints, and warps when warpMode == BREAK.
    bool pendingBreak = false;

    // Pointer stillness, for quad layers: last position seen and when it
    // changed (ms since plugin load). Quad layers draw around quadAnchor(),
    // which is lastPos unless a warp is playing; while one plays,
    // lastMotionMs is when it will have drawn, in the future.
    Vector2D lastPos;
    double   lastMotionMs = 0.0;

    // First pointer motion event since the source last took a node: where
    // the next sampled step began, for its births (sampleSource).
    std::optional<double> motionSinceInsertMs;

    // Last cursor-image geometry seen (for the shape-change break):
    // hotspot and logical size at the last cursorChanged event that
    // actually moved the box, so same-shape re-applies and animated-cursor
    // frame commits (which also fire cursorChanged, PointerManager.cpp:135,
    // 153, 165, 187, 201, 286) don't spuriously break the trail.
    Vector2D lastCursorHotspot;
    Vector2D lastCursorSizeLogical;

    // The node buffer couldn't be created: path layers are off until reload.
    bool gpuFailed = false;
};

// A layer's reach this render: its padding expression plus its damage_padding.
float layerExtentPx(const hyprtail::CLayer& layer);

// Where quad layers draw (ht_anchor, their damage box): the pointer, or the
// playback head while a warp is still drawing (CPlayback::anchor).
Vector2D quadAnchor(const STrail& trail);

// GL context must be current (inside a render, or at unload).
void trailReleaseGpu(STrail& trail);

// One layer to draw this render, with the box its lifecycle damaged.
struct SLayerDraw {
    hyprtail::CLayer* layer = nullptr;
    CBox              boxLocal; // logical, monitor-local
    float             extentPx = 0.F;
};

// All of a trail's visible layers for one render of one monitor, drawn in
// layer order, in one pass element (beneath the cursor, see main.cpp).
class CLayerPassElement : public hyprtail::compat::CPassElement {
  public:
    // nowMs is the instant the caller used for visibility and damage.
    CLayerPassElement(STrail* trail, std::vector<SLayerDraw> draws, double nowMs);
    ~CLayerPassElement() override = default;

    // Also called directly, outside the pass, by the screenshare-exclude hook.
    std::vector<UP<IPassElement>> drawWith(hyprtail::compat::RenderCtx& ctx) override;
    std::optional<CBox>           boundingBoxWith(hyprtail::compat::RenderCtx& ctx) override;

    const char*                   passName() override {
        return "CLayerPassElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    void                    drawLayer(hyprtail::compat::RenderCtx& ctx, const SLayerDraw& d);

    STrail*                 m_trail = nullptr;
    std::vector<SLayerDraw> m_draws;
    double                  m_nowMs = 0.0;
};
