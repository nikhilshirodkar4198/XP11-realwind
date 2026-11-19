@echo off
rem Builds RealWind\64\win.xpl with MSVC. Usage: build_win.bat <path-to-XPSDK>
setlocal
set SDK=%~1
if "%SDK%"=="" (echo usage: build_win.bat ^<path-to-XPSDK^> & exit /b 1)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist RealWind\64 mkdir RealWind\64
cl /nologo /O2 /MT /EHsc /LD /DIBM=1 /DXPLM200=1 /DXPLM210=1 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SDK%\CHeaders\XPLM" RealWindPlugin.cpp ^
   /Fo"%TEMP%\\" /Fe"RealWind\64\win.xpl" ^
   /link "%SDK%\Libraries\Win\XPLM_64.lib" winhttp.lib
del /q RealWind\64\win.exp RealWind\64\win.lib 2>nul
