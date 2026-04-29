@echo off

call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64

if %errorlevel% neq 0 (
    echo Failed to load VS environment
    exit /b %errorlevel%
)

pushd "%~dp0"

if exist build_nmake rmdir /s /q build_nmake

mkdir build_nmake
cd build_nmake

cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
if %errorlevel% neq 0 (
    popd
    exit /b %errorlevel%
)

nmake
if %errorlevel% neq 0 (
    popd
    exit /b %errorlevel%
)

popd

echo.
echo Build complete: build_nmake\PhobosHost_artefacts\Release\PhobosHost.exe
