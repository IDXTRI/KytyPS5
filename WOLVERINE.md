# Marvel's Wolverine on KytyPS5: performance and stability work

## v2 (branch `wolverine-v2`)

On top of `wolverine-v1`:

- **Game samplers by default** (`KYTY_BINDLESS_SAMPLERS=0` restores the generic linear/wrap
  sampler). Draws were being skipped with them: the sampler heap's buffer, whose S# reads the
  bindless sampler plan strands, could not be materialized. Such a buffer now binds empty
  (`KYTY_DEAD_BUFFER_EMPTY=0` restores failing). Fixes the tiled moon, flat ground, and the
  vanishing suit, tree trunks and title logs.
- **Translated-shader disk cache** (`KYTY_PROGRAM_DISK_CACHE=1`), **VRAM saver presets**
  (`KYTY_VRAM_SAVER=1|2`), stale bindless view guard, shader compile stats.
- Skipped draws are reported in release logs (`ProgramCache: skipping ...`).
- Diagnostics: live shader list and live skip (`KYTY_SHADER_LIST`, `KYTY_LIVE_SKIP`), glitch report
  (`KYTY_GLITCH_REPORT`), image address watch, IR dumps by hash (`KYTY_IR_DUMP_HASHES`).
- Upstream KytyPS5 main merged (2026-10-04) and 36 later commits cherry-picked (save-load crash
  fix, texture views/aliases, blending, shader instructions); the legacy FP32 MAD rounding change
  is reverted (it cost ~20 % fps). Senaxx ports: IR validation in debug only, ClampRange cache,
  NMin/NMax, lane-mask folding, NGG 9-bit primitive indices.
- **Bindless texture eviction on** (`KYTY_BINDLESS_EVICT=1`) with a **memory graveyard**
  (`KYTY_MEMORY_GRAVEYARD_MB=2048`): freed device memory is held back before it is reused, a
  safety net for a stale texture read not found yet (rare device lost). Without eviction VRAM
  reached 14 GB on a 12 GB card and the game fell to 4 fps.
- **AMPR/AMM**: a write into extended memory waits for its queued map; an unmap keeps what a later
  submission mapped (fixed a render-thread write crash).
- Bindless: keep the old view while a changed T# is pending; slots freed once; vertices built
  from a placeholder texel are dropped.
- **F9** writes the last N frames (`KYTY_GLITCH_CAPTURE=N`); burst frame dumps and raw image dumps
  (`KYTY_FRAME_DUMP_BURST`, `KYTY_IMAGE_DUMP_ADDR`).
- `KYTY_APP0_OVERRIDE_DIR`: a host file replaces the game file of the same name (engine settings
  in `commandline.txt` without touching the dump).
- A mesh draw over the host workgroup limits is skipped instead of exiting.
- Skip list: Mac's six plus `4c582eb7319682a3,be2a9db103759bff` (white plane in a town room),
  `d6ec1c4ba883d308` (white wedge on a rooftop) and `4d2a3a54c74938ad` (orange screen flood in
  fights).
- Measured: menu 33-34 fps, open areas 25-30, turning 14-20, fights 10-12, cutscenes 1-4
  (RTX 4070 Ti). Open: ground-decal flicker (two bindless material pixel shaders), the floods'
  real cause, the stale read behind the device losts.
- **Paused** after this test build: the next version moves onto Senaxx's reworked KytyPS5 build.

## v1

