# MinGW Makefile for Windows CLI client
# Usage: mingw32-make  (or make with MinGW in PATH)
#
# For MSVC, use build.bat instead.

CXX      = g++
CXXFLAGS = -std=c++11 -O2 -Wall -Wextra \
           -I../src \
           -I../../common \
           -I../../3rdparty/kcp \
           -I../../3rdparty/monocypher \
           -I../../3rdparty/cm256cc \
           -DWIN32_LEAN_AND_MEAN -DUSE_SSSE3 -mssse3
LDFLAGS  = -lws2_32 -liphlpapi -static

SHARED_SRC = ../src
KCP_DIR    = ../../3rdparty/kcp
MONO_DIR   = ../../3rdparty/monocypher
CM256_DIR  = ../../3rdparty/cm256cc

SOURCES = $(SHARED_SRC)/main.cpp          \
          $(SHARED_SRC)/cli_app.cpp        \
          $(SHARED_SRC)/cli_net.cpp        \
          $(SHARED_SRC)/cli_tunnel.cpp     \
          $(SHARED_SRC)/cli_peer.cpp       \
          $(SHARED_SRC)/cli_fec.cpp        \
          $(SHARED_SRC)/cli_tun_win.cpp    \
          $(KCP_DIR)/ikcp.c               \
          $(MONO_DIR)/monocypher.c        \
          $(CM256_DIR)/cm256.cpp          \
          $(CM256_DIR)/gf256.cpp

OBJECTS = $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(SOURCES)))
TARGET  = vlan-cli.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) -std=c99 -O2 -Wall -I$(KCP_DIR) -I$(MONO_DIR) -c -o $@ $<

clean:
	del /Q $(TARGET) 2>nul
	del /Q $(subst /,\,$(OBJECTS)) 2>nul
