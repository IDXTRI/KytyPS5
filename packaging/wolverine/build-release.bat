@echo off
rem Release build of KytyPS5 through the C:\kyty-src junction (to work\kytyps5), in its own build
rem directory, so the binaries embed C:/kyty-src/... source paths instead of this PC's folders.
rem Usage: build-kyty-release.bat [configure]
setlocal
set "PATH=%PATH:"=%"
set VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
set CMAKE="C:\Program Files\CMake\bin\cmake.exe"
set QT=C:/Qt/6.11.0/msvc2022_64
set BUILD=_Build/release

cd /d C:\kyty-src || exit /b 1
where clang-cl >nul || exit /b 1
where glslangValidator >nul || exit /b 1

if "%1"=="configure" rmdir /s /q _Build\release 2>nul
if not exist _Build\release\build.ninja (
  %CMAKE% -S . -B %BUILD% -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
    -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_PREFIX_PATH="%QT%" || exit /b 1
)
%CMAKE% --build %BUILD% --target launcher || exit /b 1
for /f %%v in ('git describe --tags --always --dirty') do set WANT=%%v
"_Build\release\kyty_emulator.exe" --help 2>&1 | findstr /c:"git = %WANT%," >nul || (
  echo Version stamp does not match %WANT%
  exit /b 1
)
%CMAKE% --install %BUILD% --prefix %BUILD%/install || exit /b 1
echo BUILD OK (%WANT%)
