RACK_DIR ?= ../Rack-SDK

FLAGS += -Idep/include
CFLAGS +=
CXXFLAGS +=

LDFLAGS +=
SOURCES += src/PureData.cpp

DISTRIBUTABLES += res examples
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/arch.mk

# LibPD
libpd := libpd/libs/libpd.a
OBJECTS += $(libpd)
DEPS += $(libpd)
FLAGS += -Ilibpd/libpd_wrapper -Ilibpd/pure-data/src -DHAVE_LIBDL

ifdef ARCH_WIN
	FLAGS += -DPD_INTERNAL -Ofast
	LDFLAGS += -Wl,--export-all-symbols
	LDFLAGS += -lws2_32
endif
	
$(libpd):
	cd libpd && $(MAKE) clean
ifdef ARCH_MAC
	cd libpd && $(MAKE) MULTI=true EXTRA=true STATIC=true ADDITIONAL_CFLAGS='-DPD_LONGINTTYPE="long long" $(DEP_MAC_SDK_FLAGS) -stdlib=libc++' ADDITIONAL_LDFLAGS='$(DEP_MAC_SDK_FLAGS) -stdlib=libc++'
else
ifdef ARCH_WIN
	cd libpd && $(MAKE) OS=Windows_NT MULTI=true EXTRA=true STATIC=true ADDITIONAL_CFLAGS='-DPD_LONGINTTYPE="long long" -DDONT_USE_ALLOCA=1'
else
	cd libpd && $(MAKE) MULTI=true EXTRA=true STATIC=true ADDITIONAL_CFLAGS='-DPD_LONGINTTYPE="long long"'
endif
endif
	cd libpd && $(MAKE) install prefix="$(DEP_PATH)"

include $(RACK_DIR)/plugin.mk
