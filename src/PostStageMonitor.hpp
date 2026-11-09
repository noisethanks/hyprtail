#pragma once

#include <utility>

// The monitor a pin-shaped render.stage listener hands to RENDER_POST. The
// pin's event carries only the stage, and its end() clears the render data's
// monitor before RENDER_POST, so the monitor comes from the same render's
// RENDER_LAST_MOMENT instead (compat::listenRenderStage). Only that stage is
// remembered, and RENDER_BEGIN forgets it: a render that bails out between
// its stages leaves nothing for the next render's RENDER_POST. Ref is a weak
// monitor reference (reset(), copyable). No Hyprland headers, header-only for
// the unit tests.
namespace hyprtail {
    template <typename Ref>
    class CPostStageMonitor {
      public:
        // RENDER_BEGIN: a new render starts.
        void begin() {
            m_ref.reset();
        }

        // RENDER_LAST_MOMENT, with the monitor the render data names then.
        void lastMoment(Ref monitor) {
            m_ref = std::move(monitor);
        }

        // RENDER_POST: the monitor remembered for this render, empty if
        // there is none. Forgets it, so it is handed out at most once.
        Ref post() {
            Ref out = std::move(m_ref);
            m_ref.reset();
            return out;
        }

      private:
        Ref m_ref;
    };
}
