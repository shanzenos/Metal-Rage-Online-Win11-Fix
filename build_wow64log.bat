@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

cl /nologo /O2 /LD /W3 ^
  wow64log.c ^
  /link /DLL /OUT:wow64log.dll ^
  kernel32.lib user32.lib

if %ERRORLEVEL% EQU 0 (
    echo.
    echo === BUILD SUCCESS ===
    dir wow64log.dll
) else (
    echo.
    echo === BUILD FAILED ===
)
