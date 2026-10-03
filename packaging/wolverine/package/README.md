# Marvel's Wolverine on KytyPS5: Wolverine build v1 (test release)

An experimental Windows build of the [KytyPS5](https://github.com/KytyPS5/KytyPS5) PS5 emulator,
tuned for **Marvel's Wolverine** (PPSA03671, game version **01.001.005**).

This is a **test release** for people who want to try the game on PC and help find problems. It
plays through the opening areas, but expect low frame rates, visual glitches and the occasional
crash. **Save often.**

**No game files, keys or firmware are included.** You need your own dump of the game.

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

The .bat file already uses the recommended settings, detects AMD processors on its own, and
writes nothing into the game folder.

## 3. Alternative: the launcher

1. Open the `emulator` folder and double-click **`launcher.exe`**. Start it from that folder: it
   reads the recommended settings from `emulator\Kyty.ini` there.
2. Open the settings (Global settings) and, under **Game folders**, add the folder that
   **contains** your game folder (for example `D:\Games`, not `D:\Games\PPSA03671-app0`).
3. Check the settings against the table in section 4. On an **AMD processor, tick
   "AMD CPU patch"** (the launcher cannot detect it). Press **Save**.
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
| AMD CPU patch | **On for AMD processors**, off for Intel | Fixes an instruction AMD CPUs compute differently. |
| Skip shaders | **Keep the preset list** (6 entries) | Skips expensive effects (one is ray tracing). Without it the game runs about 25-30 % slower. |
| Screen resolution | 1280x720 (default) | Tested setting. Higher values are untested. |
| Present mode | Mailbox | Tested setting. |
| Vulkan validation, Shader validation, RenderDoc, Command buffer dump, Tracy | **Off** | Debugging tools: they make the game much slower. |
| Shader logging, Printf output | **Silent** | Logging to files slows the game down. |

### Skipped shaders

Six of the game's shaders are skipped: their draws are left out. They were picked by Mac (from
the KytyPS5 Wolverine work) because they cost a lot of frame time or misbehave; one of them is a
ray-tracing shader. Skipping them makes the game run about 25-30 % faster, and the effects they
draw are missing (most notably ray-traced effects). Both start methods use this list:

```
bad108e74fb72e9f,4e7f2c6bb9b158a1,8bfd230b9cd875a2,c4df2a00067e0666,dc76e1223a9bf673,e6d76d24f59f8015
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
| Main menu | ~30 fps |
| Standing still, open areas | 25-30 fps |
| Dense jungle with many plants | ~12-15 fps |
| Running and turning | ~12-20 fps, with occasional stutter |
| Fights | lower; varies a lot |

The emulator's own CPU work limits the frame rate in busy scenes (the GPU is often half idle), so a
faster GPU helps less than you might expect.

## 6. Known issues

| Issue | Notes |
|---|---|
| **Crash when the window is resized or minimized** | Don't touch the window during play. |
| **Rare GPU crash ("device lost")**, more likely in long sessions | Save often. Please report it with the log (section 11). |
| **Light or shadow patches flickering on the ground**, decals (tyre tracks) appearing and disappearing | Being investigated (probably cached shadow maps). |
| Textures missing for a moment after spawning or loading | They appear after a few seconds. |
| Stutter when new things appear, especially on the first launch | The shader cache fills over time. |
| The moon in the night sky is drawn tiled | Known, not fixed yet. |
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

- **Source code:** built from commit `fb54e221` of the Wolverine branch of KytyPS5. KytyPS5 is
  licensed under the **GNU GPL 2.0** (see `LICENSE-GPL-2.0.txt`, and `LICENSE-Kyty-MIT.txt` for
  the original Kyty code). The source of this build: https://github.com/IDXTRI/KytyPS5/tree/wolverine-v1 (tag `wolverine-v1`). Third-party
  licenses are in `emulator\licenses`.
- **Credits:**
  - **KytyPS5** and all its contributors: the emulator.
  - **Mac (itsmemac), Senaxx and Ali Almohaya**: the Marvel's Wolverine work in KytyPS5 PR #937
    that this build is based on, and Senaxx's later Wolverine fixes.
  - **Jetsku**: the SPIR-V Function-array shrink pass (optional, see section 10).
  - **DXTR**: this build.

Marvel's Wolverine is a trademark of its owners. This project is not affiliated with Sony,
Insomniac Games or Marvel. Use only a copy of the game you own.
