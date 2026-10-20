QT += core gui widgets network

TARGET = VLanClient
TEMPLATE = app

CONFIG += c++11

isEmpty(BUILD_COMMIT): BUILD_COMMIT = $$system(git -C $$shell_quote($$PWD) rev-parse --short=12 HEAD)
isEmpty(BUILD_COMMIT): BUILD_COMMIT = unknown
DEFINES += VLAN_BUILD_COMMIT=\\\"$$BUILD_COMMIT\\\"
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
    ../common/version.h \
    ../common/byte_buffer.h \
    ../common/traffic_policy_codec.h \
    ../common/relay_probe_watchdog.h \
    ../common/net_common.h \
    ../common/msvc2015_compat.h \
    ../common/payload_cipher.h \
    ../common/secure_frame.h \
    ../common/signal_message_validator.h \
    ../common/overlay_packet_validator.h \
    ../3rdparty/monocypher/monocypher.h

RESOURCES += resources/resources.qrc

win32 {
    # Use the same manifest for DPI awareness and UAC in every qmake build.
    RC_FILE = resources/app.rc
    RC_INCLUDEPATH += $$PWD/resources
    CONFIG -= embed_manifest_exe
    QMAKE_LFLAGS += /MANIFEST:NO
}

LIBS += -lws2_32 -liphlpapi -lbcrypt -lgdi32 -luser32 -lole32 -loleaut32

# rc.exe and Windows SDK include files come from the selected Developer
# Command Prompt/toolchain environment, rather than a machine-specific SDK path.
