@echo off
rem Marvel's Wolverine on KytyPS5 (Wolverine build, test 1).
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

rem Recommended settings for this build.
set KYTY_LABELS_AFTER_GPU=1
set KYTY_PIPELINE_CACHE_ANY_REVISION=1
set KYTY_LIVE_FILE=%USERDATA%\kyty_live.txt
type nul > "%KYTY_LIVE_FILE%"

rem Shaders Mac (PR #937) recommends skipping: their draws cost a lot (one is a ray-tracing
rem shader). Remove the --skip-shaders option below to run them.
set "SKIP=--skip-shaders bad108e74fb72e9f,4e7f2c6bb9b158a1,8bfd230b9cd875a2,c4df2a00067e0666,dc76e1223a9bf673,e6d76d24f59f8015"

rem The AMD CPU option (guest VRSQRTPS emulation) only on AMD processors.
set "CPU_OPTS="
echo %PROCESSOR_IDENTIFIER% | find /i "AuthenticAMD" >nul && set "CPU_OPTS=--amd-cpu"

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
