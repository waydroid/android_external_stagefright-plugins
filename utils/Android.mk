LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	ffmpeg_utils.cpp \
	ffmpeg_hwaccel.c

LOCAL_SHARED_LIBRARIES += \
	libavcodec        \
	libavutil         \
	libcutils         \
	liblog \
	libmediandk

LOCAL_MODULE := libffmpeg_utils

LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)
LOCAL_EXPORT_SHARED_LIBRARY_HEADERS += libavcodec

# Workaround for inline assembly tricks in FFMPEG which don't play nice with
# Clang when included from C++
LOCAL_CLANG_CFLAGS += -DAVUTIL_ARM_INTREADWRITE_H

include $(BUILD_SHARED_LIBRARY)
