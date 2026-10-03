# Packaging the Marvel's Wolverine build

Files used to build the Windows release (`Wolverine-KytyPS5-v1.zip`):

| File | Purpose |
|---|---|
| `build-release.bat` | Release build through a `C:\kyty-src` junction to the checkout, in `_Build/release`, so the binaries embed `C:/kyty-src/...` source paths and not the builder's folders. Paths to Visual Studio, CMake and Qt are those of the machine that made v1; adjust them. |
| `package/Play Wolverine.bat` | Starts the game with the recommended options: asks for the game folder once, adds `--amd-cpu` on AMD processors, skips Mac's six shaders, keeps saves, cache and logs in `userdata\`. |
| `package/emulator/Kyty.ini` | Launcher preset read when `launcher.exe` starts from the `emulator` folder: Wolverine options on, debugging off, the skip list. |
| `package/README.md` | The user guide shipped in the zip. |
| `RELEASE_NOTES-v1.md` | Text of the GitHub release. |

## Steps

1. Commit everything (a build from a dirty tree disables the driver pipeline cache).
2. `mklink /J C:\kyty-src <checkout>` once, then run `build-release.bat`.
3. Package folder `Wolverine-KytyPS5-v<N>`:
   - `emulator\`: `_Build\release\install` without `include\` and `lib\`, plus `package\emulator\Kyty.ini`;
   - `Play Wolverine.bat` and `README.md` from `package\`;
   - `LICENSE` as `LICENSE-GPL-2.0.txt` and `LICENSES\Kyty-MIT.txt` as `LICENSE-Kyty-MIT.txt`.
4. Zip the folder. Check that it holds no `userdata`, `_SaveData`, `_PipelineCache` or
   `game_path.txt` (a test run leaves them in the folder).
5. Push the source (GPL-2.0: the source of the binaries must be published), tag it, and attach the
   zip to a GitHub release.
