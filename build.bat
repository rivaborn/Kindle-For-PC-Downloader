@echo off
:: Usage: build.bat [release|debug]
:: Defaults to release if no argument given.

set CONFIG=%~1
if /i "%CONFIG%"=="debug" (
    set CONFIG=debug
) else (
    set CONFIG=release
)

echo [build] Configuration: %CONFIG%

call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"

if not exist build\%CONFIG% mkdir build\%CONFIG%

taskkill /f /im "KindleDownloader.exe" >nul 2>&1
del /f /q "build\%CONFIG%\KindleDownloader.exe" >nul 2>&1

rc.exe /fo "build\%CONFIG%\Kindle Downloader.res" "Kindle Downloader.rc"
if errorlevel 1 exit /b %errorlevel%

if /i "%CONFIG%"=="debug" (
    :: Debug: no optimisation, full debug symbols, debug runtime, _DEBUG defined
    cl.exe /EHsc /W4 /std:c++17 ^
        /Od /Zi /D_DEBUG /MDd ^
        /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS ^
        "Kindle Downloader.cpp" KWindow.cpp ^
        /Fd:"build\debug\KindleDownloader.pdb" ^
        /Fe:"build\debug\KindleDownloader.exe" ^
        /link /DEBUG ^
        user32.lib kernel32.lib comctl32.lib shell32.lib ^
        "build\debug\Kindle Downloader.res"
) else (
    :: Release: full optimisation, no debug symbols, release runtime, NDEBUG defined
    cl.exe /EHsc /W4 /std:c++17 ^
        /O2 /DNDEBUG /MD ^
        /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS ^
        "Kindle Downloader.cpp" KWindow.cpp ^
        /Fe:"build\release\KindleDownloader.exe" ^
        /link ^
        user32.lib kernel32.lib comctl32.lib shell32.lib ^
        "build\release\Kindle Downloader.res"
)
