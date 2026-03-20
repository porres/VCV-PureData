RACK_DIR ?= ../Rack-SDK

FLAGS += -Idep/include
CFLAGS +=
CXXFLAGS +=

LDFLAGS +=
SOURCES += src/PureData.cpp

DISTRIBUTABLES += res patches
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/arch.mk

# LibPD
libpd := dep/lib/libpd.a
OBJECTS += $(libpd)
DEPS += $(libpd)
FLAGS += -Idep/include/libpd -DHAVE_LIBDL

ifdef ARCH_WIN
	# PD_INTERNAL leaves the function declarations for libpd unchanged
	# not specifying that flag would enable the  "EXTERN __declspec(dllexport) extern" macro
	# which throws a linker error. I guess this macro should only be used for the windows
	# specific .dll dynamic linking format.
	# The corresponding #define resides in "m_pd.h" inside th Pure Data sources
	FLAGS += -DPD_INTERNAL -Ofast
	LDFLAGS += -Wl,--export-all-symbols
	LDFLAGS += -lws2_32
endif

$(libpd):
	cd dep && git clone "https://github.com/libpd/libpd.git" --recursive
	cd dep/libpd && git checkout tags/0.15.0
	
ifdef ARCH_MAC
	# libpd's Makefile is handmade, and it doesn't honor CFLAGS and LDFLAGS environments.
	# So in order for Mac 10.15 (for example) to make a build that works on Mac 10.7+, we have to manually add DEP_MAC_SDK_FLAGS to CFLAGS and LDFLAGS.
	# We can't just add the environment's CFLAGS/LDFLAGS because `-march=nocona` makes libpd segfault when initialized.
	# Perhaps inline assembly is used in libpd? Who knows.
	cd dep/libpd && $(MAKE) MULTI=true STATIC=true ADDITIONAL_CFLAGS='-DPD_LONGINTTYPE="long long" $(DEP_MAC_SDK_FLAGS) -stdlib=libc++' ADDITIONAL_LDFLAGS='$(DEP_MAC_SDK_FLAGS) -stdlib=libc++'
else
ifdef ARCH_WIN
	# libpd relies on OS=Windows_NT for platform detection even when cross-compiling.
	# Also force heap allocation path to avoid missing alloca.h in some MinGW toolchains.
	cd dep/libpd && $(MAKE) OS=Windows_NT MULTI=true STATIC=true ADDITIONAL_CFLAGS='-DPD_LONGINTTYPE="long long"'
else
	cd dep/libpd && $(MAKE) MULTI=true STATIC=true ADDITIONAL_CFLAGS='-DPD_LONGINTTYPE="long long"'
endif
endif
	cd dep/libpd && $(MAKE) install prefix="$(DEP_PATH)"

LDFLAGS += -Wl,-export_dynamic
include $(RACK_DIR)/plugin.mk
