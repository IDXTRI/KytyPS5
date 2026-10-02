# Marvel's Wolverine on KytyPS5: performance and stability work

Branch `wolverine-perf`, built on the community Wolverine work (PR #937: Mac/itsmemac, Senaxx,
Ali Almohaya). Not yet rebased on the latest `main`.

Tested with PPSA03671 v01.001.005 on Windows 11, Ryzen 7 7800X3D, RTX 4070 Ti (12 GB),
clang-cl Release build. Run with
`--bindless --redzone --tessellation --readback-linear-images true` and
`KYTY_IGNORE_WRITTEN_OVERLAP=1 KYTY_LABELS_AFTER_GPU=1`.

## Results

| Scene | Before | Now |
|---|---|---|
| Gameplay spot, standing still | ~11 fps | ~17-20 fps |
| Main menu | ~28 fps | ~30-31 fps |

Every change was measured with same-process A/B (live switches, Tracy), and only changes with a
measured gain are on by default.

## Main changes (default on)

- **Recording thread** (`KYTY_RECORD_THREAD=1`): Vulkan command recording moves off Thread_Gpu.
- **Native SRT walker** (`KYTY_SRT_NATIVE=1`): resource tables are evaluated by compiled plans.
- **Page read cache** (`KYTY_SHADER_READ_CHUNKS=2`): fewer guest-memory lookups while
  materializing shader resources.
- **Bindless generation skip** (`KYTY_BINDLESS_SKIP=1`): unchanged bindless heaps are not
  re-checked every draw.
- **Async readbacks** (`KYTY_ASYNC_READBACK=1`): guest writes to GPU-owned pages no longer wait
  for a synchronous download.
- **Garbage collection tuning**: image-aware buffer age (`KYTY_BUFFER_GC_AGE=120`), collector
  every 4 ms (`KYTY_GC_INTERVAL_US=4000`), combined buffer+image budget (`KYTY_GC_COMBINED=1`),
  critical ages 60 (buffers) / 30 (images) to stop delete/recreate churn in combat.
- **GPU thread priority** (`KYTY_GPU_THREAD_PRIORITY=1`, above normal).
- Fixes: record-thread deadlock with the copy queue, address-space reservation race, image GC
  that dropped UI glyphs.

## Experimental (default off)

- `KYTY_BINDLESS_EVICT=1` + `KYTY_IMAGE_DEDICATED_KB=1024`: shaders now report which bindless
  textures they sample, so unused ones can be unpinned and freed, and large images get their own
  device memory. While turning the camera in combat: median frame 133 -> 100 ms and no VRAM
  paging. **But** it exposes a stale texture read (an image read ~11 s after it was deleted) that
  loses the device within 1-2 minutes. Under investigation.

## Diagnostics

- Live switches: set `KYTY_LIVE_FILE` to a text file and write `NAME=value` lines while the game
  runs (A/B in one process).
- `KYTY_MEMORY_STATS`, `KYTY_IMAGE_STATS`, `KYTY_IMAGE_GC_STATS`, `KYTY_BUFFER_STATS`,
  `KYTY_GPU_TIME`, `KYTY_FAULT_STATS`: periodic reports in the log.
- Device loss: `VK_EXT_device_fault` + `VK_EXT_device_address_binding_report` name the buffer or
  image a faulting GPU address belonged to and when it was released
  (`KYTY_ADDRESS_BINDING_REPORT=0` turns the binding report off).

## Known issues

- Resizing, minimizing or full-screening the window can lose the device (from PR #937).
- Pixel shader 0x7dce3261e625949c can hang the GPU in the night scene (from PR #937).
- Moon tiled across the night sky; occasional ground flicker.
