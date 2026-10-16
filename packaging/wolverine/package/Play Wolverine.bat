@echo off
rem Marvel's Wolverine on KytyPS5 (Wolverine build v2).
rem First launch asks for your game folder and remembers it in game_path.txt.
rem Saves, shader cache and logs go to the userdata folder next to this file, never into the game.
setlocal EnableExtensions
set "HERE=%~dp0"
set "EMU=%HERE%emulator\kyty_emulator.exe"
set "USERDATA=%HERE%userdata"
set "PATHFILE=%HERE%game_path.txt"

if not exist "%EMU%" (
  echo Could not find emulator\kyty_emulator.exe next to this file.
  echo Extract the whole archive, keeping its folders.
  pause
  exit /b 1
)

if exist "%PATHFILE%" set /p GAME=<"%PATHFILE%"
if not defined GAME goto ask
if not exist "%GAME%\eboot.bin" (
  echo The saved game folder no longer has eboot.bin: %GAME%
  goto ask
)
goto run

:ask
echo.
echo Enter the full path of your Marvel's Wolverine game folder
echo ^(the folder that contains eboot.bin, for example D:\Games\PPSA03671-app0^):
set "GAME="
set /p GAME=Game folder:
rem Accept a path pasted with quotes.
set GAME=%GAME:"=%
if not exist "%GAME%\eboot.bin" (
  echo eboot.bin was not found in "%GAME%". Try again.
  goto ask
)
> "%PATHFILE%" echo %GAME%

:run
if not exist "%USERDATA%\logs" mkdir "%USERDATA%\logs"

rem Recommended settings for this build (the ones it was tested with).
set KYTY_IGNORE_WRITTEN_OVERLAP=1
set KYTY_LABELS_AFTER_GPU=1
set KYTY_PIPELINE_CACHE_ANY_REVISION=1
rem Keep translated shaders on disk: the second launch skips most shader translation (less stutter).
set KYTY_PROGRAM_DISK_CACHE=1
rem Video memory: budget-aware image pool, retire safety net, smaller blocks, and eviction of
rem textures the game no longer uses (without it VRAM overflows into system memory after a while
rem and the frame rate drops to single digits). Freed memory is held back up to 2 GiB as a safety
rem net against a known stale read (rare "device lost").
set KYTY_VRAM_SAVER=2
set KYTY_BINDLESS_EVICT=1
set KYTY_MEMORY_GRAVEYARD_MB=2048
rem Host fault handling for a rare write crash.
set KYTY_FAULT_WATCHED_UNMAPPED=1
set KYTY_FAULT_REPAIR_HOST_PAGE=1
set KYTY_FAULT_SETTLE_MS=50
rem Live switches for testing: write NAME=value lines into this file while the game runs.
set KYTY_LIVE_FILE=%USERDATA%\kyty_live.txt
type nul > "%KYTY_LIVE_FILE%"
rem F9 saves the last 40 frames into userdata\framedump (attach them to a bug report).
set KYTY_GLITCH_CAPTURE=40
rem Files in userdata\overrides replace the game's file of the same name without touching the
rem game folder (e.g. commandline.txt for engine settings). Empty = the stock game.
set "KYTY_APP0_OVERRIDE_DIR=%USERDATA%\overrides"
if not exist "%KYTY_APP0_OVERRIDE_DIR%" mkdir "%KYTY_APP0_OVERRIDE_DIR%"

rem Shaders Mac (PR #937) recommends skipping: their draws cost a lot (one is a ray-tracing
rem shader). Remove the --skip-shaders option below to run them.
rem Ours: 4c582eb7/be2a9db1 draw a white plane in one building room, d6ec1c4b a white wedge on a
rem rooftop (light volumes), 4d2a3a54 floods the screen orange in fights (a fight effect).
rem Nothing visible goes missing without them.
set "SKIP=--skip-shaders bad108e74fb72e9f,4e7f2c6bb9b158a1,8bfd230b9cd875a2,c4df2a00067e0666,dc76e1223a9bf673,e6d76d24f59f8015,4c582eb7319682a3,be2a9db103759bff,d6ec1c4ba883d308,4d2a3a54c74938ad"

rem --amd-cpu (despite the name) replaces the guest's approximate VRSQRTPS with an exact value
rem through a CPU exception per executed instruction. The PS5's CPU is AMD, so on an AMD PC the
rem native instruction already behaves like the console's; the option only costs time there.
rem It is off on every CPU; put --amd-cpu here to try it (e.g. on Intel if animation or physics
rem look wrong). The log then shows "Guest VRSQRTPS emulated (5 s): N".
set "CPU_OPTS="

for /f %%d in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HHmmss"') do set STAMP=%%d
set "LOG=%USERDATA%\logs\run_%STAMP%.log"

rem The emulator writes _SaveData and its caches relative to the working directory.
cd /d "%USERDATA%" || exit /b 1

echo.
echo Starting Marvel's Wolverine. The first launch compiles shaders: expect a long first load
echo and stutter that fades as the cache fills.
echo Do not resize or minimize the game window while it runs.
echo Log: %LOG%
"%EMU%" --game "%GAME%" --bindless --redzone --tessellation --readback-linear-images true %CPU_OPTS% %SKIP% %* > "%LOG%" 2>&1
set CODE=%ERRORLEVEL%
echo Exit code: %CODE% >> "%LOG%"
echo.
if "%CODE%"=="0" (
  echo The game closed normally.
) else (
  echo The emulator stopped with code %CODE%. If it crashed, please share the log file:
  echo %LOG%
)
pause