Branch `wolverine-v1` (release
[wolverine-v1](https://github.com/IDXTRI/KytyPS5/releases/tag/wolverine-v1)): KytyPS5 release
2026-10-03 (f53e5d2) merged with the community Wolverine work (PR #937: Mac/itsmemac, Senaxx, Ali
Almohaya) and the changes below. The earlier branch `wolverine-perf` was not merged with upstream.

Tested with PPSA03671 v01.001.005 on Windows 11, Ryzen 7 7800X3D, RTX 4070 Ti (12 GB),
clang-cl Release build. Run with
`--bindless --redzone --tessellation --readback-linear-images true` (the earlier `--amd-cpu` for
AMD CPUs is no longer recommended: it emulates VRSQRTPS through a CPU exception per use and the
PS5's CPU is AMD anyway) and
Mac's skip list `--skip-shaders bad108e74fb72e9f,4e7f2c6bb9b158a1,8bfd230b9cd875a2,c4df2a00067e0666,dc76e1223a9bf673,e6d76d24f59f8015`.
No environment variables are needed: `KYTY_LABELS_AFTER_GPU` is on by default, and upstream's
written-overlap check only warns (`KYTY_IGNORE_WRITTEN_OVERLAP` is gone). The release package
(`Play Wolverine.bat`, launcher preset) sets all of this.

## Results

| Scene | `wolverine-perf` (before the merge) | `wolverine-v1` |
|---|---|---|
| Main menu | ~30 fps | ~30 fps |
| Gameplay spot, standing still | ~29 fps (benchmark) | ~29-30 fps |
| Dense jungle | not measured | ~12-15 fps |
| VRAM (device usage, 12 GB card) | 12.7 GiB, ~1.8 GiB in shared memory | 10.3-11.1 GiB, ~0.7 GiB shared |

Same spot (slot 2 save); `wolverine-v1` numbers with the user driving, the user judged it the
best of the builds compared. Without Mac's skip list the spot runs at
21-22 fps. In busy scenes the GPU is only ~36-40 % busy: the emulator's CPU side is the limit.

Changes were measured with same-process A/B (live switches, Tracy) where possible, and only
changes with a measured gain or a fix are on by default.

## Main changes (default on)

**Memory**
- **Two-level BDA page table**: shader memory accesses translate guest pages through a directory
  and 2,048 chunks (64 MiB) instead of a flat 768 MiB table.
- **Image pool** (`KYTY_IMAGE_RECYCLE_MB=256`): freed images are parked and reused by images of
  the same shape; parked images are trimmed after 2 s or above the cap.
- **Garbage collection tuning**: image-aware buffer age (`KYTY_BUFFER_GC_AGE=120`), collector every
  4 ms (`KYTY_GC_INTERVAL_US=4000`), combined buffer+image budget (`KYTY_GC_COMBINED=1`), critical
  ages 60 (buffers) / 30 (images).
- **Streamed textures** (`KYTY_BINDLESS_REVALIDATE=1`, from Senaxx 0563e10e): a bindless key whose
  heap T# changed is settled again and its old image is unpinned.

**Performance**
- **Recording thread** (`KYTY_RECORD_THREAD=1`): Vulkan command recording moves off Thread_Gpu.
- **Native SRT walker** (`KYTY_SRT_NATIVE=1`) and compiled plans (`KYTY_SRT_COMPILED=1`), with the
  replay of the last reachability walk.
- **Page read cache** (`KYTY_SHADER_READ_CHUNKS=2`).
- **Bindless generation skip** (`KYTY_BINDLESS_SKIP=1`).
- **Async readbacks** (`KYTY_ASYNC_READBACK=1`, `KYTY_COPY_QUEUE_READBACK=1`).
- **Driver pipeline cache kept across builds** (`KYTY_PIPELINE_CACHE_ANY_REVISION=1`).

**Stability**
- **GPU fault handling**: a host fault on a page the GPU cache no longer maps is retried once the
  page is accessible; guest pages whose protection was orphaned get their access restored.
- **Suspend points** block only on the previous suspend point, as on the console.
- **Labels after GPU** (`KYTY_LABELS_AFTER_GPU=1`): EOP labels, timestamps and flips are published
  when the GPU has executed the preceding work.

**Input**: the Xbox View/Back button acts as the PS5 touchpad.

## Merge with KytyPS5 2026-10-03 (e6cb880, f53e5d2)

Upstream versions were taken where they replace earlier copies of the same work (BVH execution,
FLAT apertures and local stacks, code-hash shader identities, IR operand storage, explicit gather
LOD, 64-bit atomics). Kept or adjusted for Wolverine:

- `KYTY_SKIP_BVH_DISPATCHES=1`: programs with BVH intersections are still skipped (Wolverine's
  CS 0x29be8047c4e86afe ran skipped; upstream's BVH GPU test loses the device on this build).
- Upstream's scalar-read overlap check (6409be28) warns once per shader pair instead of exiting.
- A scalar read through a null base evaluates to 0 (CS 0x0b4b91abfed42248).
- Finite images only for sampled handles (Senaxx edc4532a); fast-launch mesh instance VGPR and
  primitive capacity (Senaxx e6f33fdb).
- Explicit-LOD gathers with linear mip filtering or LOD bias read the selected mip instead of
  failing the specialization (upstream skipped the draw: 3,130 failures in the first minutes).
- Our suspend points, `KYTY_BOOL_PREDICATION_WAIT`, windowed async downloads and the
  once-per-submission BDA sync are kept.

## Experimental (default off)

- `KYTY_FUNCTION_ARRAY_SHRINK=1` (from Jetsku/KytyPS5): shrinks Function-storage arrays to their
  proven index bound before the driver sees the module. In Wolverine it changes 7 modules by 32
  elements each: no VRAM gain, more stutter (the driver recompiles them).
- `KYTY_SKIP_BVH_DISPATCHES=0`: runs the ray-tracing programs (untested in the game).
- `KYTY_BINDLESS_EVICT=1`: unpins bindless textures the shaders stop sampling. It exposes a read of
  an image whose memory was already released (device lost within minutes); under investigation.
- `KYTY_VMA_BLOCK_MB=64`: smaller device memory blocks; same stale-image read.

## Diagnostics

- Live switches: set `KYTY_LIVE_FILE` to a text file and write `NAME=value` lines while the game
  runs (A/B in one process).
- `KYTY_MEMORY_STATS`, `KYTY_IMAGE_STATS`, `KYTY_IMAGE_GC_STATS`, `KYTY_BUFFER_STATS`,
  `KYTY_BUFFER_AUDIT`, `KYTY_GPU_TIME`, `KYTY_FAULT_STATS`: periodic reports in the log.
- Device loss: `VK_EXT_device_fault` + `VK_EXT_device_address_binding_report` name the buffer or
  image a faulting GPU address belonged to, its lifecycle events (created, taken from or parked in
  the image pool, destroyed) and the draws that bound it (`KYTY_ADDRESS_BINDING_REPORT=0` turns
  the binding report off). `KYTY_BDA_VERIFY=N` checks the BDA table against the buffer cache.

## Known issues

- Resizing, minimizing or full-screening the window can lose the device (from PR #937).
- Rare device loss in long sessions: an image's memory is released while the GPU can still read
  it (seen with 64 MiB blocks; under investigation).
- Flickering light/shadow patches on the ground and decals appearing and disappearing (not
  texture streaming, readback timing or recording order; probably cached shadow maps).
- Moon tiled across the night sky.
- Ray-traced effects are skipped.
- Pixel shader 0x7dce3261e625949c can hang the GPU in the night scene (from PR #937).
