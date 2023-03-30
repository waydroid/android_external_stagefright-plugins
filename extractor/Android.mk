LOCAL_PATH := $(call my-dir)

include $(SF_COMMON_MK)

LOCAL_SRC_FILES := \
	FFmpegExtractor.cpp \
	extractor_utils.cpp \
	ffmpeg_source.cpp

LOCAL_SHARED_LIBRARIES += \
	libavcodec        \
	libavformat       \
	libavutil         \
	libcutils         \
	libffmpeg_utils   \
	liblog            \
	libstagefright_foundation \
	libutils libmediandk

LOCAL_HEADER_LIBRARIES += \
	ffmpeg_codec2_headers \
	libmedia_headers

LOCAL_MODULE:= libffmpeg_extractor
LOCAL_MODULE_RELATIVE_PATH := extractors

include $(BUILD_SHARED_LIBRARY)
