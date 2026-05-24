# libsdrgg - Minimal zero-copy SDR driver for Linux
#
# Targets:
#   make          - Build static library
#   make shared   - Build shared library
#   make examples - Build example programs
#   make clean    - Remove build artifacts
#   make install  - Install to /usr/local
#
# Cross-compile for RPi4:
#   make CC=aarch64-linux-gnu-gcc

CXX ?= g++
AR ?= ar
CXXFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c++2a -fPIC -D_GNU_SOURCE
LDFLAGS = -lpthread -lm

PREFIX ?= /usr/local
EXAMPLES_DIR = examples
EXAMPLE_SRCS = $(EXAMPLES_DIR)/enumerate_devices.cpp $(EXAMPLES_DIR)/show_capabilities.cpp $(EXAMPLES_DIR)/register_access.cpp $(EXAMPLES_DIR)/stream_capture.cpp $(EXAMPLES_DIR)/read_sync_capture.cpp $(EXAMPLES_DIR)/gain_control.cpp $(EXAMPLES_DIR)/save_iq_u8.cpp $(EXAMPLES_DIR)/chip_aware_device.cpp $(EXAMPLES_DIR)/device_reset.cpp $(EXAMPLES_DIR)/multi_device_reliability.cpp
EXAMPLE_BINS = $(EXAMPLE_SRCS:.cpp=)

SRCS = sdrgg_usb.cpp sdrgg_rtl.cpp sdrgg_r820t.cpp sdrgg_fc0012.cpp sdrgg_tuner_caps.cpp sdrgg_core.cpp sdrgg_reset.cpp
OBJS = $(SRCS:.cpp=.o)
HEADERS = sdrgg.h sdrgg_internal.h sdrgg_r820t_internal.h sdrgg_fc0012_internal.h

LIB_STATIC = libsdrgg.a
LIB_SHARED = libsdrgg.so

.PHONY: all shared examples clean install

all: $(LIB_STATIC)

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

shared: $(OBJS)
	$(CXX) -shared -o $(LIB_SHARED) $^ $(LDFLAGS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

examples: all $(EXAMPLE_BINS)

$(EXAMPLES_DIR)/enumerate_devices: $(EXAMPLES_DIR)/enumerate_devices.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/show_capabilities: $(EXAMPLES_DIR)/show_capabilities.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/register_access: $(EXAMPLES_DIR)/register_access.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/stream_capture: $(EXAMPLES_DIR)/stream_capture.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/read_sync_capture: $(EXAMPLES_DIR)/read_sync_capture.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/gain_control: $(EXAMPLES_DIR)/gain_control.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/save_iq_u8: $(EXAMPLES_DIR)/save_iq_u8.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/chip_aware_device: $(EXAMPLES_DIR)/chip_aware_device.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/device_reset: $(EXAMPLES_DIR)/device_reset.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

$(EXAMPLES_DIR)/multi_device_reliability: $(EXAMPLES_DIR)/multi_device_reliability.cpp $(LIB_STATIC)
	$(CXX) $(CXXFLAGS) -o $@ $< -L. -lsdrgg $(LDFLAGS)

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) $(EXAMPLE_BINS)

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(PREFIX)/lib $(PREFIX)/include
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 755 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 sdrgg.h $(PREFIX)/include/
