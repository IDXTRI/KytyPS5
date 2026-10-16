**Test release v2** of KytyPS5 tuned for **Marvel's Wolverine** (PPSA03671, version 01.001.005). Windows 64-bit only.

No game files, keys or firmware are included: you need your own dump of the game.

> **Test build.** This is a snapshot of the work so far, published for testing. It is not a
> finished release, and **no further v2 updates are planned for now**: the next version waits
> for Senaxx's new KytyPS5 build (a reworked render path), which this work will move onto.


### How to use
1. Download `Wolverine-KytyPS5-v2.zip` and extract it.
2. Double-click `Play Wolverine.bat` and enter the folder that contains the game's `eboot.bin`.
3. The first launch compiles shaders: expect a long first load and stutter. The second launch is much smoother (translated shaders are kept on disk).

Updating from v1: keep your `userdata` folder (saves carry over).

Full guide (recommended settings, known issues, troubleshooting, the launcher): **README.md** in the zip.

### New in v2
- **The moon is a single moon again** and **ground and trees keep their detail**: the game's own texture samplers are used. Draws that were silently dropped with them (Wolverine's suit, tree trunks, title-screen logs) are fixed.
- **White plane in a town room, white wedge on a rooftop and orange screen floods in fights** removed (shaders skipped).
- **Unused textures are released**: video memory no longer overflows into system memory in long sessions (before: down to 4 fps after a while).
- **F9** saves the last 40 frames into `userdata\framedump` for bug reports; files in `userdata\overrides` can replace game files (e.g. engine settings) without touching your game folder.
- **Less stutter on the second launch**: translated shaders are kept on disk.
- **Video memory**: budget-aware texture pool and a safety net for deleted images.
- **Fixes from KytyPS5 (October 2026)**: loading a save from the in-game menu no longer crashes, texture view and alias fixes, blending fixes, more shader instructions, mesh-shader triangle indices; Senaxx's later Wolverine fixes.
- Tested on a Ryzen 7 7800X3D + RTX 4070 Ti: ~33-34 fps in the menu, 25-30 standing in open areas, ~14-15 at the busy "Leap Attack" jungle spot.

### Known issues
Don't resize or minimize the game window. Random GPU crash ("device lost"), more likely when video memory is full and when loading another save: save often, and restart the game to change saves. Cutscenes run at 1-4 fps, fights at about 10-12. Ground/wall detail and decals (blood, tyre tracks) flicker (present one frame, gone the next). Occasional short purple/magenta colours. Ray-traced effects are skipped. Only the opening areas were tested.

### Credits
KytyPS5 and contributors; Mac (itsmemac), Senaxx and Ali Almohaya (PR #937 and later Wolverine fixes); Jetsku. Source: branch `wolverine-v2` of this repository (GPL-2.0).

Not affiliated with Sony, Insomniac Games or Marvel.
