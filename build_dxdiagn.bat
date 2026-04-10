@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1

cl /nologo /O2 /LD /W3 ^
  dxdiagn_stub.c ^
  /link /DLL /OUT:dxdiagn.dll ^
  kernel32.lib ole32.lib oleaut32.lib user32.lib

if %ERRORLEVEL% EQU 0 (
    echo.
    echo === BUILD SUCCESS ===
    dir dxdiagn.dll
) else (
    echo.
    echo === BUILD FAILED ===
)
