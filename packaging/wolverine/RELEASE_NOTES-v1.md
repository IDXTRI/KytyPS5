**Test release** of KytyPS5 tuned for **Marvel's Wolverine** (PPSA03671, version 01.001.005). Windows 64-bit only.

No game files, keys or firmware are included: you need your own dump of the game.

### How to use
1. Download `Wolverine-KytyPS5-v1.zip` and extract it.
2. Double-click `Play Wolverine.bat` and enter the folder that contains the game's `eboot.bin`.
3. The first launch compiles shaders: expect a long first load and stutter that fades as the cache fills.

Full guide (recommended settings, known issues, troubleshooting, the launcher): **README.md** in the zip.

### Highlights
- Based on KytyPS5 release 2026-10-03 (f53e5d2) with the Wolverine work from KytyPS5 PR #937.
- VRAM use about 10-11 GB instead of 12.7 GB; fewer GPU crashes; streamed textures are updated.
- Recommended settings built in; the Xbox View button works as the touchpad; the shader cache is kept across updates.
- Tested on a Ryzen 7 7800X3D + RTX 4070 Ti: ~30 fps in menus, 25-30 standing, 12-20 in dense jungle.

### Known issues
Don't resize or minimize the game window; occasional GPU crash (save often); flickering light/shadow patches and decals; stutter when new things appear; the moon is tiled; ray-traced effects are skipped. Only the opening areas were tested.

### Credits
KytyPS5 and contributors; Mac (itsmemac), Senaxx and Ali Almohaya (PR #937 and later Wolverine fixes); Jetsku (Function-array shrink pass). Source: branch `wolverine-v1` of this repository (GPL-2.0).

Not affiliated with Sony, Insomniac Games or Marvel.
