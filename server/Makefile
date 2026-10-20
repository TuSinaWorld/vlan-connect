CXX      = g++
CXXFLAGS = -std=c++11 -O2 -Wall -Wextra -I../common -I../3rdparty/kcp -I../3rdparty/monocypher
LDFLAGS  = -lpthread
BUILD_COMMIT ?= $(shell git rev-parse --short=12 HEAD 2>/dev/null || printf unknown)
CXXFLAGS += -DVLAN_BUILD_COMMIT=\"$(BUILD_COMMIT)\" -MMD -MP

SRCDIR   = src
SOURCES  = $(SRCDIR)/main.cpp       \
           $(SRCDIR)/signal_server.cpp \
           $(SRCDIR)/relay_server.cpp  \
           $(SRCDIR)/room.cpp          \
           $(SRCDIR)/server_logger.cpp \
           ../3rdparty/kcp/ikcp.c \
           ../3rdparty/monocypher/monocypher.c

OBJECTS  = $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(SOURCES)))
TARGET   = vlan-server

.PHONY: all clean

all: $(TARGET)

-include $(OBJECTS:.o=.d)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) -std=c99 -O2 -Wall -I../3rdparty/kcp -I../3rdparty/monocypher -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJECTS) $(OBJECTS:.o=.d)
