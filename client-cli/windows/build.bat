@echo off
REM Run in an x64 MSVC 2015+ environment; optional argument is an independent build directory.
setlocal EnableExtensions
set "BUILD_DIR=%~1"
if not defined BUILD_DIR set "BUILD_DIR=%~dp0"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if errorlevel 1 exit /b 1
pushd "%BUILD_DIR%"
if errorlevel 1 exit /b 1

set "SRC=%~dp0..\src"
set "COMMON=%~dp0..\..\common"
set "KCP=%~dp0..\..\3rdparty\kcp"
set "MONO=%~dp0..\..\3rdparty\monocypher"
set "CM256=%~dp0..\..\3rdparty\cm256cc"
set "SOURCE_ROOT=%~dp0..\.."
if not defined VLAN_BUILD_COMMIT for /f %%i in ('git -C "%SOURCE_ROOT%" rev-parse --short^=12 HEAD 2^>nul') do set "VLAN_BUILD_COMMIT=%%i"
if not defined VLAN_BUILD_COMMIT set "VLAN_BUILD_COMMIT=unknown"
set INCLUDES=/I"%SRC%" /I"%COMMON%" /I"%KCP%" /I"%MONO%" /I"%CM256%"
set DEFINES=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /Dcm256cc_STATIC /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DUSE_SSSE3 /DVLAN_BUILD_COMMIT=\"%VLAN_BUILD_COMMIT%\"
REM C++11 is the source baseline; /std:c++14 is unsupported by the first VS2015 releases.
set CFLAGS=/nologo /O2 /EHsc /W3 /utf-8 /FI"%COMMON%\msvc2015_compat.h" %DEFINES% %INCLUDES%
set CFLAGS_C=/nologo /O2 /W3 /utf-8 /FI"%COMMON%\msvc2015_compat.h" %INCLUDES%

echo Compiling C sources...
cl %CFLAGS_C% /c "%KCP%\ikcp.c" /Fo:ikcp.obj
if errorlevel 1 goto failed
cl %CFLAGS_C% /c "%MONO%\monocypher.c" /Fo:monocypher.obj
if errorlevel 1 goto failed

echo Compiling C++ sources...
for %%f in (cm256 gf256) do (
    cl %CFLAGS% /c "%CM256%\%%f.cpp" /Fo:%%f.obj
    if errorlevel 1 goto failed
)
for %%f in (cli_fec cli_peer cli_net cli_tunnel cli_tun_win cli_app cli_machine_app main) do (
    cl %CFLAGS% /c "%SRC%\%%f.cpp" /Fo:%%f.obj
    if errorlevel 1 goto failed
)

echo Linking...
link /nologo /OUT:vlan-cli.exe ^
    main.obj cli_app.obj cli_machine_app.obj cli_net.obj cli_tunnel.obj cli_peer.obj ^
    cli_fec.obj cli_tun_win.obj ^
    ikcp.obj monocypher.obj cm256.obj gf256.obj ^
    ws2_32.lib iphlpapi.lib bcrypt.lib ole32.lib oleaut32.lib
if errorlevel 1 goto failed
vlan-cli.exe --version
if errorlevel 1 goto failed
echo Build succeeded: %CD%\vlan-cli.exe
popd
exit /b 0

:failed
echo Build failed; no package should be published from this output directory.
popd
exit /b 1
