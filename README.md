# vkskeleton — experimental Vulkan renderer for Sven Co-op (Linux)

`vkskeleton` replaces Sven Co-op's OpenGL rendering with Vulkan on Linux,
without engine source code. It is a preloaded library (`LD_PRELOAD`) that
intercepts the engine's OpenGL 1.x calls, emulates the fixed-function state it
uses and draws the frame with Vulkan on the game's own window.

> **Status: experimental.** It runs real maps (world, lightmaps, models, HUD,
> menus, mirrors) and has been played on, but it is not a complete OpenGL
> implementation. Developed and tested on one machine: AMD RX 580, Mesa RADV,
> Debian, KDE Plasma on Wayland (game via Xwayland).

> **⚠️ VAC warning.** A library that hooks OpenGL with `LD_PRELOAD` looks like a
> classic GoldSrc cheat to anti-cheat. Joining VAC-secured servers with it loaded
> can get your account VAC-banned for the game, and VAC bans cannot be appealed.
> Use it offline or on servers you trust, at your own risk.

Not affiliated with or endorsed by the Sven Co-op team or Valve.

## Features

- **World**: textures + lightmaps (two texture units), perspective-correct
  mapping, alpha test, blending and depth/stencil state, emulating the
  fixed-function subset the engine uses.
- **Models, sprites, HUD, menus, console text.**
- **Mirrors**: framebuffer→texture copies (`glCopyTexSubImage2D`), mid-frame
  clears, stencil, user clip planes.
- **Decals** without z-fighting (`glPolygonOffset` → depth bias).
- **Performance**: no duplicate GL rendering or GL state forwarding, two
  frames in flight, batched texture uploads, vertex transform and clipping on
  the GPU, indexed drawing, cached draw state, keyed pipeline cache,
  SSE2/`-O3` build, render thread.
- **Window resize** (swapchain rebuild), `MAILBOX` presentation when available.
- **Freecam**: press **I** to fly the camera (see *Controls*).
- Options → Video → Renderer reads **"Vulkan"** during Vulkan sessions.

## Requirements

- Linux, Sven Co-op (Steam, Linux build), Steam running.
- A Vulkan driver with 32-bit support (e.g. `mesa-vulkan-drivers:i386`).
- Build tools: `gcc` and `binutils` with `-m32` support, `python3`,
  Vulkan and X11 headers (`libvulkan-dev`, `libx11-dev`), and the 32-bit
  runtime libraries `libc6:i386` and `libx11-6:i386`.
- Optional: `spirv-tools` (`spirv-val`) to validate the generated shaders.
  No shader compiler is needed; SPIR-V is emitted by `tools/gen_shaders.py`.

## Build

```sh
./build.sh          # -> build/vkskeleton.so
```

## Play

```sh
./play_vulkan.sh                 # start Sven Co-op with the Vulkan renderer
./play_vulkan.sh +map hl_t00     # extra arguments go to the game
```

The script finds the game in the usual Steam locations; otherwise set
`SVEN_DIR=/path/to/Sven Co-op`. It waits until you quit the game.

While the game runs it temporarily:

- adds the preload to the game's `svencoop.sh` (original saved as
  `svencoop.sh.vkskeleton-orig` next to it), and
- places a copy of `gameui_english.txt` in `svencoop_addon/resource/` that
  relabels the Renderer entry.

Both are undone when the game exits or on Ctrl-C; a run that was killed hard is
repaired on the next start. Starting Sven Co-op normally from Steam runs the
stock OpenGL renderer.

### Environment variables

| Variable | Effect |
|---|---|
| `SVEN_DIR` | game folder, if not found automatically |
| `VKSKELETON_PRESENT_MODE` | `mailbox`, `immediate` or `fifo` (default: best available, `mailbox` preferred) |
| `VKSKELETON_GL_PASSTHROUGH=1` | also send draws to OpenGL (slower; for debugging) |
| `VKSKELETON_STUDIO=0` | draw all models through the engine (disables the model fast path) |
| `VKSKELETON_GPU_SKIN=0` | skin and light fast-path models on the CPU instead of the GPU |
| `VKSKELETON_RENDER_THREAD=0` | record/submit/present Vulkan frames on the game thread instead of a separate render thread (on by default) |
| `VKSKELETON_DEBUG=1` | extra one-shot logging of GL state events |

Logs: `/tmp/vkplay.log.<pid>` (renderer) and `/tmp/vk-play-stdout.log` (game).

## Controls (freecam)

| Key | Action |
|---|---|
| **I** | toggle freecam |
| W / A / S / D | fly along the view |
| Space / Ctrl | up / down |
| Shift | 3× speed |
| Mouse | look (turns the player too) |

The engine renders and culls from the freecam position, so the world draws
correctly; distant entities may be missing because the server only sends what
is near your player. `I` is bound to `inventory` by default (`unbind i` if it
gets in the way).

## Known limitations

- Not emulated: fog, rendering into framebuffer objects, display lists, mipmaps,
  full `GL_COMBINE` semantics, and GL state combinations the engine was not seen
  to use.
- Models are skinned and lit on the GPU when they pass the fast-path checks;
  chrome, additive or masked textures, render effects and models with separate
  texture files still go through the engine.

## How it works / development notes

See [docs/NOTES.md](docs/NOTES.md) for the research log: GL call traces,
measurements, profiling results and the reasoning behind each fix.
The MetaHookSv Renderer project (<https://github.com/MetaHookSv/Renderer>)
was read as an architecture reference only; no code was taken from it.

## License

[MIT](LICENSE)
