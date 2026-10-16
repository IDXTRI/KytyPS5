# Marvel's Wolverine on KytyPS5: Wolverine build v2 (test release)

An experimental Windows build of the [KytyPS5](https://github.com/KytyPS5/KytyPS5) PS5 emulator,
tuned for **Marvel's Wolverine** (PPSA03671, game version **01.001.005**).

This is a **test release** for people who want to try the game on PC and help find problems. It
plays through the opening areas, but expect low frame rates, visual glitches and the occasional
crash. **Save often.**

**No game files, keys or firmware are included.** You need your own dump of the game.

> **Test build.** This is a snapshot of the work so far, published for testing. It is not a
> finished release, and **no further v2 updates are planned for now**: the next version waits
> for Senaxx's new KytyPS5 build (a reworked render path), which this work will move onto.


---

## Contents

1. [Requirements](#1-requirements)
2. [Quick start (recommended): Play Wolverine.bat](#2-quick-start-recommended-play-wolverinebat)
3. [Alternative: the launcher](#3-alternative-the-launcher)
4. [Recommended and not recommended](#4-recommended-and-not-recommended)
5. [What to expect (performance)](#5-what-to-expect-performance)
6. [Known issues](#6-known-issues)
7. [Troubleshooting](#7-troubleshooting)
8. [Where your files go](#8-where-your-files-go)
9. [What this build changes compared with KytyPS5](#9-what-this-build-changes-compared-with-kytyps5)
10. [Advanced: switches](#10-advanced-switches)
11. [Reporting a problem](#11-reporting-a-problem)
12. [Source code, licenses and credits](#12-source-code-licenses-and-credits)

---

## 1. Requirements

| | Minimum | Tested |
|---|---|---|
| OS | Windows 10 or 11, 64-bit | Windows 11 |
| CPU | Recent 6-core or better | AMD Ryzen 7 7800X3D |
| GPU | Vulkan 1.3, 8 GB VRAM | NVIDIA RTX 4070 Ti, 12 GB |
| RAM | 16 GB | 32 GB DDR5 |
| Disk | ~1 GB free for the shader cache and saves | |
| Game | Your own dump of Marvel's Wolverine **01.001.005** (the folder with `eboot.bin`) | |

- **12 GB of VRAM or more is recommended.** The game uses about 10-11 GB here; with less, part
  of it spills into system memory and the game runs slower.
- Keep your GPU driver up to date. Only NVIDIA was tested. AMD and Intel GPUs may work but are
  untested.

## 2. Quick start (recommended): Play Wolverine.bat

1. Extract the whole archive to a folder. Keep the `emulator` folder next to
   `Play Wolverine.bat`. A short path without special characters is safest (for example
   `D:\Wolverine-KytyPS5`).
2. Double-click **`Play Wolverine.bat`**.
3. The first time, it asks for your game folder. Paste the **full path of the folder that
   contains `eboot.bin`** (for example `D:\Games\PPSA03671-app0`) and press Enter. It is saved in
   `game_path.txt`; delete that file to choose another folder.
4. Wait. **The first launch is slow**: the emulator translates and compiles thousands of game
   shaders. The first load can take several minutes and the game stutters whenever it shows
   something new. This gets much better as the cache fills, and later launches are faster.
5. When you close the game, the black window says whether it closed normally or shows the path
   of the log file.

The .bat file already uses the recommended settings, and
writes nothing into the game folder.

## 3. Alternative: the launcher

1. Open the `emulator` folder and double-click **`launcher.exe`**. Start it from that folder: it
   reads the recommended settings from `emulator\Kyty.ini` there.
2. Open the settings (Global settings) and, under **Game folders**, add the folder that
   **contains** your game folder (for example `D:\Games`, not `D:\Games\PPSA03671-app0`).
3. Check the settings against the table in section 4. Leave **"AMD CPU patch"** off (see the
   table). Press **Save**.
4. Select Marvel's Wolverine in the list and press **Run**. A console window opens next to the
   game; leave it open.

The launcher keeps its own saves and cache in the `emulator` folder (see section 8). Both ways
give the same performance once each has its own shader cache.

## 4. Recommended and not recommended

### Settings (launcher names; the .bat sets these for you)

| Setting | Use | Why |
|---|---|---|
| Bindless textures | **On** | Required: the game picks its textures on the GPU. |
| Enable tessellation support | **On** | Required for some terrain and character surfaces. |
| Enable readback | **On** | Required: the game reads back images it renders. |
| Windows SysV red zone crash protection | **On** | Prevents a class of crashes. |
| AMD CPU patch | **Off** | Despite its name it replaces the game's approximate `VRSQRTPS` with an exact value, one CPU exception per use (slow). The PS5's CPU is AMD, so AMD PCs already match the console. Try it only if animation or physics look wrong, most likely on Intel. |
| Skip shaders | **Keep the preset list** (10 entries) | Skips expensive effects (one is ray tracing) and four shaders that draw visual errors (white planes and wedges, an orange screen flood in fights). Without it the game runs about 25-30 % slower. |
| Screen resolution | 1280x720 (default) | Tested setting. Higher values are untested. |
| Present mode | Mailbox | Tested setting. |
| Vulkan validation, Shader validation, RenderDoc, Command buffer dump, Tracy | **Off** | Debugging tools: they make the game much slower. |
| Shader logging, Printf output | **Silent** | Logging to files slows the game down. |

### Skipped shaders

Eight of the game's shaders are skipped: their draws are left out. The first six were picked by
Mac (from the KytyPS5 Wolverine work) because they cost a lot of frame time or misbehave; one of
them is a ray-tracing shader. Skipping them makes the game run about 25-30 % faster, and the
effects they draw are missing (most notably ray-traced effects). The last two (new in v2) draw a
solid white plane over one room in the town buildings; nothing visible is missing without them.
Both start methods use this list:

```
bad108e74fb72e9f,4e7f2c6bb9b158a1,8bfd230b9cd875a2,c4df2a00067e0666,dc76e1223a9bf673,e6d76d24f59f8015,4c582eb7319682a3,be2a9db103759bff,d6ec1c4ba883d308,4d2a3a54c74938ad
```

- **Launcher:** it is in Global settings, **Skip shaders**. If you cleared it, paste the line above
  back in and press Save.
- **Play Wolverine.bat:** it is the `set "SKIP=--skip-shaders ..."` line. To run without it (for
  testing), put `rem ` in front of that line.

Separately, any other shader that uses ray tracing (BVH instructions) is skipped automatically;
`KYTY_SKIP_BVH_DISPATCHES=0` turns that off (section 10).

### Do

- **Save often**, especially before long sessions.
- **Let the first session run** through the stutter; the cache fixes most of it.
- **Keep the game window as it is** after it opens.

### Don't

- **Don't resize, maximize or minimize the game window** while it runs: the emulator can lose
  the GPU device and close. (Fullscreen is untested.)
- **Don't copy someone else's shader cache.** It only works with the same GPU model and driver,
  and it is built from game files.
- **Don't put the emulator inside the game folder.**
- **Don't run two copies at the same time.**

## 5. What to expect (performance)

Measured on a Ryzen 7 7800X3D + RTX 4070 Ti at 1280x720, after the cache had filled:

| Situation | Frame rate |
|---|---|
| Main menu | ~33-34 fps |
| Standing still, open areas | 25-30 fps |
| The "Leap Attack" prompt at the start of a new game (busy jungle) | ~14-15 fps |
| Dense jungle with many plants | ~12-15 fps |
| Running and turning | ~12-20 fps, with occasional stutter |
| Fights | lower; varies a lot |

The emulator's own CPU work limits the frame rate in busy scenes (the GPU is often half idle), so a
faster GPU helps less than you might expect.

## 6. Known issues

| Issue | Notes |
|---|---|
| **Crash when the window is resized or minimized** | Don't touch the window during play. |
| **GPU crash ("device lost")**, at random, more likely when video memory is full (heavy areas, long sessions) and when loading another save | Save often; to change saves, quit and restart the game. Cause known in part (a texture read after it was freed); please report it with the log (section 11). |
| **Detail flickering**: ground/wall detail and decals (blood, tyre tracks) present on one frame and gone the next | Known since the first build. Narrowed to two ground-material shaders; not fixed yet. |
| **Cutscenes run at 1-4 fps** | Known; the emulator waits on the GPU there. |
| Fights run at about 10-12 fps | See section 5. |
| Short purple or magenta colours on some surfaces | Seen now and then; they go back to normal. |
| Textures missing for a moment after spawning or loading | They appear after a few seconds. |
| Stutter when new things appear, especially on the first launch | The shader cache fills over time. |
| Ray-traced effects are missing | Those shaders are skipped on purpose (see section 10). |
| A character's face can render black in some cutscenes | Seen once; please report where. |
| Low frame rate in dense foliage | See section 5. |

Only the opening areas of the game have been tested. Later parts may not work.

## 7. Troubleshooting

| Problem | Try |
|---|---|
| The .bat says `eboot.bin was not found` | Give the folder that directly contains `eboot.bin`. Avoid trailing spaces. |
| Nothing happens / the window closes at once | Run the .bat from the extracted folder (not from inside the zip). Update your GPU driver. Check the log in `userdata\logs`. |
| Very long first load | Normal on the first launch (shader compilation). Wait several minutes. |
| The game froze or closed with a device lost error | Restart and load your last save. Report it with the log if it repeats. |
| Much lower frame rate than in section 5 | Check that the Skip shaders list and the settings in section 4 are in place, and that no debugging option is on. |
| "Press touchpad" prompt does nothing | Xbox pad: press **View** (the small button left of centre). Keyboard: **Backspace** or **Tab**. |

## 8. Where your files go

| | Started with Play Wolverine.bat | Started with the launcher |
|---|---|---|
| Saves | `userdata\_SaveData` | `emulator\_SaveData` |
| Shader cache | `userdata\_PipelineCache` | `emulator\_PipelineCache` |
| Logs | `userdata\logs` | console window only |

- Nothing is ever written into the game folder.
- To move your saves between the two ways of starting, **copy** the `_SaveData` folder
  (keep a backup first).
- When you update to a newer build of this release, keep your `userdata` folder (or the launcher's
  `_SaveData` and `_PipelineCache`): saves and the shader cache carry over.

## 9. What this build changes compared with KytyPS5

### New in v2

- **The game's own texture samplers**: the moon is a single moon again, and ground and trees
  keep their detail. Shaders that lost their draws with them (Wolverine's suit, tree trunks,
  title-screen logs) draw again.
- **White plane in a town room**, **white wedge on a rooftop** and **orange screen floods in fights** removed (four shaders skipped, see section 4).
- **Unused textures are released** (`KYTY_BINDLESS_EVICT=1`): video memory no longer overflows into system memory in long sessions (before: 4 fps after a while). Freed memory is held back up to 2 GiB as a safety net.
- **F9** saves the last 40 frames into `userdata\framedump`, to attach to a bug report.
- **Engine settings without touching the game**: files in `userdata\overrides` replace the game's file of the same name (for example `commandline.txt`).
- A garbage mesh draw is skipped instead of closing the emulator.
- **Shader disk cache**: translated shaders are kept on disk, so the second launch stutters
  much less.
- **Video memory**: budget-aware texture pool and a safety net for deleted images
  (`KYTY_VRAM_SAVER=2`).
- **Fixes from KytyPS5 (October 2026)**: loading a save from the in-game menu no longer
  crashes; texture views and 2D/3D aliases, blending on swizzled targets, more shader
  instructions; mesh-shader triangle indices (edge flags); Senaxx's later Wolverine fixes.
- Main menu ~33-34 fps (v1: ~30).

### Since v1 and earlier

This build is KytyPS5 (release 2026-10-03, f53e5d2) merged with the Marvel's Wolverine work from
KytyPS5 PR #937 and further fixes:

- **Lower VRAM use**: a two-level GPU page table for shader memory access (about 700 MB less),
  texture memory recycling, and fixes that keep the game inside a 12 GB card's budget (about
  10-11 GB instead of 12.7 GB).
- **Fewer crashes**: GPU fault handling and repair of guest memory protection, safer texture
  memory reuse.
- **Streaming textures**: a texture slot whose texture the game replaces is updated, and the old
  texture is released (from Senaxx's work).
- **Wolverine-specific fixes** on top of the new KytyPS5 code (texture selection, mesh drawing,
  gather sampling) so the game boots and renders.
- **Controller**: the Xbox View button works as the PS5 touchpad.
- **Shader cache**: kept across updates of this build.
- **Defaults for Wolverine**: the settings it needs are on by default.

## 10. Advanced: switches

Environment variables, set in `Play Wolverine.bat` (add a line `set NAME=value` before the line
that starts the emulator). Leave them alone unless you are testing.

| Switch | Default | Effect |
|---|---|---|
| `KYTY_SKIP_BVH_DISPATCHES` | 1 | 0 runs the ray-tracing shaders. **Experimental: can crash the GPU.** |
| `KYTY_BINDLESS_SAMPLERS` | 1 | 0 samples the game's textures with one generic sampler (v1 behaviour: tiled moon, stretched light streaks). |
| `KYTY_PROGRAM_DISK_CACHE` | 1 | 0 stops keeping translated shaders on disk (more stutter on every launch). |
| `KYTY_VRAM_SAVER` | 2 | 0 or 1: less aggressive video-memory saving. |
| `KYTY_BINDLESS_REVALIDATE` | 1 | 0 stops re-checking textures the game replaces. |
| `KYTY_LABELS_AFTER_GPU` | 1 | 0 uses KytyPS5's original GPU synchronization (not recommended). |
| `KYTY_PIPELINE_CACHE_ANY_REVISION` | 1 | 0 discards the shader cache whenever the build changes. |
| `KYTY_FUNCTION_ARRAY_SHRINK` | 0 | 1 shrinks per-thread shader arrays (from Jetsku's fork). No gain measured in this game; more stutter. |
| `KYTY_MEMORY_STATS` | 0 | 1 writes memory statistics to the log every 5 seconds. |

## 11. Reporting a problem

Please include:

- What happened and where in the game (a screenshot helps).
- Your GPU, its driver version, your CPU and RAM.
- How you started the game (.bat or launcher) and any settings you changed.
- The log file from `userdata\logs` (.bat), or a screenshot or copy of the console window
  (launcher). Logs contain no personal data except folder paths.

## 12. Source code, licenses and credits

- **Source code:** built from the Wolverine branch of KytyPS5. KytyPS5 is licensed under the
  **GNU GPL 2.0** (see `LICENSE-GPL-2.0.txt`, and `LICENSE-Kyty-MIT.txt` for the original Kyty
  code). The source of this build: https://github.com/IDXTRI/KytyPS5/tree/wolverine-v2 (tag `wolverine-v2`). Third-party
  licenses are in `emulator\licenses`.
- **Credits:**
  - **KytyPS5** and all its contributors: the emulator.
  - **Mac (itsmemac), Senaxx and Ali Almohaya**: the Marvel's Wolverine work in KytyPS5 PR #937
    that this build is based on, and Senaxx's later Wolverine fixes.
  - **Jetsku**: the SPIR-V Function-array shrink pass (optional, see section 10).
  - **DXTR**: this build.

Marvel's Wolverine is a trademark of its owners. This project is not affiliated with Sony,
Insomniac Games or Marvel. Use only a copy of the game you own.
