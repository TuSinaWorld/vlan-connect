QT += core gui widgets network

TARGET = VLanClient
TEMPLATE = app

CONFIG += c++11
QMAKE_CXXFLAGS += /utf-8
QMAKE_CFLAGS   += /utf-8

DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX cm256cc_STATIC USE_SSSE3

# MSVC 2015 + Windows SDK 26100 compat: the new UCRT uses _mm_loadu_si64
# in wchar.h, which MSVC 2015 does not have. Force-include our polyfill
# before any system header.
MSVC2015_COMPAT = $$shell_path($$PWD/../common/msvc2015_compat.h)
QMAKE_CXXFLAGS += /FI$$MSVC2015_COMPAT
QMAKE_CFLAGS   += /FI$$MSVC2015_COMPAT

INCLUDEPATH += \
    ../3rdparty/wintun \
    ../3rdparty/kcp \
    ../3rdparty/cm256cc \
    ../3rdparty/monocypher \
    ../common

SOURCES += \
    src/main.cpp \
    src/core/tun_adapter.cpp \
    src/core/kcp_tunnel.cpp \
    src/core/raw_udp_tunnel.cpp \
    src/core/tunnel_manager.cpp \
    src/core/peer_connection.cpp \
    src/core/fec_codec.cpp \
    src/network/signal_client.cpp \
    src/network/data_channel.cpp \
    src/network/room_manager.cpp \
    src/ui/app_settings.cpp \
    src/ui/mainwindow.cpp \
    src/ui/modern_tray_menu.cpp \
    src/ui/roomwidget.cpp \
    src/ui/style_manager.cpp \
    src/ui/ui_strings.cpp \
    src/ui/log_manager.cpp \
    ../3rdparty/kcp/ikcp.c \
    ../3rdparty/cm256cc/cm256.cpp \
    ../3rdparty/cm256cc/gf256.cpp \
    ../3rdparty/monocypher/monocypher.c

HEADERS += \
    src/core/tun_adapter.h \
    src/core/kcp_tunnel.h \
    src/core/raw_udp_tunnel.h \
    src/core/tunnel_manager.h \
    src/core/peer_connection.h \
    src/core/fec_codec.h \
    src/network/signal_client.h \
    src/network/data_channel.h \
    src/network/room_manager.h \
    src/ui/app_settings.h \
    src/ui/mainwindow.h \
    src/ui/modern_tray_menu.h \
    src/ui/roomwidget.h \
    src/ui/style_manager.h \
    src/ui/ui_strings.h \
    src/ui/log_manager.h \
    ../3rdparty/kcp/ikcp.h \
    ../3rdparty/cm256cc/cm256.h \
    ../3rdparty/cm256cc/gf256.h \
    ../3rdparty/cm256cc/export.h \
    ../common/protocol.h \
    ../common/byte_buffer.h \
    ../common/net_common.h \
    ../common/msvc2015_compat.h \
    ../common/payload_cipher.h \
    ../common/secure_frame.h \
    ../3rdparty/monocypher/monocypher.h

RESOURCES += resources/resources.qrc

RC_ICONS = resources/vlan.ico

LIBS += -lws2_32 -liphlpapi -lbcrypt

# MSVC 2015 can't find rc.exe from the newer Windows SDK; point to it explicitly
RC_INCLUDEPATH = "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um"
QMAKE_RC = "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe"
