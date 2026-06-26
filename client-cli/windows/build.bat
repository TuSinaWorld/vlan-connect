@echo off
REM MSVC build script for VLan CLI Client (Windows)
REM Run from VS Developer Command Prompt (or after calling vcvarsall.bat)

setlocal

set SRC=..\src
set COMMON=..\..\common
set KCP=..\..\3rdparty\kcp
set MONO=..\..\3rdparty\monocypher
set CM256=..\..\3rdparty\cm256cc

set INCLUDES=/I%SRC% /I%COMMON% /I%KCP% /I%MONO% /I%CM256%
set CFLAGS=/nologo /O2 /EHsc /std:c++14 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Dcm256cc_STATIC /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DUSE_SSSE3 %INCLUDES%
set CFLAGS_C=/nologo /O2 /W3 %INCLUDES%

echo Compiling C sources...
cl %CFLAGS_C% /c %KCP%\ikcp.c /Fo:ikcp.obj
cl %CFLAGS_C% /c %MONO%\monocypher.c /Fo:monocypher.obj

echo Compiling C++ sources...
cl %CFLAGS% /c %CM256%\cm256.cpp /Fo:cm256.obj
cl %CFLAGS% /c %CM256%\gf256.cpp /Fo:gf256.obj
cl %CFLAGS% /c %SRC%\cli_fec.cpp /Fo:cli_fec.obj
cl %CFLAGS% /c %SRC%\cli_peer.cpp /Fo:cli_peer.obj
cl %CFLAGS% /c %SRC%\cli_net.cpp /Fo:cli_net.obj
cl %CFLAGS% /c %SRC%\cli_tunnel.cpp /Fo:cli_tunnel.obj
cl %CFLAGS% /c %SRC%\cli_tun_win.cpp /Fo:cli_tun_win.obj
cl %CFLAGS% /c %SRC%\cli_app.cpp /Fo:cli_app.obj
cl %CFLAGS% /c %SRC%\main.cpp /Fo:main.obj

echo Linking...
link /nologo /OUT:vlan-cli.exe ^
    main.obj cli_app.obj cli_net.obj cli_tunnel.obj cli_peer.obj ^
    cli_fec.obj cli_tun_win.obj ^
    ikcp.obj monocypher.obj cm256.obj gf256.obj ^
    ws2_32.lib iphlpapi.lib

if %ERRORLEVEL%==0 (
    echo Build succeeded: vlan-cli.exe
) else (
    echo Build failed!
)

endlocal
