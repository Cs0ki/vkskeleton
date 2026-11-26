> **Development notes / research log.** This is the chronological log kept while
> building the renderer. Paths, script names (`re-vulkan/`, `build_vkskeleton.sh`,
> `test_vkskeleton.sh`, GL tracer tools) and `/tmp/...` capture files refer to the
> original working setup and are not part of this repository. See the top-level
> `README.md` for current usage.

# Sven Co-op renderer RE / Vulkan notes

## Reference measurement only (rejected): Vulkan via Zink

`../svencoop-vulkan.sh` was an experiment that launches the game with:

```sh
export MESA_LOADER_DRIVER_OVERRIDE=zink
```

That makes Mesa run the game's OpenGL calls through **Zink** (OpenGL-over-Vulkan) on RADV instead of the native `radeonsi` GL driver. Verified with:

```sh
glxinfo -B                                      # OpenGL renderer: radeonsi
MESA_LOADER_DRIVER_OVERRIDE=zink glxinfo -B     # OpenGL renderer: zink Vulkan 1.4 (RADV POLARIS10)
```

Steam launch option equivalent:

```text
MESA_LOADER_DRIVER_OVERRIDE=zink %command%
```

This is only a comparison measurement. It does **not** change `hw.so`; Mesa translates its GL calls to Vulkan. It is **not** the native rewrite and is not used by the prototype below.

## Why a native Vulkan renderer is a long-term project

- `hw.so` is a 32-bit x86 shared library (~3 MB `.text`) with only ~12 exported functions and ~76 imported GL symbols. There is no renderer source or `hw.so.dbg` in the install.
- `MetaHookSv/Renderer` is a full source renderer replacement for the same engine family, but it is Windows/MSVC/x86, OpenGL 4.4 Core, and contains no Vulkan code. It is useful as an architecture reference only. **It is not installed and its hooks are not being used.**
- Messing with its hooks is pointless here; everything below is standalone.

## What was built: working GL-call tracer

Directory contents:

| File | Purpose |
| --- | --- |
| `glfuncs.txt` | 133 GL/GLX function names to interpose |
| `gen_c.py` | Parses `/usr/include/GL/*.h` and generates type-correct C wrappers |
| `wrappers.c` | Generated wrappers; forwards to real GL and records each call |
| `tracer.c` | Counters, caller-module attribution, sampled return addresses, report writer |
| `gl_names_c.h` | Generated name/index table shared by wrappers and tracer |
| `build.sh` | Builds 32-bit `gltracer.so` without `gcc-multilib` |

Build:

```sh
cd re-vulkan
./build.sh
```

The build works around missing 32-bit dev packages by using small shims in `compat/`, compiling with `-m32`, and linking directly with `ld -m elf_i386 -shared ... /usr/lib/i386-linux-gnu/libc.so.6`.

### Result of the first successful full trace

Launched through Steam with all 133 wrappers preloaded:

```sh
# temporarily in svencoop.sh, then launch via Steam
export GLTRACE_CALLERS=1
export GLTRACE_SAMPLE=200
export LD_PRELOAD=/path/to/gltracer.so
steam -applaunch 225840 -windowed -w 640 -h 480 -insecure -nomaster -nosteamoverlay
kill -TERM "$(pgrep -x svencoop_linux | head -1)"   # dump report
```

Report: `/tmp/svencoop-gltrace-54848.txt`, **945,808 calls in ~14 s**, all attributed to `hw.so`.

Top calls:

```text
120120  glTexCoord2f
119980  glVertex2f
114843  glEnable
 82283  glMatrixMode
 64432  glBindTexture
 60854  glDisable
 50560  glBlendFunc
 41143  glPushMatrix / glLoadIdentity / glPopMatrix (~41k each)
 34280  glTranslatef
 3424   glDrawArrays / glVertexPointer / glTexCoordPointer
  858   glBindFramebufferEXT
```

Notably unused in this run: shaders, VBOs, vertex attrib arrays, `glDrawElements`, `glReadPixels`, `glFlush`/`glFinish`. The renderer is overwhelmingly **fixed-function immediate-mode GL 1.x** plus `GL_EXT_framebuffer_object` and `glActiveTextureARB`.

### Do not go back to the assembly interposer

The earlier generated assembly stubs crashed the game as soon as two or more wrappers were active (SIGSEGV near `0xed9136bd`, `mov 0x58(%esi),%ecx`) even with `-nosteamoverlay` and even with no sampling. The C-wrapper generator is stable with the full 133-function list. Keep using `gen_c.py`/`wrappers.c`.

## How to trace again after restoring the launcher

`svencoop.sh` has been restored to its original state. To trace, temporarily add this just before `STATUS=42`:

```sh
export GLTRACE_CALLERS=1
export GLTRACE_SAMPLE=200
export LD_PRELOAD=/path/to/re-vulkan/gltracer.so
```

and redirect the game line to a log if needed:

```sh
${DEBUGGER} "${GAMEROOT}/${GAMEEXE}" ${GAMEPARAMS} > /tmp/game-stdout.log 2>&1
```

Launch via Steam, let it run, then `kill -TERM` the game to flush the report. Remove the hook afterward.

Gotchas learned:

- Env vars passed to `steam -applaunch` do not reliably reach the game; Steam/overlay rewrites `LD_PRELOAD`. Editing `svencoop.sh` works.
- Running `svencoop_linux` directly misses Steam runtime libs (`libopenal.so.1`, Pulse symbols, `hw.so` loading). Use the Steam launcher.
- Reports are per-PID (`/tmp/svencoop-gltrace-<pid>.txt`) because multiple helper processes inherit the preload.

## Vulkan port roadmap

1. **Keep the stock launcher playable** while doing RE. Zink is not the native rewrite and is not used by this prototype.
2. Use the tracer to formalize the real GL subset. The hot path is immediate mode + fixed-function state, not modern GL.
3. Decide the integration point:
   - **Interposer translation layer:** keep exporting the same GL symbols, but implement a state tracker that emits Vulkan commands. This avoids needing the undocumented `hw.so` export ABI, but must reproduce GL semantics (matrix stack, texture units, FBOs, blending, fog).
   - **Renderer replacement:** use `MetaHookSv/Renderer` only to learn the plugin/export pattern, then implement a new Linux/Vulkan renderer behind the engine's renderer ABI. This requires recovering that ABI from `hw.so`; it is the higher-risk route.
