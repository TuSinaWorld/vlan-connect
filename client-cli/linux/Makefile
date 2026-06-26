CXX      = g++

ARCH := $(shell uname -m)
ifeq ($(ARCH),x86_64)
  SIMD_FLAGS = -DUSE_SSSE3 -mssse3
else ifeq ($(ARCH),i686)
  SIMD_FLAGS = -DUSE_SSSE3 -mssse3
else ifeq ($(ARCH),aarch64)
  SIMD_FLAGS = -DUSE_NEON
else ifeq ($(ARCH),armv7l)
  SIMD_FLAGS = -DUSE_NEON -mfpu=neon
else
  SIMD_FLAGS = -DUSE_SSSE3 -mssse3
endif

CXXFLAGS = -std=c++11 -O2 -Wall -Wextra -pthread $(SIMD_FLAGS) \
           -I../src \
           -I../../common \
           -I../../3rdparty/kcp \
           -I../../3rdparty/monocypher \
           -I../../3rdparty/cm256cc
LDFLAGS  = -lpthread

SHARED_SRC = ../src
COMMON     = ../../common
KCP_DIR    = ../../3rdparty/kcp
MONO_DIR   = ../../3rdparty/monocypher
CM256_DIR  = ../../3rdparty/cm256cc

SOURCES = $(SHARED_SRC)/main.cpp          \
          $(SHARED_SRC)/cli_app.cpp        \
          $(SHARED_SRC)/cli_net.cpp        \
          $(SHARED_SRC)/cli_tunnel.cpp     \
          $(SHARED_SRC)/cli_peer.cpp       \
          $(SHARED_SRC)/cli_fec.cpp        \
          $(SHARED_SRC)/cli_tun_linux.cpp  \
          $(KCP_DIR)/ikcp.c               \
          $(MONO_DIR)/monocypher.c        \
          $(CM256_DIR)/cm256.cpp          \
          $(CM256_DIR)/gf256.cpp

OBJECTS = $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(SOURCES)))
TARGET  = vlan-cli

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) -std=c99 -O2 -Wall -I$(KCP_DIR) -I$(MONO_DIR) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJECTS)
