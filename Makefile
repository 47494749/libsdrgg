# libsdrgg - Minimal zero-copy SDR driver for Linux
#
# Targets:
#   make          - Build shared library
#   make static   - Build static library
#   make examples - Build example programs
#   make clean    - Remove build artifacts
#   make install  - Install to /usr/local
#   make examples SDRGG_ENABLE_DIAGNOSTICS=1 - Enable diagnostic stderr logging
#
# Cross-compile for RPi4:
#   make CXX=aarch64-linux-gnu-g++

CXX ?= g++
AR ?= ar
CPPFLAGS ?=
CXXFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c++2a -fPIC -D_GNU_SOURCE
LDFLAGS = -lpthread -lm
SDRGG_ENABLE_DIAGNOSTICS ?= 0

ifeq ($(SDRGG_ENABLE_DIAGNOSTICS),1)
CPPFLAGS += -DSDRGG_ENABLE_DIAGNOSTICS=1
endif

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

VERSION_MAJOR ?= 1
VERSION_MINOR ?= 2
VERSION_PATCH ?= 1
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

EXAMPLES_DIR = examples
EXAMPLE_SRCS = $(EXAMPLES_DIR)/enumerate_devices.cpp $(EXAMPLES_DIR)/show_capabilities.cpp $(EXAMPLES_DIR)/register_access.cpp $(EXAMPLES_DIR)/stream_capture.cpp $(EXAMPLES_DIR)/read_sync_capture.cpp $(EXAMPLES_DIR)/gain_control.cpp $(EXAMPLES_DIR)/save_iq_u8.cpp $(EXAMPLES_DIR)/chip_aware_device.cpp $(EXAMPLES_DIR)/device_reset.cpp $(EXAMPLES_DIR)/multi_device_reliability.cpp
EXAMPLE_BINS = $(EXAMPLE_SRCS:.cpp=)
EXAMPLE_RPATH = -Wl,-rpath,'$$ORIGIN/..'

SRCS = sdrgg_usb.cpp sdrgg_rtl.cpp sdrgg_r820t.cpp sdrgg_fc0012.cpp sdrgg_tuner_caps.cpp sdrgg_core.cpp sdrgg_reset.cpp
OBJS = $(SRCS:.cpp=.o)
HEADERS = sdrgg.h sdrgg_internal.h sdrgg_r820t_internal.h sdrgg_fc0012_internal.h

LIB_STATIC = libsdrgg.a
LIB_SHARED = libsdrgg.so
LIB_SHARED_SONAME = $(LIB_SHARED).$(VERSION_MAJOR)
LIB_SHARED_VERSIONED = $(LIB_SHARED).$(VERSION)
PKGCONFIG_IN = libsdrgg.pc.in

.PHONY: all static shared examples clean install

all: $(LIB_SHARED)

static: $(LIB_STATIC)

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

shared: $(LIB_SHARED)

$(LIB_SHARED_VERSIONED): $(OBJS)
	$(CXX) -shared -Wl,-soname,$(LIB_SHARED_SONAME) -o $@ $^ $(LDFLAGS)

$(LIB_SHARED_SONAME): $(LIB_SHARED_VERSIONED)
	ln -sf $(LIB_SHARED_VERSIONED) $@

$(LIB_SHARED): $(LIB_SHARED_SONAME)
	ln -sf $(LIB_SHARED_SONAME) $@

%.o: %.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

examples: $(LIB_SHARED) $(EXAMPLE_BINS)

$(EXAMPLE_BINS): %: %.cpp $(LIB_SHARED)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< -L. $(EXAMPLE_RPATH) -lsdrgg $(LDFLAGS)

clean:
	rm -f $(OBJS) $(LIB_STATIC) $(LIB_SHARED) $(LIB_SHARED_SONAME) $(LIB_SHARED_VERSIONED) $(EXAMPLE_BINS)

install: $(LIB_STATIC) $(LIB_SHARED)
	install -d $(LIBDIR) $(INCLUDEDIR) $(PKGCONFIGDIR)
	install -m 644 $(LIB_STATIC) $(LIBDIR)/
	install -m 755 $(LIB_SHARED_VERSIONED) $(LIBDIR)/
	ln -sf $(notdir $(LIB_SHARED_VERSIONED)) $(LIBDIR)/$(LIB_SHARED_SONAME)
	ln -sf $(notdir $(LIB_SHARED_SONAME)) $(LIBDIR)/$(LIB_SHARED)
	install -m 644 sdrgg.h $(INCLUDEDIR)/
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@LIBDIR@|$(LIBDIR)|g' \
	    -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    $(PKGCONFIG_IN) > $(PKGCONFIGDIR)/libsdrgg.pc