4. Start with a Vulkan backend covering the measured subset: immediate-mode vertex buffering, fixed-function emulation in shaders, texture/FBO emulation, and swapchain presentation. Validate call-for-call against the trace.
5. Only after parity, worry about performance and the remaining rare modern-GL entry points.

This is a multi-stage reverse-engineering effort; the tracer is the first concrete step toward defining the Vulkan backend's required surface.

## Important correction: hw.so is the whole engine

The dlsym tracer (`dlsymtrace.c`, `build_dlsymtrace.sh`) showed that the engine asks `hw.so` for exactly one interface:

```text
VENGINE_LAUNCHER_API_VERSION002
```

That is `IEngineAPI::Run`, the GoldSrc engine entrypoint, not a renderer-only interface. The returned object's vtable has three slots (destructor(s) plus `Run`). Therefore **replacing `hw.so` would mean reimplementing the entire engine**, not just the renderer. The native rewrite must hook/replace rendering inside `hw.so` — the GL interposer is the practical integration point.

## Native Vulkan prototype status

`vkskeleton.c` / `build_vkskeleton.sh` is an experimental native-Vulkan prototype. It is **disabled by default** and must never be preloaded unless explicitly enabled:

```sh
VKSKELETON_ENABLE=1 LD_PRELOAD=/path/to/vkskeleton.so ...
```

### Window capture redesign

The earlier SDL-based capture was abandoned:

- Interposing `SDL_GL_CreateContext` was fragile. Immediately after that call, `glXGetCurrentDisplay()` returned `NULL` and `glXGetCurrentDrawable()` returned `0`.
- Interposing `SDL_CreateWindow` / `SDL_GetWindowWMInfo` was rejected too: SDL dev headers are not installed, and manual `SDL_SysWMinfo` ABI guessing is unnecessary risk.
- One enabled run produced `failed to create sdl window`, so the SDL hook was removed.

The current prototype captures the window one layer lower, directly from Xlib:

- `XCreateWindow` / `XCreateSimpleWindow`: record `Display*`, `Window`, parent, and requested size.
- `XMapWindow` / `XMapRaised`: mark the real game window as mapped.
- The largest mapped captured window is selected, and the Vulkan Xlib surface/swapchain is created from it.
- `SDL_GL_SwapWindow` is still interposed, but only for presentation; it no longer affects window creation.

This avoids SDL headers completely and keeps the stock launcher untouched.

### Build-time shader generation

`gen_shaders.py` emits the two tiny SPIR-V 1.0 modules directly, so the build needs no GLSL compiler. `build_vkskeleton.sh` runs `spirv-val` when it is available and skips validation otherwise. The generated vertex shader takes already-transformed vec2 positions, UVs, a vec4 color, and a vec2 `(alpha_ref, alpha_func)` pair. The fragment shader samples a `sampler2D`, multiplies it by the color to emulate `GL_MODULATE`/`GL_REPLACE`, and discards fragments that fail tracked GL alpha-test state. All GoldSrc matrix emulation stays on the CPU, including the GL-to-Vulkan Y flip.

An earlier crash in `vkCreateGraphicsPipelines` was caused by invalid decoration layout in the generated SPIR-V (`OpDecorate` appeared after variable declarations). Moving decorations into the annotation section fixed validation and the driver crash.

### Runtime gates

| Variable | Effect |
| --- | --- |
| `VKSKELETON_ENABLE=1` | Required. Without it the library still forwards GL/X calls but does no Vulkan work. |
| `VKSKELETON_GEOMETRY=1` | Enables GL immediate-mode state/vertex tracking and Vulkan draws. Unset means clear/present scaffolding only. |
| `VKSKELETON_LOG` | Log path; defaults to `/tmp/vkskeleton.log`. |
| `VKSKELETON_LOG_PID=1` | Appends `.pid` to the log path. Useful because Steam helper processes also inherit the preload and can truncate a shared log. |

For bisecting, `gcc -DVKSKELETON_NO_GEOMETRY_WRAPPERS` builds a variant that does not export the geometry-tracking GL wrappers.

### Verified enabled run

With `VKSKELETON_ENABLE=1`, `VKSKELETON_GEOMETRY=1`, and the preload temporarily injected into `svencoop.sh`, the game stayed alive and the prototype logged:

```text
vkskeleton: loaded (enabled, geometry=1)
vkskeleton: captured X window 23068687 parent=852 32x32
vkskeleton: captured X window 23068693 parent=852 640x480
vkskeleton: X window 23068693 mapped
vkskeleton: selected X window 23068693 640x480 mapped=1
vkskeleton: init
vkskeleton: instance ready
vkskeleton: surface ready
vkskeleton: physical device selected
vkskeleton: device ready
vkskeleton: device procs ready
vkskeleton: surface info ready
vkskeleton: swap extent 640x480 format=44
vkskeleton: swapchain ready
vkskeleton: sync ready
vkskeleton: render pass ready
vkskeleton: image views ready
vkskeleton: depth image ready
vkskeleton: framebuffers ready
vkskeleton: vertex shader ready
vkskeleton: fragment shader ready
vkskeleton: texture resources ready
vkskeleton: default texture ready
vkskeleton: pipeline layout ready
vkskeleton: blend/depth pipelines ready
vkskeleton: vertex buffer ready
vkskeleton: graphics resources ready
vkskeleton: ready
vkskeleton: first Vulkan draw (210 vertices)
vkskeleton: first Vulkan present
```

The 32×32 utility window was correctly ignored in favor of the mapped 640×480 game window. No pipeline, shader, acquire, submit, or present failures were logged, and there was no SDL window-creation error. The launcher was restored afterward.

### How to test

`test_vkskeleton.sh` is the opt-in Steam harness. It temporarily injects the preload into `svencoop.sh`, launches through Steam so the 32-bit runtime is available, prints the logs, kills the game, and restores the launcher via `trap`:

