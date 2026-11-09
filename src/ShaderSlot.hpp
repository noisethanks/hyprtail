#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <helpers/memory/Memory.hpp>
#include <render/Shader.hpp>

#include "ShaderPair.hpp"
#include "ShaderSource.hpp"

namespace hyprtail {
    // What `hyprctl hyprtail` shows for a slot.
    struct SSlotStatus {
        bool        active  = false; // a program is in use
        bool        pending = false; // a reloaded pair waits for the next render
        std::string topology;
        std::string vertOrigin, fragOrigin; // active program's files, "" = built-in
        std::string lastResult;             // outcome of the last compile attempt
    };

    // One user-replaceable shader program (a layer's): built-in stages,
    // optional user files per stage, preprocessing, deferred compile on the
    // next render, and "keep the previous working program" on failure.
    // Reports under shader:<name>.
    class CShaderSlot {
      public:
        CShaderSlot(std::string name, std::string vertBuiltin, std::string fragBuiltin);

        // Main thread (config reload, file change). Reads each configured
        // stage (empty = the slot's built-in, "builtin:<file>" = that shipped
        // shader, else a path), preprocesses it, and queues the pair for
        // the next prepare(). A file or preprocessing error is reported and
        // keeps the active program. Appends every file to watch (including
        // missing ones, so creating them reloads).
        void reload(const std::string& vertConfigured, const std::string& fragConfigured, std::vector<std::filesystem::path>& watch);

        // Inside a render (GL current): compile a pending pair. On failure
        // (declarations that don't fit together, compile/link error, or it
        // uses inputs the contract doesn't provide) it's reported and:
        //   - same files as the active program (an edit): keep the active one;
        //   - other files (a config change) or nothing active: built-in.
        // Returns an error only if no program is usable at all.
        std::optional<std::string> prepare();

        // GL context must be current.
        void                release();

        const SP<CShader>&  shader() const;
        const SProgramInfo& info() const;       // of the active program
        uint64_t            generation() const; // bumped on every activation

        // Uniform location in the active program, cached until the next
        // program swap. -1 if not declared or unused (glUniform ignores -1).
        GLint              loc(const std::string& uniform);

        const std::string& name() const;
        bool               hasPending() const;
        SSlotStatus        status() const;

      private:
        const SShaderPair&                     builtin();
        std::optional<std::string>             compileAndActivate(const SShaderPair& pair);

        std::string                            m_name, m_vertBuiltin, m_fragBuiltin;
        std::string                            m_activeOrigin; // vertOrigin + '\n' + fragOrigin of the active program
        std::string                            m_activeVertOrigin, m_activeFragOrigin;
        std::string                            m_lastResult = "none yet";
        std::optional<SShaderPair>             m_builtin;
        std::optional<SShaderPair>             m_pending;
        SP<CShader>                            m_shader;
        SProgramInfo                           m_info;
        uint64_t                               m_generation = 0;
        std::unordered_map<std::string, GLint> m_locs;
    };
}
