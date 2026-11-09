
-- ---- hyprtail smoke test (tests/hyprtester/smoke.lua) ----
-- `make smoke` appends this to hyprtester/test.lua (same directory, so its
-- relative requires keep working). Plugin keys are unknown until the plugin
-- loads, so this reports a config error at startup; the reload after
-- `/plugin load` (PluginSystem.cpp:135) applies it.

-- Output created and removed by the hotplug rounds (hyprtail_smoke.cpp),
-- far right of every other output. Without this rule it would get the
-- catch-all rule above, which disables unknown outputs.
hl.monitor({ output = "HYPRTAIL-TEST", mode = "1280x720@60", position = "20000x0", scale = "1" })

-- Plugin settings the test changes while it runs (the instanced-topology
-- rounds in hyprtail_smoke.cpp): it writes "trail=" and "params=" lines
-- to $XDG_STATE_HOME/hyprtail-smoke-plugin.conf and sends /reload, which
-- runs this file again. No file: the defaults below.
local function plugin_settings()
    local settings = {
        -- A fixture hyprtail_smoke.cpp copies in before the plugin loads
        -- (tests/hyprtester/fixtures): a path layer and an idle layer
        -- (delay 50 ms, duration 200 ms).
        -- trail:fade_ms is FADE_MS in hyprtail_smoke.cpp.
        trail    = "presets/smoke-path.conf",
        params   = "trail:fade_ms=500",
    }

    local state = os.getenv("XDG_STATE_HOME")
    local file  = state and io.open(state .. "/hyprtail-smoke-plugin.conf", "r")
    if file then
        for line in file:lines() do
            local key, value = line:match("^(%w+)=(.*)$")
            if key == "trail" or key == "params" then
                settings[key] = value
            end
        end
        file:close()
    end
    return settings
end

local settings = plugin_settings()

hl.config({
    plugin = {
        hyprtail = {
            trail    = settings.trail,
            params   = settings.params,
            warp     = "line", -- the test moves by warps; connect them into a ribbon
        },
    },
})

-- No Xwayland: the test doesn't need it, and it would claim an X display in
-- the shared /tmp/.X11-unix.
hl.config({ xwayland = { enabled = false } })