```sh
cd re-vulkan
./build_vkskeleton.sh
./test_vkskeleton.sh 1 15
```

Usage:

```text
./test_vkskeleton.sh [geometry=0|1] [seconds]
```

- `1`: tracking + real Vulkan draws
- `0`: clear/present scaffolding only
- If a previous run was interrupted, the harness restored `svencoop.sh` from a clean backup before patching it.
- Do not launch `svencoop_linux` or `svencoop-vulkan.sh` directly from a shell; they miss Steam's 32-bit runtime (`libopenal.so.1`, then `hw.so`).

**VAC: no restriction (owner's decision).** `play_vulkan.sh` and `test_vkskeleton.sh` launch without `-insecure` and never block server connects, so the hooked client can join VAC-secured servers. A preloaded library hooking OpenGL resembles a classic GoldSrc wallhack; a VAC ban for this game cannot be appealed, and that risk was accepted by the account owner. (History: an earlier version used `-insecure` plus a kill-on-`Connecting to` guard after a test window was used to join a public server.)

### Playing with the Vulkan renderer

`play_vulkan.sh` launches the game through Steam with `vkskeleton.so` and no time limit, using your normal resolution/window settings, with no VAC restriction. `svencoop.sh` is hooked only while the game runs and restored when it exits (also on Ctrl-C; a hard-killed run is repaired on the next start). The Vulkan log goes to `/tmp/vkplay.log.<pid>`, game output to `/tmp/vk-play-stdout.log`.

```sh
cd re-vulkan
./play_vulkan.sh                     # play; online servers allowed
./play_vulkan.sh +map hl_t00         # extra game args are passed through
```

### Textured draw milestone

The prototype uploads the measured RGBA8 / `GL_UNSIGNED_BYTE` texture subset into real Vulkan images:

- One descriptor-set layout, descriptor pool, combined-image-sampler binding at set 0 / binding 0, and a small sampler set (linear/nearest × repeat/clamp).
- A host-visible 8 MiB staging buffer plus a dedicated transfer command buffer and fence for synchronous texture uploads.
- Slot 0 is a default white texture, so untextured geometry still samples valid image data.
- `glTexImage2D` creates/recreates a Vulkan image and view; `glTexSubImage2D` updates a rectangle of an existing image. GL texture row order is preserved; the coordinate-system flip is applied to vertex Y instead, so images are not vertically mirrored.
- `glTexEnvf` `GL_TEXTURE_ENV_MODE` is tracked; `GL_REPLACE` forces white vertex color so the fragment result is the sampled texel, while `GL_MODULATE` keeps vertex-color × texture.
- `glTexParameterf`/`glTexParameteri` track min/mag filter and S/T wrap per texture. The observed font/HUD textures use `GL_LINEAR` + `GL_CLAMP`, so the prototype selects linear/clamp samplers instead of the old linear/repeat default.

### Geometry, depth and blending

GoldSrc's fixed-function matrix work is emulated on the CPU. The vertex shader consumes a CPU-transformed clip-space `vec4` position, two UV pairs, `vec4` color and the alpha-test parameter pair; position Y is flipped from GL's bottom-left origin to Vulkan's top-left origin, and GL clip Z `[-w, w]` is remapped to Vulkan `[0, w]` (see "World textures" below for why the divide is left to the GPU).

- A `D24_UNORM_S8_UINT` depth attachment is created for the swapchain framebuffers; the render pass clears it each frame to 1.0.
- The observed `glBlendFunc` pairs are `0x302 0x303` (`GL_SRC_ALPHA`, `GL_ONE_MINUS_SRC_ALPHA`), `0x302 0x1` (`GL_SRC_ALPHA`, `GL_ONE`) and `0x1 0x1` (`GL_ONE`, `GL_ONE`). Four blend pipelines (opaque, alpha, additive-src-alpha, additive-one) × depth/no-depth cover them.
- `glEnable`/`glDisable(GL_ALPHA_TEST)` and `glAlphaFunc` are tracked. Observed functions were `GL_NOTEQUAL ref 0` (`0x205`) and `GL_GREATER ref 0.25` (`0x204`). Vertex attribute location 3 carries `(alpha_ref, alpha_func)` so the fragment shader can `discard` failed fragments; this is what keeps HUD/text cutouts intact.
- `glEnable`/`glDisable(GL_DEPTH_TEST)` is tracked, and separate depth/no-depth pipelines exist. The HUD, console and text are drawn with GL depth testing disabled, so they must not be rejected by the depth attachment even when earlier geometry wrote depth.
- `record_frame_vertices()` batches by contiguous texture slot, blend mode and depth mode, changing pipeline and descriptor set only at batch boundaries.
- Geometry drawn while a non-default FBO is bound is skipped; offscreen FBO contents are not emulated.

### World textures: multitexture units and perspective-correct UVs

Two separate bugs made the world look wrong even though texture uploads worked.

**1. Flat per-face colours — texture units were ignored.** `hw.so` loads GL through GLEW and draws world brushes with two texture units: unit 0 = surface texture, unit 1 = lightmap (`glActiveTextureARB`, `glMultiTexCoord2fARB`). The GL tracer never saw `glMultiTexCoord*` because it only counts a fixed function list. The prototype had a single "current texture", so binding the lightmap on unit 1 replaced the surface texture, and the per-unit texcoords were dropped; each face sampled one texel. Fix:

- Track the active unit, the texture bound per unit, `GL_TEXTURE_2D` enable per unit, and `GL_TEXTURE_ENV_MODE` / `GL_RGB_SCALE` per unit (`glTexEnvf` and `glTexEnvi`).
- Wrap `glActiveTexture[ARB]` and `glMultiTexCoord2f[v][ARB]`. Because they come from `glXGetProcAddress` (GLEW), `substitute()` stores the real pointer the engine would have received and the wrapper forwards to it.
- Vertices carry a second UV pair (attribute location 4) and a second texture slot. The pipeline layout has two descriptor sets with the same layout: set 0 = unit 0, set 1 = unit 1. The fragment shader outputs `color × tex0 × tex1`; when unit 1 is disabled the 1×1 white texture is bound, so no branch is needed. Batches split on either slot.
- `GL_REPLACE` on unit 0 resolves to white vertex colour; `GL_RGB_SCALE` of a `GL_COMBINE` unit is folded into the vertex RGB (equivalent because the shader result is a pure product).
- `GL_RGB` source uploads (seen for 8×8 textures) are expanded to RGBA instead of dropped. Observed formats: `RGBA/RGBA8/RGB` internal with `GL_RGBA` + `GL_UNSIGNED_BYTE` source (bulk, including 128×128 lightmaps), `GL_RGB` source (rare), one 1×1 `GL_BYTE` (still ignored).

**2. Textures warping as the camera moved — affine interpolation.** Vertices were divided by `w` on the CPU and sent with `w = 1`, so the rasterizer interpolated UVs linearly in screen space (PS1-style swimming). Vertices are now emitted in clip space `(x, -y, (z + w) / 2, w)` after the CPU near/far clip, attribute 0 is a `vec4`, and the vertex shader passes it straight to `gl_Position`, so the GPU divides and interpolates perspective-correctly. Confirmed in-game: textures no longer warp.

`VKSKELETON_DEBUG=1` (opt-in, off by default) logs each distinct `glTexImage2D` level/format/type combination, the texture units activated and the units receiving `glMultiTexCoord*`.

### Studio models

Studio models already render (verified in-game: the `hl_t00` hologram is textured with its additive glow). An earlier "models missing" note was a false alarm from comparing GL and Vulkan captures taken from different camera positions.

`VKSKELETON_DEBUG=1` statistics (dumped at frames 600/2400/3600) show how `hw.so` draws them: immediate-mode `glBegin(GL_TRIANGLE_STRIP)` / `GL_TRIANGLE_FAN` (~1,150 strips and ~165 fans per frame in `hl_t00`), every vertex with a valid uploaded unit-0 texture, unit 1 off, roughly half opaque and half `GL_ONE, GL_ONE` additive. World brushes are `GL_POLYGON`; HUD/text is `glDrawArrays(GL_QUADS)` with 2D vertices (some with a colour array enabled, whose colours are not yet applied). No FBO-bound draws were dropped, and ~98.5% of submitted triangles survive near/far clipping.

`glDepthRange` is emulated: the range is folded into clip-space Z at emit time, `z' = n·w + (f − n)·(z + w)/2`, which equals GL's window depth after the GPU divide. Draws are emitted at `glEnd`/`glDrawArrays`, so the range current at that moment is the one GL would use. In `hl_t00` without weapons only `0..1` was observed; the compressed viewmodel range is untested until a weapon is held.

`GL_COLOR_ARRAY` is applied in the client-array path. The engine uses 4 × `GL_FLOAT` colours with stride 24 for some HUD draws; float and unsigned-byte, 3- or 4-component arrays are read per vertex, and the current colour is restored afterwards.

### Client-array path

The measured subset has 3,424 `glDrawArrays` calls, all `GL_QUADS` with interleaved stride 16: two UV floats at offset 0 and two XY floats at offset 8. `glVertexPointer`, `glTexCoordPointer`, `glEnableClientState` and `glDrawArrays` are now intercepted, and `emit_array_geometry()` converts those arrays into the same triangle list used by immediate mode. The first observed calls emit 54/84/42/24 vertices and use the dynamically updated font-atlas texture slot.

Verified with `./test_vkskeleton.sh 1 30 '+map hl_t00'` after the diagnostic cleanup:

```text
vkskeleton: depth image ready
vkskeleton: blend/depth pipelines ready
vkskeleton: first Vulkan draw (210 vertices)
vkskeleton: Vulkan draw size 210 vertices
vkskeleton: first Vulkan present
vkskeleton: Vulkan draw size 414 vertices
...
launcher RESTORED
```

Per-call texture/array/state diagnostics were removed after verification; only structural init/draw/failure logs remain. `VK_ERROR_OUT_OF_DATE_KHR` from `AcquireNextImageKHR` now stops Vulkan presentation instead of flooding the log with acquire failures.

### HUD/text verification

Matched late-map captures at t=50 s (`+map hl_t00`) compare the clean GL launcher (`/tmp/gl20-t50-0x1600015.png`) with the Vulkan prototype (`/tmp/vk20-t50-0x3a00015.png`):

| Region | mean abs diff | pixels >32 |
| --- | ---: | ---: |
| full frame | 22.92 | 18.4% |
| top 100 px | 25.85 | 23.0% |
| center | 18.60 | 13.5% |
| HUD strip | 28.72 | 24.2% |
| bottom-left | 36.86 | 27.8% |
| bottom-right | 22.61 | 24.0% |

High-resolution crops of top-right text and HUD-right text show the same glyph strokes. The alpha-test, blend-mode and sampler fixes visibly removed the earlier garbage/overbright glyphs.

### Menu/startup font fix: sub-image staging layout

Menu and startup text used to render every character as the first glyph of the font atlas (e.g. all `J`). The cause was in `upload_texture_pixels()`, not in UVs, vertex positions or matrices:

- The font atlas is filled by many per-glyph `glTexSubImage2D` calls at increasing `xoffset` (0, 4, 8, … 226).
- The old code copied each sub-image into the staging buffer at column `xoffset` of a full-texture-width layout (`bufferRowLength = tex->width`).
- `VkBufferImageCopy` always reads source rows starting at buffer column 0; `imageOffset` only moves the **destination**. So every glyph upload actually copied the stale first-glyph region from the staging buffer.

Fix: each sub-image is now packed tightly at staging column 0 (`memcpy` to `upload_map + row * row_bytes`, `w` pixels per row) with `copy.bufferRowLength = w`. Rule for any future upload path: **sub-image uploads must be tightly packed at staging column 0 with `bufferRowLength = w`; position them only through `imageOffset`.**

Verified visually: `/tmp/vkmenu-t3.png` (server browser: "Servers", Internet/Favorites/History/Spectate/Lan/Friends tabs, server list) and `/tmp/vkmenu-t9.png` ("Loading...", "Verifying resources...", "Cancel", VAC notice) show crisp, correct text, matching the clean GL captures (`/tmp/glmenu-t3-0x1600015.png`). The earlier inconclusive `/tmp/gl21-*` vs `/tmp/vk21-*` menu comparison is superseded.

Ruled out during the investigation (do not revisit): the menu text path is `glDrawArrays(GL_QUADS)` with interleaved stride-16 client arrays; UVs and positions were correct; the `GL_TEXTURE` matrix (`0x1702`) is not used, so applying a texture matrix was not the fix.

`glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 2)` (`0x8573`) under `GL_COMBINE` was observed but is not implemented; it was not the cause of the garbled text, and text parity suggests it is low priority.

`vkskeleton.so` is still a research scaffold, not a playable renderer: offscreen FBO presentation, display lists, mipmaps, full `GL_COMBINE`/`GL_RGB_SCALE` semantics, border-clamp behavior, and many GL state combinations are not yet emulated. Filtering/wrap, alpha-test, the observed blend pairs, two-unit multitexture (surface + lightmap) and perspective-correct UVs are now emulated; world textures and studio models render correctly in-game, late HUD/text glyphs match GL closely, and menu/startup text renders correctly after the sub-image staging fix. `glDepthRange` and HUD colour arrays are emulated (viewmodel depth range not yet exercised with a weapon).

### World-brush findings

A separate world-path GL trace (`/tmp/gltrace-world.txt`, map `hl_t00`) confirmed that world brushes are **fixed-function immediate mode**, not indexed draws:

```text
5360  glBegin / glEnd
13072 glVertex3f
2419  glVertex3fv
17264 glTexCoord2f
11392 glVertex2f
206   glDrawArrays
0     glDrawElements
1     glFrustum
1274  glOrtho
71    glBindFramebufferEXT
```

The observed world projection was:

```text
0.5252  0       0        0
0       0.7002  0        0
0       0       -1.0002  -8.001
0       0       -1       0
```

This exposed three separate problems:

1. **Matrix-mode aliasing.** `GL_TEXTURE`/`GL_COLOR` matrix calls were sharing the modelview matrix/stack. Each mode now has its own matrix and stack; the current trace only uses `GL_PROJECTION` (`0x1701`) and `GL_MODELVIEW` (`0x1700`), but the separation is required for correct GL emulation.
2. **Near-plane clipping.** World quads straddle the camera plane. Rejecting vertices with `w <= 0` dropped whole triangles and blanked/corrupted the world. Immediate-mode triangles are now clipped in homogeneous clip space against the GL near plane (`cz + cw >= 0`) and far plane (`cw - cz >= 0`) before perspective division and the GL-to-Vulkan Y/depth conversion.
3. **Texture-slot exhaustion.** The original `MAX_VK_TEXTURES` of 512 was too small because world textures use GL IDs around `1000..1999+`. Those draws fell back to slot 0, the white default texture. The table is now 4096 slots with a direct `texture_slot_by_gl[8192]` lookup. Enabled world runs now log nonzero slots such as `curgl=1018 -> tex=1017` instead of `tex=0`, and no longer report capacity misses.

Late-map captures now show coherent, textured brush geometry instead of the earlier white/black streaked mess. It is not pixel-identical to the GL renderer yet, and the remaining GL state gaps still apply.

Temporary world/matrix/texture and HUD/text diagnostics were removed after verification; only structural init/draw/failure logs remain. The launcher remains unmodified after each opt-in test run.

## Performance notes: what MetaRenderer does, and what applies here

Reference only (`https://github.com/MetaHookSv/Renderer`, read at commit `e5854b6`); no code is taken from it.

How MetaRenderer gets its speed:

1. **No CPU/GPU stalls on streamed data.** `gl_ringbuffer.cpp`: one large persistently mapped buffer (`glBufferStorage` with `MAP_PERSISTENT|COHERENT`), allocated linearly each frame; a `glFenceSync` per frame marks the region in flight, and the CPU only waits (`glClientWaitSync`) if it laps a region the GPU has not finished.
2. **World geometry uploaded once per map.** `gl_wsurf.cpp`: BSP surfaces go into static VBO/EBO/VAO at load; each frame only issues indexed draws (`glDrawElements*`), batched by texture, with camera/scene data in UBOs.
3. **Models skinned on the GPU.** `gl_studio.cpp` + `studio_shader.vert.glsl`: static per-model vertex buffers with a packed bone index per vertex; bone matrices (`StudioUBO.bonematrix[]`, with a bone cache keyed by model/sequence/frame) are uploaded per entity and the vertex shader transforms.
4. **Batched HUD/2D** drawing and parallel shader prewarming (not relevant to Vulkan with prebuilt SPIR-V).

Items 2 and 3 need access to engine data structures (BSP surfaces, studio headers, bone transforms). An interposer only sees the GL call stream, so they are out of reach without hooking `hw.so` internals. What does transfer, ordered by expected gain for `vkskeleton.so`:

1. **Stop rendering every frame twice.** All draw wrappers (`glBegin`/`glVertex*`/`glEnd`/`glDrawArrays`, …) still forward to real OpenGL, so the GPU renders the full GL frame *and* the Vulkan frame; only the GL swap is skipped. Skipping the real GL draw calls while Vulkan presents should be the biggest win. Risk: anything that reads GL results back (`glReadPixels` screenshots, FBO-based effects) breaks, so keep state calls forwarded and gate it behind an env var first.
2. **Frames in flight + ring-buffered vertices** (MetaRenderer item 1). `end_frame_present()` waits on `render_fence` right after every submit, fully serialising CPU and GPU, and one vertex buffer is reused each frame. Use 2–3 frames, each with its own command buffer, fence, acquire/render semaphores and vertex-buffer region; wait only on the fence of the frame being reused.
3. **Batch texture uploads.** Every `glTexImage2D`/`glTexSubImage2D` does its own submit + fence wait (the font atlas alone is uploaded one glyph per call). Record copies into the frame's command buffer from a per-frame staging ring instead. This is the likely cause of the ~36–43 fps seen with the pause menu open versus 91 fps in game.
4. **Fewer pipeline/descriptor binds.** `record_frame_vertices()` rebinds the pipeline on every batch; bind only when blend/depth state changes.

### Implemented: items 1 and 2

- **No double rendering.** While Vulkan is presenting and framebuffer 0 is bound, `glBegin`/`glVertex*`/`glEnd` and `glDrawArrays` are not forwarded to OpenGL (`should_skip_gl_draw()`; the choice is latched per `glBegin` so a begin/end pair is never split). State calls, FBO-bound draws and frames Vulkan does not present still go to GL. `VKSKELETON_GL_PASSTHROUGH=1` restores the old behaviour (e.g. for GL screenshots).
- **Frames in flight.** `FRAMES_IN_FLIGHT = 2`: each slot has its own command buffer, fence, acquire semaphore and region of the (now 2× sized) vertex buffer; each swapchain image has a render-finished semaphore that present waits on. `begin_frame()` waits only on the fence of the slot it reuses; `end_frame_present()` no longer waits after submit. The command pool now sets `RESET_COMMAND_BUFFER_BIT` (required for re-recording).
- **In-flight texture safety.** Each texture records the serial of the last frame that sampled it; destroying a texture or rewriting its descriptor set (`glTexImage2D` re-create, filter/wrap change) first waits for in-flight frames if that serial has not completed.

Verified: `hl_t00` renders correctly (world, hologram model, HUD, MOTD dialog) in the play launcher and the capture harness. Not yet measured reliably: uncapped GL and Vulkan both hit the engine's 200 fps limit at 640×480 in `hl_t00`, so the gain needs a heavier map or higher resolution to show. Note: `+fps_max` on the command line is saved into `svencoop/config.cfg`; restore it after benchmarking.

### Window resize and presentation (Xwayland)

- **Resize:** `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` (acquire or present) or a size change seen by a check every 30 frames triggers `recreate_swapchain()`: device idle, destroy image views/depth/framebuffers, create the new swapchain with `oldSwapchain`, rebuild those resources. Viewport/scissor are dynamic state set per frame, so pipelines survive. A minimized (0×0) window skips frames until it has a size. Previously any out-of-date result stopped Vulkan permanently, leaving a black window. Built; not yet confirmed in-game.
- **Presentation stall:** measured on `crimecity2` online (KDE Wayland → Xwayland, 1280×960, 42–48 fps): sampling the game thread's `/proc` state showed ~57% running, ~28% blocked in `poll()` (Mesa's X11 WSI presenting an `IMMEDIATE` swapchain on the caller's thread, waiting for Xwayland), ~10% in `drm_syncobj` fence waits; GPU ~30% busy. The swapchain now prefers `MAILBOX` (Mesa presents it from its own thread; no tearing, no vsync cap), then `IMMEDIATE`, then `FIFO`, with `minImageCount + 2` images. The chosen mode is logged (`present mode ...`). Override: `VKSKELETON_PRESENT_MODE=mailbox|immediate|fifo ./play_vulkan.sh` (passed through to the game by the launcher). Not yet measured in-game.

### Model-heavy / dynamically lit scenes: upload batching and per-vertex cost

Live sample on `crimecity2` (neon-lit street, ~48,800 model polys): 7–12 fps (126 ms), GPU ~25% busy, game thread mostly running or in `drm_syncobj` fence waits. Two causes, both fixed (built; not yet measured in-game):

- **Texture uploads stalled the frame.** Dynamic lights make the engine re-upload lightmaps with `glTexSubImage2D` every frame. Each upload was its own submit + fence wait, and since it queued behind the frames in flight, every wait also waited for those frames. Uploads are now recorded into a per-frame-slot command buffer (`upload_cmds[cur_frame]`) using that slot's 8 MiB staging region (`UPLOAD_SLOT_BYTES`, buffer is 2× that) and submitted in the same `vkQueueSubmit` as the frame, before its draws. Image barriers (fragment-read → transfer-write) order them after earlier frames' sampling. A synchronous flush happens only when the staging region fills, when a texture with pending copies is destroyed, or when Vulkan does not present the frame.
- **Per-vertex CPU work.** Render state (texture slots, blend, depth, alpha test, texenv scale) was recomputed for every vertex, and each triangle re-transformed its vertices, so strip vertices were transformed up to 3×. State is now resolved once per `glBegin`/`glDrawArrays` (`compute_draw_state()`; GL forbids state changes inside begin/end), every vertex is transformed to clip space once (`clip_verts[]`), and triangles entirely inside the near/far planes skip the Sutherland–Hodgman clipper.
- **Disappearing HUD/models/brush entities in heavy views.** `FRAME_MAX_VERTICES` (262,144) was a hard per-frame cap; past it `emit_tri()` silently dropped triangles. GoldSrc draws world → entities → HUD, so overflow removed models, brush entities and the HUD while the world stayed. Raised to 786,432 (≈51 MiB CPU array + 2 × 51 MiB host-visible vertex buffer), and an overflow now logs `frame vertex buffer full` (up to 4 times). If that line appears, the next step is flushing mid-frame instead of a bigger cap.

### Fewer wasted calls and copies (CPU path)

- **Per-vertex GL calls are no longer forwarded inside skipped draws.** GL draws were already skipped while Vulkan presents, but every `glColor*`/`glTexCoord2f`/`glMultiTexCoord*` inside them still went to Mesa, which queues them to its `gl0` worker thread for nothing. Inside a skipped `glBegin`/`glEnd` they are now dropped; at the skipped `glEnd` the final colour, texcoord and unit-1 texcoord are sent once so GL's current state stays what the game expects.
- **Vertices are written straight into the mapped GPU buffer.** `frame_vertices` now points at the current frame slot's region of the host-visible vertex buffer (`frame_slot_begin()` waits for that slot's fence on the frame's first triangle). The 51 MiB CPU staging array and the per-frame `memcpy` are gone; each triangle is one sequential 168-byte write.
- **Batching moved to a CPU-side list.** Texture slots, blend and depth mode are tracked in `frame_batches[]` as triangles are emitted (never read back from GPU-visible memory), so `vk_vertex_t` shrank from 68 to 56 bytes (`_Static_assert`-checked layout: pos@0, uv@16, colour@24, alpha-test@40, uv2@48).

Built; not yet measured in-game.

### Vertex transform and clipping on the GPU

The CPU no longer transforms or clips vertices. Each draw (`glEnd` / `glDrawArrays`) computes `Fix · Projection · ModelView` once, where `Fix` folds in the GL→Vulkan conversion: `y' = −y`, `z' = n·w + (f−n)·(z+w)/2` (GL clip z ∈ [−w, w] → `glDepthRange` [n, f] inside Vulkan's [0, w]). The matrix goes into a per-frame table (`frame_mats[]`, de-duplicated against the previous draw, since world and studio models share one matrix in GoldSrc), batches split on it, and recording issues `vkCmdPushConstants` (64 bytes, vertex stage) only when it changes. The vertex shader (`gen_shaders.py`, push-constant block with a column-major `mat4`, validated with `spirv-val`) computes `gl_Position = mvp · vec4(pos, 1)`; the GPU's clipper replaces the CPU Sutherland–Hodgman code (kept, marked unused). `vk_vertex_t` is now 52 bytes with an untransformed `vec3` position.

Offline checks: layout `_Static_assert`ed (pos@0, uv@12, colour@20, alpha-test@36, uv2@44); the folded matrix matches the old CPU path to 3e-7 relative error over 100,000 random matrices/vertices/depth ranges. Known difference: with a compressed `glDepthRange` (viewmodel), geometry GL would clip at its near/far planes but that still lands inside Vulkan's [0, w] is no longer clipped. Built; not yet run in-game.

### Profile-driven fixes (perf on crimecity2)

`perf record -F 499 -g -t <game thread>` for 10 s on the GPU-transform build (needs `kernel.perf_event_paranoid=1`): game thread ≈ 50% `vkskeleton` (31% `emit_tri`), 18% `hw.so` (engine), 14% kernel (≈9% syscalls, ≈3.6% `amdgpu_gem_fault` page faults), 7% Mesa GL (`libgallium`), 5% RADV.

- **x87 → SSE2.** `-m32 -O2` defaults to the x87 FPU; ~60% of `emit_tri`'s samples were `flds`/`fstps` copying vertex floats one by one, plus 16% `rep movsl`. `build_vkskeleton.sh` now uses `-msse2 -mfpmath=sse`, and `emit_tri` copies each pending vertex with one 52-byte `memcpy` (`im_vertex_t` begins with `vk_vertex_t`'s exact layout, enforced by `_Static_assert`). Disassembly: `emit_tri` has 0 x87 and 0 `rep movs` instructions; the library has none at all.
- **Texture sub-updates no longer duplicated into GL.** Every `glTexSubImage2D` (per-frame lightmaps) was still forwarded to Mesa, which allocates and maps an upload buffer per call (the page faults, ioctls and gallium time). While Vulkan presents, updates Vulkan handled are no longer forwarded; anything Vulkan did not take still is, and `VKSKELETON_GL_PASSTHROUGH=1` forwards everything.

Built; not yet profiled in-game.

### White models on big maps (texture slot exhaustion)

On `bts_rc` (after `crimecity2` in the same session) the log showed `texture slot exhausted GL=4990…`: the slot table (4096) never released anything because `glDeleteTextures` was not intercepted, so later textures fell back to the white default. Now `glDeleteTextures` destroys the Vulkan image, clears the GL-id mapping and unit bindings, and returns the slot to a free list; reused slots keep their descriptor set (the pool never frees sets). Limits raised to 8192 slots / GL ids < 65536 for direct lookup. Recording falls back to the white slot if a batch references a texture deleted earlier in the same frame (its descriptor would point at a destroyed view). Built; not yet run in-game.

Mirrors (e.g. on `bts_rc`) still do not work: they render the scene a second time into an offscreen target (FBO or framebuffer copy) that the Vulkan path does not emulate.

### Mirrors: framebuffer → texture copies

`VKSKELETON_DEBUG=1` now logs each distinct offscreen/mirror-related call once (`dbg mirror f=<frame> fb=<fbo> …`): FBO binds/attachments, `glCopyTex(Sub)Image2D`, `glClipPlane`, stencil, `glColorMask`, `glClear` masks, depth func/mask, cull face, viewport. `play_vulkan.sh` passes `VKSKELETON_DEBUG=1` through.

Log on a mirror map (1280×960) when the mirror came into view: no FBOs at all (`fb=0`). The engine enables `GL_CLIP_PLANE0`, clears depth mid-frame, renders the mirrored view to the screen, **copies the whole screen into textures 90 and 91 with `glCopyTexSubImage2D`**, then uses stencil (`glClear` stencil, `StencilFunc ALWAYS ref 3` + `StencilOp REPLACE` under `ColorMask 0000`, then `EQUAL/LEQUAL ref 1`) and renders the normal view. The mirror was white because the copy was not emulated.

Implemented (built; not yet run in-game):
- **Ordered frame command list** (`frame_cmds[]`: draw batch / clear / copy) so these happen in the game's order; batches only merge across consecutive draws.
- **`glCopyTexSubImage2D` / `glCopyTexImage2D`** → at that point the render pass ends, the swapchain image (now created with `TRANSFER_SRC` when supported) is blitted into the texture with the rows flipped (GL framebuffer is bottom-up; textures keep GL row order), and drawing resumes in `render_pass_load` (same attachments, `LOAD` for colour/depth/stencil; depth/stencil are now stored). Barriers cover the swapchain image, the texture and the depth/stencil buffer. GL's own copy is skipped while Vulkan presents.
- **Mid-frame `glClear`** → `vkCmdClearAttachments` (colour/depth/stencil per mask); the frame-start clear stays in the render pass.
- **`glColorMask(0,0,0,0)` draws are skipped** (stencil-marking pass).

Not yet emulated: `glClipPlane` (geometry between the camera and the mirror plane can show up in the reflection) and stencil tests (stencil-restricted draws are drawn unrestricted).
- **Why the mirror stayed white after the copy emulation:** extra debug lines (`copy OK/REJECT`, storage-texture creation, draws that sample a copy target) showed `copy REJECT tex=1679 … image=0`. The engine allocates the mirror copy targets as `glTexImage2D(1280×960, GL_RGBA, GL_BYTE, NULL)`, and the texture path only accepted `GL_UNSIGNED_BYTE`, so no Vulkan image existed to copy into. Storage-only textures (`pixels == NULL`) are now created regardless of the declared type. Built; not yet confirmed in-game.
- **Stencil, depth mask, depth func and colour mask are now emulated.** With the copy working, the log showed the mirror is finished by (1) a 4-vertex polygon drawn with `ColorMask 0000` and `StencilFunc ALWAYS ref 3 / StencilOp REPLACE` marking the mirror's outline, then (2) a quad with the copied texture (`GL_REPLACE`, UV 0..1) drawn under `StencilFunc EQUAL/LEQUAL ref 1`. Without stencil the reflection covered the whole screen. Pipelines are now created lazily per state key (`pipeline_key()`: blend, depth test/write/compare, colour write, stencil test/compare/sfail/zfail/zpass; GL compare enums map 1:1 to `VkCompareOp`) and cached (`get_pipeline()`); the stencil reference is dynamic state set per batch. `glEnable(GL_STENCIL_TEST)`, `glStencilFunc`, `glStencilOp`, `glDepthMask` and `glDepthFunc` are tracked; colour-masked draws are now real draws with colour writes off instead of being skipped. Side effect: `glDepthMask(0)` and `GL_LEQUAL` now behave as in GL everywhere (previously every pipeline used `LESS` and always wrote depth). Built; not yet confirmed in-game. `glClipPlane` is still not emulated.
- **Mirror confirmed working in-game** (reflection shows the room and the player model). Follow-up reports: wall sprites/decals z-fighting and props flashing. `glPolygonOffset` (imported by `hw.so`, used for decals) was not emulated; it now maps to Vulkan depth bias: key bit 21 (`GL_POLYGON_OFFSET_FILL` with depth test on) enables `depthBiasEnable`, and `factor`/`units` are dynamic state (`vkCmdSetDepthBias(units, 0, factor)`) stored per batch. Built; not yet confirmed. Open: models sometimes cut in half (cause not yet identified).
- **`glClipPlane` emulated (models cut in half in the mirror).** The mirror view enables `GL_CLIP_PLANE0` to cut away everything behind the mirror plane (mainly the wall the mirror is on, which sits right in front of the mirrored camera). Without it that geometry covered and sliced the reflection. Now: `glClipPlane` stores the plane in eye space (`eq · inverse(modelview)`, as GL specifies); each draw converts it to its object space (`plane_eye · modelview`) and pushes it with the matrix (push constants now 80 bytes: `mat4 mvp` + `vec4 clip`); the vertex shader writes `gl_ClipDistance[0] = dot(clip, vec4(pos, 1))` (SPIR-V `ClipDistance` capability, validated). Disabled = `(0,0,0,1)`. The device enables `shaderClipDistance`. Offline check with rigid modelviews (±4000-unit translations): matches GL's `eq · v` to ~1e-7 relative over 100,000 cases. Built; not yet confirmed in-game.

### "Vulkan" in Options → Video → Renderer

The Renderer dropdown is the localization token `#GameUI_OpenGL` (in `gameui.so`), defined in `svencoop/resource/gameui_english.txt` (UTF-16LE, BOM, CRLF; only English ships). While a `play_vulkan.sh` session runs, `make_vk_gameui.py` writes a copy with only that value changed to `"Vulkan"` into `svencoop_addon/resource/gameui_english.txt`, which overrides the base game. Created files are listed in `svencoop_addon/resource/.vkskeleton-temporary`; they are removed on exit (and leftovers from a hard-killed run on the next start), plus the folder if it ends up empty. A user's own file there is never overwritten; normal OpenGL launches show "OpenGL". Offline check: exactly one of 270 lines differs, encoding and line endings preserved. Not yet seen in-game. (An earlier main-menu label variant was dropped.)

### Freecam (I)

During Vulkan sessions **I** toggles a free camera (first version used F5). The engine gets its view from `client.so`'s `V_CalcRefdef` (looked up by name with `dlsym`, which `substitute()` intercepts); `vk_V_CalcRefdef()` lets the game compute its view, then overwrites `vieworg`, `viewangles` and `forward/right/up` in `ref_params_t` (HL SDK layout: floats 0–2, 3–5, 6–14; `frametime` at 15). The engine renders and culls from that origin, so the world draws correctly; entities are still limited to what the server sends for the player's position.

- **Keys** are read each frame (edge-detected toggle on I; W/A/S/D fly along the view, Space/Left Ctrl up/down, Left Shift ×3 speed, 400 → 1200 units/s). In-game both hooks attached (`V_CalcRefdef`, `CL_CreateMove`) but neither the `SDL_PollEvent` hook nor `SDL_GetKeyboardState` saw a key press. The process has the game's `libSDL2-2.0.so.0` plus `/usr/lib/i386-linux-gnu/libSDL3.so`, i.e. most likely sdl2-compat on SDL3. Keys are now taken from SDL **or** the X11 keymap of the game's own display (`XQueryKeymap` + `XKeysymToKeycode`); the log says once whether a press was seen `via SDL` or `via X11`. **Confirmed in-game:** presses arrive via X11 (`key press seen via X11`, then `freecam on/off`); SDL never reported a key. Keys also reach the game (`I` = `inventory` by default; `unbind i` if needed).
- **Player frozen** by wrapping `client.so`'s `CL_CreateMove` (also fetched by name): while active, `forwardmove`/`sidemove`/`upmove` (usercmd_t offsets 16/20/24 on i386) are zeroed and the JUMP/DUCK/FORWARD/BACK/MOVELEFT/MOVERIGHT bits cleared from `buttons` (offset 30).
- **Looking** uses the game's own `viewangles` (the mouse still turns the player). Reading `SDL_GetRelativeMouseState` directly picked up the game's cursor re-centring warps and pinned the camera looking up; removed.
- The log reports `freecam: V_CalcRefdef hook active` and `CL_CreateMove hook active` once each, `key press seen via X11/SDL` once, and `freecam on/off` on each toggle. Working in-game (toggle on I, flying).
