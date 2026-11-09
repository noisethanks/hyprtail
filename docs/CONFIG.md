# Basic Customization

## Terms
- **Builtins** are default trail styles, internal to hyprtail. They can be referenced in the configs as such:
```
trail = "builtin:comet" -- builtin:ink is the lighter choice
```
- **Presets** are identical copies of the builtins. These copies are intended to provide a simple starting point for basic configurations, such as changing color or tail width. This document focuses on these kinds of basic customizations.
- A **stage** is one step in the rendering pipeline of a shader. A shader willl always have a vertex stage, and, in hyprtail, will always have a fragment stage.
- A **layer** in hyprtail is one draw layer of a shader. For example `glow` and `core` in the vivid presets. 
- The **hyprtail config root** is `$XDG_CONFIG_HOME/hypr/hyprtail/`
  (`~/.config/hypr/hyprtail/` if `XDG_CONFIG_HOME` is unset). Every
  relative path hyprtail reads from your config resolves against it.
- **Paths:** a trail config path is relative to the hyprtail config root, absolute, or `~/...` (your home directory); shader paths (a preset's stages and the `layerN_vertex`/`layerN_fragment` overrides) take the same forms, plus `builtin:<file>` for a shipped shader (`builtin:taper.vert`).


## Getting started with Presets

1. Begin with downloading the hyprtail directory from the project root and placing them it at `~/.config/hypr/hyprtail/`. It contains two folders, which are presets and shaders.
2. Preset trails are composed of the preset shaders. A preset references them by stage as such:
```
trail:vertex = builtin:taper.vert
trail:fragment = builtin:gradient.frag
```
3. To edit the shaders themselves, change those lines to
```
trail:vertex = shaders/taper.vert
trail:fragment = shaders/gradient.frag
```
3. Edit the files freely. Saving the active preset file, or a shader it
   uses, triggers autoreload. With `misc:disable_autoreload = true` saves do
   nothing; run **hyprctl reload** instead.
4. See [SHADERS.md](SHADERS.md) for more detailed documentation about shader definitions.

### Preset file format

A preset is a file `~/.config/hypr/hyprtail/presets/<name>.conf`:

```
contract    = 2
description = Thin neutral trail
layers      = core

core:vertex   = builtin:taper.vert   # can be a builtin shader, or
core:fragment = shaders/solid.frag   # your file, relative to `$XDG_CONFIG_HOME/hypr/hyprtail`
# core:fragment = ~/.config/hypr/hyprtail/shaders/solid.frag # or absolute path
core:fade_ms  = 350                  # any other key sets a layer parameter
core:width    = 4
core:color    = rgba(ffffffa0)
```

- `contract = 2` is required. It refers to hyprtail shader pipeline contract. The contract may change with future updates so this value will help track compatibility. 
- `layers = <name>[, <name>...]` lists 1–4 layer names, draw order first =
  bottom. No duplicates allowed. `source` can't be a layer name.
- `source = pointer | spring` (optional, default `pointer`) picks what
  produces the trail's points, see "Sources" below.
- `source:<name> = <value>` sets a setting of that source, see below.
- Every other line is `<layer>:<key> = <value>`, where `<layer>` must be one
  of the names in `layers`.
  - `<layer>:vertex` / `<layer>:fragment` pick that layer's shader:
    `builtin:<name>` for an embedded one (geometry: `builtin:taper.vert`,
    `builtin:scatter.vert`, `builtin:drift.vert`, `builtin:halo.vert`; looks:
    `builtin:gradient.frag`, `builtin:dots.frag`, `builtin:pulse.frag`,
    `builtin:sizzle.frag`), or
    anything else as a path to your own file. 
  - Any other `<key>` sets a default value for a parameter that layer's
    shader declares (see [SHADERS.md](SHADERS.md) for how shaders declare parameters).
- `#` starts a comment to end of line; blank lines are ignored.
- If the selected preset fails to load (missing file, parse error, bad
  shader reference), hyprtail reports it and uses `builtin:ink` instead.

### Sources

Every layer of a preset draws from one shared source of points, so the
source belongs to the preset (`source = ...`), not to a layer.

A source's settings are set as `source:<name> = <value>` in the preset or
`source:<name>=<value>` in `params`, with the same value rules and precedence
as layer parameters (the source's default < the preset < `params`), applied
live. Both sources have:
| Name | Default | Range | Meaning |
|---|---|---|---|
| `min_spacing` | `2` | 0–256 | Logical px the pointer must travel before a new point is taken. |
| `max_spacing` | `8` | 0.5–256 | Logical px two consecutive points are at most apart. Fast motion and warps are filled with points this far apart; lower is smoother and holds more points for the same fade. If set to < `min_spacing`, it defines the `min_spacing` value.
- **`pointer`** (default): the recorded history of the pointer. A point is
  taken once the pointer has moved `min_spacing` px, and a longer step is
  filled with points along it, so points are never more than `max_spacing`
  apart, whatever the refresh rate. Points are kept by age: as long as the
  preset's longest-fading `path` or `instanced` layer draws them (its
  `fade_ms`, plus 50 ms), so a trail is never cut short however long or fast
  the stroke. The buffer holds at most 4096 points; beyond that the oldest
  are dropped early, and `hyprctl hyprtail` counts them.
- **`spring`**: a chain of `capacity` points that chase each other, so the
  points are fixed and always there; nothing is recorded. The first point
  chases the pointer and each of the others the point before it, each axis an
  independent [damped spring](https://en.wikipedia.org/wiki/Damping#Damping_ratio_definition).
  Each link is set by its natural frequency and its damping ratio. Its own
  settings:

  | Name | Default | Range | Meaning |
  |---|---|---|---|
  | `capacity` | `64` | 2–4096 | The chain's length in points. A preset switch to another spring preset keeps the newest points that fit. |
  | `freq_hz` | `40` | 0.5–100 | Natural frequency of every link after the head. Each link lags about `damping_ratio / (3.14 * freq_hz)` seconds, and the lags add up down the chain: at 40 Hz and 0.9, 64 points lag about half a second. Lower is a longer, lazier rope. |
  | `damping_ratio` | `0.9` | 0.71–10 | Damping of those links: 1 is critical, above 1 sluggish. Below about 0.71 every link would amplify the bounce of the one before it, and over a long chain the tail flies off, so lower values are rejected. |
  | `head_freq_hz` | `freq_hz` | 0.5–100 | Natural frequency of the head, the link that chases the pointer. Unset, it is the chain's. |
  | `head_damping_ratio` | `damping_ratio` | 0.2–10 | Damping of the head. Unset, it is the chain's. The head may go as low as 0.2: it bounces past the pointer and back, and the chain behind it carries the bounce down the rope as a wave that shrinks toward the tail. |
  | `age_step_ms` | `10` | 0–1000 | How much older each point is than the one in front of it. It tapers a ribbon toward the tail; with a layer's `fade_ms` it sets how many points are visible (`fade_ms / age_step_ms`). |

  The chain moves in fixed 1 ms steps of wall-clock time, so it looks the
  same at any refresh rate, with VRR, and on any number of monitors.

  A chain has no severed pieces to fade, so anything that would start a new
  segment in a pointer history (screen lock, pointer constraint, a warp in
  `break` mode, a workspace change, a monitor layout change) restarts the
  chain at the pointer instead. So does a point that ends up more than
  8192 px from the head. A warp in `line` or `curve` mode moves the whole
  chain by the jump, still moving as it was. The chain stops redrawing once
  it has stopped moving and every layer's `fade_ms` has passed.

#### Warps

The plugin setting `warp` (`"break"`, `"line"` or `"curve"`) decides what
a pointer warp does to the trail: `break` starts a new segment, `line` and
`curve` fill the jump with points along a straight line or a curve, drawn
like fast motion: same spacing (`max_spacing`), from where the pointer was
to where it landed, and later fading in the same order. A `pointer` preset
times that fill:

| Name | Default | Range | Meaning |
|---|---|---|---|
| `warp_speed` | `4` | 0.05–100 | Logical px per ms a warp draws at, on average: a warp takes its length divided by `warp_speed` to draw, so a long warp moves as fast as a short one. |
| `warp_max_ms` | `250` | 1–5000 | The longest a warp takes to draw, ms. Longer warps draw faster to fit. |
| `warp_p1` | `0,0` | each −1–2 | First inner control point of the bezier that shapes the warp's position over its duration. |
| `warp_p2` | `1,1` | each −1–2 | Second inner control point. |

The curve's ends are fixed at `0,0` and `1,1`, as in `hl.curve`, and the
defaults are linear. A control point's x is clamped to 0–1; y outside 0–1
overshoots. With `warp = "break"` these keys have no effect, and a `spring`
chain moves across the jump as one body, so it doesn't declare them.

Moving the pointer before a warp has drawn draws the rest at once, and idle
(`quad`) layers wait for the pointer to be still from when the warp has
drawn, not from the warp itself.

`hyprctl hyprtail` shows which source is active, its points and capacity,
how long the pointer source keeps points and how many it dropped at the
4096 ceiling, the spacing, the warp timing, how many points a warp still has
to draw, and whether the source's points are still moving.

## Overriding a preset's shaders

| Key | Type | Default |
|---|---|---|
| `layer1_vertex` … `layer4_vertex` | path or `builtin:<file>` | `""` (use the preset's shader) |
| `layer1_fragment` … `layer4_fragment` | path or `builtin:<file>` | `""` (use the preset's shader) |

These pick a shader by the layer's **position** in the active preset's
`layers` list (1 = bottom layer), not by name. `""` means "use whatever the
preset itself specifies for that layer." Setting an index the active preset
doesn't have (e.g. `layer3_vertex` on a 1-layer preset) is a warning and is
ignored. They take a path or `builtin:<file>`, like a preset's stages. Path resolution is
the same as for a preset's own shader paths: `~`/`~/` expand to your home
directory, an absolute path is used as-is, and a relative path resolves
against the hyprtail config root (`~/.config/hypr/hyprtail/`).


## Overriding layer parameters
You can use the params property in hyprland.lua to override parameters in the active preset.
This is to allow for quick, convenient customizations without needing to edit the preset files themselves.
Refer to the [Readme](https://github.com/noisethanks/hyprtail/blob/master/README.md) for examples.
| Key | Type | Default |
|---|---|---|
| `params` | string | `""` |

One string, space-separated entries of the form `<layer>:<name>=<value>`:

```
plugin:hyprtail:params = core:width=4 glow:radius=18 core:color=rgba(ffffffa0)
```

- `<layer>` is a layer name from the active preset, or `source` for the
  preset's source (see "Sources").
- `<name>` is a parameter that layer's shader declares (or a reserved name,
  below).
- `<value>` syntax depends on the parameter's type: a plain number for
  `float`/`int`, `true`/`false` (also `1`/`0`, `yes`/`no`) for `bool`, `x,y`
  for `vec2`, and `0xAARRGGBB` / `rgba(RRGGBBAA)` / `rgb(RRGGBB)` for
  `color`.
- Precedence: a shader's own built-in default < the preset manifest's value
  < `params`. So `params` always wins.
- An unknown layer, unknown parameter name, or a value that's the wrong
  type or out of the parameter's declared range is a per-entry warning; that
  one entry is ignored and everything else in the string still applies.
- Re-read on every config reload and on every preset or shader file save — no
  separate reload needed. That includes the copy count K of an `instanced`
  layer (`builtin:jitter`'s `copies`, `builtin:comet`'s `sparks:count`): it changes
  live, without recompiling the shader.

**Reserved parameter names**, settable on any layer the same way as any
other parameter:

| Name | Type | Default | Meaning |
|---|---|---|---|
| `enabled` | bool | `true` | `false` turns this layer off entirely (not even compiled). |
| `draw_when_cursor_hidden` | bool | `true` for `path` and `instanced` layers, `false` for `quad` layers | Whether the layer keeps drawing while the OS cursor is hidden. |
| `fade_ms` | float, ms | `500` | Trail/particle layers: how long a point stays visible after it's created. |
| `start_ms` | float, ms | `500` | Idle-style layers: how long the pointer must sit still before the layer starts showing. |
| `duration_ms` | float, ms | `1500` | Idle-style layers: how long it stays visible once started (`0` = forever, until the pointer moves). |

Example: `params = "glow:enabled=false"` turns off a layer named `glow`
without switching presets.

