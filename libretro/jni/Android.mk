LOCAL_PATH := $(call my-dir)

CORE_DIR := $(LOCAL_PATH)/../..

include $(CORE_DIR)/libretro/Makefile.common

# USE_FILE32API: bionic only declares fopen64/fseeko64/ftello64 -- and even
# fseeko/ftello -- from API 24, while APP_PLATFORM is android-21. minizip's
# own switch maps its file layer to fopen/fseek/ftell, which exist on every
# API level and are plenty for ROM-sized archives.
COREFLAGS := -DANDROID -D__LIBRETRO__ -DHAVE_STRINGS_H -DRIGHTSHIFT_IS_SAR -DALLOW_CPU_OVERCLOCK $(INCFLAGS) $(UNZIP_DEFINES) -DUSE_FILE32API

GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
  COREFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

include $(CLEAR_VARS)
LOCAL_MODULE    := retro
LOCAL_SRC_FILES := $(SOURCES_C) $(SOURCES_CXX)
# ndk-build defaults to -O2, and to thumb -Oz on armeabi-v7a; the RP2040 interpreter needs the speed.
LOCAL_ARM_MODE  := arm
LOCAL_CXXFLAGS  := $(COREFLAGS)
LOCAL_CFLAGS    := $(COREFLAGS) -O3
LOCAL_LDFLAGS   := -Wl,-version-script=$(CORE_DIR)/libretro/link.T
# zlib ships with the NDK, so $(UNZIP_LIBS) resolves without extra setup
LOCAL_LDLIBS    := $(UNZIP_LIBS)
include $(BUILD_SHARED_LIBRARY)
