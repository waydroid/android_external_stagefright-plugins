/*
 * Copyright 2022 Michael Goffioul <michael.goffioul@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "C2FFMPEGVideoDecodeComponent"
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android/hardware/graphics/common/1.2/types.h>
#include <log/log.h>
#include <algorithm>

#include <SimpleC2Interface.h>
#include "C2FFMPEGVideoDecodeComponent.h"
#include "ffmpeg_hwaccel.h"
extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}
#ifdef CONFIG_VAAPI
#include <C2AllocatorGralloc.h>
extern "C" {
#include <libavutil/hwcontext_internal.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <va/va_drmcommon.h>
#endif

#define DEBUG_FRAMES 0
#define DEBUG_WORKQUEUE 0
#define DEBUG_EXTRADATA 0

#define ALIGN(A, B) (((A) + (B)-1) & ~((B)-1))

#define DEINTERLACE_MODE_NONE 0
#define DEINTERLACE_MODE_SOFTWARE 1
#define DEINTERLACE_MODE_AUTO 2

using android::hardware::graphics::common::V1_2::BufferUsage;

typedef struct {
    int width;
    int height;
    int format;
} FilterSettings;

namespace android {

static int getDeinterlaceMode() {
    std::string prop = base::GetProperty("debug.ffmpeg-codec2.deinterlace", "auto");

    if (prop == "auto") {
        return DEINTERLACE_MODE_AUTO;
    } else if (prop == "software") {
        return DEINTERLACE_MODE_SOFTWARE;
    } else if (prop != "none") {
        ALOGE("unsupported deinterlace mode: %s", prop.c_str());
    }
    return DEINTERLACE_MODE_NONE;
}

C2FFMPEGVideoDecodeComponent::C2FFMPEGVideoDecodeComponent(
        const C2FFMPEGComponentInfo* componentInfo,
        const std::shared_ptr<C2FFMPEGVideoDecodeInterface>& intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<C2FFMPEGVideoDecodeInterface>>(componentInfo->name, 0, intf)),
      mInfo(componentInfo),
      mIntf(intf),
      mCodecID(componentInfo->codecID),
      mCtx(NULL),
      mFilterGraph(NULL),
      mFilterSrcCtx(NULL),
      mFilterSinkCtx(NULL),
      mImgConvertCtx(NULL),
      mFrame(NULL),
      mPacket(NULL),
      mCodecAlreadyOpened(false),
      mExtradataReady(false),
      mEOSSignalled(false) {
    ALOGD("C2FFMPEGVideoDecodeComponent: mediaType = %s", componentInfo->mediaType);
}

C2FFMPEGVideoDecodeComponent::~C2FFMPEGVideoDecodeComponent() {
    ALOGD("~C2FFMPEGVideoDecodeComponent: mCtx = %p", mCtx);
    onRelease();
}

c2_status_t C2FFMPEGVideoDecodeComponent::initDecoder() {
    mCtx = avcodec_alloc_context3(NULL);
    if (! mCtx) {
        ALOGE("initDecoder: avcodec_alloc_context failed.");
        return C2_NO_MEMORY;
    }

    C2StreamPictureSizeInfo::output size(0u, 320, 240);
    c2_status_t err = mIntf->query({ &size }, {}, C2_DONT_BLOCK, nullptr);
    if (err != C2_OK) {
        ALOGE("initDecoder: cannot query picture size, err = %d", err);
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    mCtx->codec_id = mCodecID;
    mCtx->extradata_size = 0;
    mCtx->extradata = NULL;
    mCtx->width = size.width;
    mCtx->height = size.height;

    const C2FFMPEGVideoCodecInfo* codecInfo = mIntf->getCodecInfo();

    if (codecInfo) {
        ALOGD("initDecoder: use codec info from extractor");
        mCtx->codec_id = (enum AVCodecID)codecInfo->codec_id;
    }

    mUseDrmPrime = base::GetBoolProperty("debug.ffmpeg-codec2.hwaccel.drm", true);
    mDeinterlaceMode = getDeinterlaceMode();
    mDeinterlaceIndicator = 0;

    ALOGD("initDecoder: %p [%s], %d x %d, %s, usage = %#" PRIx64 ", use-drm-prime = %d, deinterlace = %d",
          mCtx, avcodec_get_name(mCtx->codec_id), size.width, size.height, mInfo->mediaType,
          mIntf->getConsumerUsage(), mUseDrmPrime, mDeinterlaceMode);

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::openDecoder() {
    if (mCodecAlreadyOpened) {
        return C2_OK;
    }

    // Can't change extradata after opening the decoder.
#if DEBUG_EXTRADATA
    ALOGD("openDecoder: extradata_size = %d", mCtx->extradata_size);
#endif
    mExtradataReady = true;

    // Find decoder again as codec_id may have changed.
    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (! mCtx->codec) {
        ALOGE("openDecoder: ffmpeg video decoder failed to find codec %d", mCtx->codec_id);
        return C2_NOT_FOUND;
    }

    // Configure decoder.
    mCtx->workaround_bugs   = 1;
    mCtx->idct_algo         = 0;
    mCtx->skip_frame        = AVDISCARD_DEFAULT;
    mCtx->skip_idct         = AVDISCARD_DEFAULT;
    mCtx->skip_loop_filter  = AVDISCARD_DEFAULT;
    mCtx->error_concealment = 3;
    mCtx->thread_count      = base::GetIntProperty("debug.ffmpeg-codec2.threads", 0);

    if (base::GetBoolProperty("debug.ffmpeg-codec2.fast", false)) {
        mCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    ffmpeg_hwaccel_init(mCtx);

#if CONFIG_VAAPI
    if (mCtx->hw_device_ctx
            && ((AVHWDeviceContext*)mCtx->hw_device_ctx->data)->type == AV_HWDEVICE_TYPE_VAAPI
            && mUseDrmPrime) {
        openDecoderVAAPI();
    }
#endif

    ALOGD("openDecoder: opening ffmpeg decoder(%s): threads = %d, hw = %s",
          avcodec_get_name(mCtx->codec_id), mCtx->thread_count, mCtx->hw_device_ctx ? "yes" : "no");

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("openDecoder: ffmpeg video decoder failed to initialize. (%s)", av_err2str(err));
        return C2_NO_INIT;
    }
    mCodecAlreadyOpened = true;

    ALOGD("openDecoder: open ffmpeg video decoder(%s) success, caps = %08x",
          avcodec_get_name(mCtx->codec_id), mCtx->codec->capabilities);

    mFrame = av_frame_alloc();
    if (! mFrame) {
        ALOGE("openDecoder: oom for video frame");
        return C2_NO_MEMORY;
    }

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::deInitDecoder() {
    ALOGD("%p deInitDecoder: %p", this, mCtx);
    if (mFilterGraph) {
        av_freep(&mFilterGraph->opaque);
        avfilter_graph_free(&mFilterGraph);
        mFilterSrcCtx = mFilterSinkCtx = NULL;
    }
    if (mCtx) {
        if (avcodec_is_open(mCtx)) {
            avcodec_flush_buffers(mCtx);
        }
#if CONFIG_VAAPI
        if (mCtx->hw_frames_ctx
                && mCtx->pix_fmt == AV_PIX_FMT_VAAPI
                && mUseDrmPrime) {
            deInitDecoderVAAPI();
        }
#endif
        if (mCtx->extradata) {
            av_free(mCtx->extradata);
            mCtx->extradata = NULL;
            mCtx->extradata_size = 0;
        }
        if (mCodecAlreadyOpened) {
            avcodec_close(mCtx);
            mCodecAlreadyOpened = false;
        }
        ffmpeg_hwaccel_deinit(mCtx);
        av_freep(&mCtx);
    }
    if (mFrame) {
        av_frame_free(&mFrame);
        mFrame = NULL;
    }
    if (mPacket) {
        av_packet_free(&mPacket);
        mPacket = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
    mEOSSignalled = false;
    mExtradataReady = false;
    mFilterInitialized = false;
    mPendingWorkQueue.clear();
#if CONFIG_VAAPI
    mBlockPool.reset();
    mSurfaceWidth = -1;
    mSurfaceHeight = -1;
#endif
}

c2_status_t C2FFMPEGVideoDecodeComponent::processCodecConfig(C2ReadView* inBuffer) {
    int orig_extradata_size = mCtx->extradata_size;
    int add_extradata_size = inBuffer->capacity();

#if DEBUG_EXTRADATA
    ALOGD("processCodecConfig: add = %u, current = %d", add_extradata_size, orig_extradata_size);
#endif
    if (! mExtradataReady) {
        mCtx->extradata_size += add_extradata_size;
        mCtx->extradata = (uint8_t *) realloc(mCtx->extradata, mCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (! mCtx->extradata) {
            ALOGE("processCodecConfig: ffmpeg video decoder failed to alloc extradata memory.");
            return C2_NO_MEMORY;
        }
        memcpy(mCtx->extradata + orig_extradata_size, inBuffer->data(), add_extradata_size);
        memset(mCtx->extradata + mCtx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }
    else {
        ALOGW("processCodecConfig: decoder is already opened, ignoring...");
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::sendInputBuffer(
        C2ReadView *inBuffer, int64_t timestamp) {
#if DEBUG_FRAMES
    ALOGD("sendInputBuffer: size=%d, ts=%" PRId64, inBuffer->capacity(), timestamp);
#endif
    if (!mPacket) {
        mPacket = av_packet_alloc();
        if (!mPacket) {
            ALOGE("sendInputBuffer: oom for video packet");
            return C2_NO_MEMORY;
        }
    }

    mPacket->data = inBuffer ? const_cast<uint8_t *>(inBuffer->data()) : NULL;
    mPacket->size = inBuffer ? inBuffer->capacity() : 0;
    mPacket->pts = timestamp;
    mPacket->dts = AV_NOPTS_VALUE;

    int err = avcodec_send_packet(mCtx, mPacket);
    av_packet_unref(mPacket);

    if (err < 0) {
        ALOGE("sendInputBuffer: failed to send data (%d) to decoder: %s (%08x)",
              inBuffer->capacity(), av_err2str(err), err);
        if (err == AVERROR(EAGAIN)) {
            // Frames must be read first, notify main decoding loop.
            ALOGD("sendInputBuffer: returning C2_BAD_STATE");
            return C2_BAD_STATE;
        }
        // Otherwise don't send error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::deinterlaceFrame(bool* hasPicture) {
    int err = 0;

    // Check filter graph
    if (mFilterGraph && mFilterInitialized) {
        FilterSettings *settings = (FilterSettings*)mFilterGraph->opaque;

        if (settings->width != mFrame->width
                || settings->height != mFrame->height
                || settings->format != mFrame->format) {
            av_freep(&mFilterGraph->opaque);
            avfilter_graph_free(&mFilterGraph);
            mFilterSrcCtx = mFilterSinkCtx = NULL;
            mFilterInitialized = false;
        }
    }

    // Setup filter graph.
    if (!mFilterGraph) {
        const AVFilter *buffersrc = avfilter_get_by_name("buffer");
        const AVFilter *buffersink = avfilter_get_by_name("buffersink");
        AVFilterInOut *inputs = NULL;
        AVFilterInOut *outputs = NULL;
        enum AVPixelFormat pix_fmts[] = { (enum AVPixelFormat)mFrame->format, AV_PIX_FMT_NONE };
        std::string args;
        FilterSettings *settings;

        // If time_base is not known, assume MPEG-like codec.
        // A valid time_base is required to setup the filter.
        if (mCtx->time_base.num == 0) {
            mCtx->time_base.num = 1;
            mCtx->time_base.den = 90000;
        }

        // Allocate temporary I/O data structures.
        inputs = avfilter_inout_alloc();
        outputs = avfilter_inout_alloc();
        if (!inputs || !outputs) {
            ALOGE("deinterlaceFrame: oom in filter generation (i/o)");
            err = -ENOMEM;
            goto filterend;
        }

        // Allocate filter graph.
        mFilterGraph = avfilter_graph_alloc();
        if (!mFilterGraph) {
            ALOGE("deinterlaceFrame: oom in filter generation (graph)");
            err = -ENOMEM;
            goto filterend;
        }

        // Store filter settings
        mFilterGraph->opaque = settings = (FilterSettings*)av_mallocz(sizeof(FilterSettings));
        if (!settings) {
            ALOGE("deinterlaceFrame: oom in filter generation (settings)");
            err = -ENOMEM;
            goto filterend;
        }
        settings->width = mFrame->width;
        settings->height = mFrame->height;
        settings->format = mFrame->format;

        // Create filter input source.
        args = base::StringPrintf(
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                 mFrame->width, mFrame->height, mFrame->format,
                 mCtx->time_base.num, mCtx->time_base.den,
                 mCtx->sample_aspect_ratio.num, mCtx->sample_aspect_ratio.den);
        ALOGI("deinterlaceFrame: filter source = %s", args.c_str());
        err = avfilter_graph_create_filter(&mFilterSrcCtx, buffersrc, "in",
                                           args.c_str(), NULL, mFilterGraph);
        if (err < 0) {
            ALOGE("deinterlaceFrame: failed to generate filter (source): %s (%08x)",
                  av_err2str(err), err);
            goto filterend;
        } else {
            AVBufferSrcParameters params = {
                .format = AV_PIX_FMT_NONE, // Don't change pixel format set above
                .hw_frames_ctx = mCtx->hw_frames_ctx
            };

            err = av_buffersrc_parameters_set(mFilterSrcCtx, &params);
            if (err < 0) {
                ALOGE("deinterlaceFrame: failed to generate filter (source params): %s (%08x)",
                      av_err2str(err), err);
                goto filterend;
            }
        }

        // Create filter output sink.
        err = avfilter_graph_create_filter(&mFilterSinkCtx, buffersink, "out",
                                           NULL, NULL, mFilterGraph);
        if (err < 0) {
            ALOGE("deinterlaceFrame: failed to generate filter (sink): %s (%08x)",
                  av_err2str(err), err);
            goto filterend;
        }
        err = av_opt_set_int_list(mFilterSinkCtx, "pix_fmts", pix_fmts,
                                  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        if (err < 0) {
            ALOGE("deinterlaceFrame: failed to generate filter (sink format): %s (%08x)",
                  av_err2str(err), err);
            goto filterend;
        }

        // Connect source to filter (our output is the filter's input).
        outputs->name = av_strdup("in");
        outputs->filter_ctx = mFilterSrcCtx;
        outputs->pad_idx = 0;
        outputs->next = NULL;

        // Connect sink to filter (the filter's output is our input).
        inputs->name = av_strdup("out");
        inputs->filter_ctx = mFilterSinkCtx;
        inputs->pad_idx = 0;
        inputs->next = NULL;

        // Create deinterlace filter.
        args.clear();
#if CONFIG_VAAPI
        if (args.empty() && mFrame->format == AV_PIX_FMT_VAAPI) {
            args = base::StringPrintf(
                    "deinterlace_vaapi=mode=%s",
                    base::GetProperty("debug.ffmpeg-codec2.deinterlace.vaapi", "default").c_str());
        }
#endif
        if (args.empty()) {
            args = base::StringPrintf(
                    "yadif=deint=interlaced:parity=auto");
        }
        ALOGI("deinterlaceFrame: filter graph = %s", args.c_str());
        err = avfilter_graph_parse_ptr(mFilterGraph, args.c_str(),
                                       &inputs, &outputs, NULL);
        if (err < 0) {
            ALOGE("deinterlaceFrame: failed to generate filter (graph): %s (%08x)",
                  av_err2str(err), err);
            goto filterend;
        }
        err = avfilter_graph_config(mFilterGraph, NULL);
        if (err < 0) {
            ALOGE("deinterlaceFrame: failed to generate filter (config): %s (%08x)",
                  av_err2str(err), err);
            goto filterend;
        }

        mFilterInitialized = true;

filterend:
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
    }

    // Process/Filter frame.
    if (mFilterInitialized) {
        // Feed frame to the filter graph.
        err = av_buffersrc_add_frame_flags(mFilterSrcCtx, mFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_unref(mFrame);
        if (err == 0) {
            // Read from from filter graph.
            err = av_buffersink_get_frame(mFilterSinkCtx, mFrame);
            if (err == 0) {
                *hasPicture = true;
            } else if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
                *hasPicture = false;
            } else {
                // Don't send error to client, skip frame!
                ALOGE("deinterlaceFrame: failed to filter frame (output): %s, (%08x)",
                      av_err2str(err), err);
            }
        } else {
            // Don't send error to client, skip frame!
            ALOGE("deinterlaceFrame: failed to filter frame (input): %s (%08x)",
                  av_err2str(err), err);
        }
    } else {
        // Don't send error to client, don't deinterlace
        *hasPicture = true;
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::receiveFrame(bool* hasPicture) {
    int err = avcodec_receive_frame(mCtx, mFrame);
    c2_status_t c2err;

    *hasPicture = false;
    if (err == 0) {
#if DEBUG_FRAMES && CONFIG_VAAPI
        if (mFrame->format == AV_PIX_FMT_VAAPI) {
            ALOGD("receiveFrame: VASurfaceID = %p", mFrame->data[3]);
        }
#endif
        // Update deinterlace indicator during the first 30 frames. We don't expect
        // interlace status to change mid-stream, but there has been instances of progressive
        // streams with sporadic interlaced frames. After the initial period, the interlace
        // status is frozen.
        if (mCtx->frame_number <= 30) {
            mDeinterlaceIndicator += (mFrame->interlaced_frame ? 1 : -1);
#if DEBUG_FRAMES
            ALOGD("receiveFrame: deinterlace indicator = %d", mDeinterlaceIndicator);
#endif
        } else if (mFilterGraph && mDeinterlaceIndicator < 0) {
            // Deinterlace filter was incorrectly initialized.
            ALOGW("receiveFrame: releasing deinterlace filter, as content is not interlaced");
            av_freep(&mFilterGraph->opaque);
            avfilter_graph_free(&mFilterGraph);
            mFilterSrcCtx = mFilterSinkCtx = NULL;
            mFilterInitialized = false;
        }
        // Handle deinterlace and HW frame download
        // - if use HW deinterlace: deinterlace => download
        // - else if use SW deinterlace: download => deinterlace
        // - else: download
        if (mDeinterlaceMode != DEINTERLACE_MODE_NONE && mDeinterlaceIndicator > 0) {
            bool canDeinterlaceInHW =
#if CONFIG_VAAPI
                mFrame->format == AV_PIX_FMT_VAAPI ||
#endif
                false;
            // Check whether deinterlacing should be done in HW context
            if (mDeinterlaceMode == DEINTERLACE_MODE_AUTO && canDeinterlaceInHW) {
                c2err = deinterlaceFrame(hasPicture);
                if (c2err == C2_OK && *hasPicture) {
                    c2err = downloadFrame(false);
                    if (c2err != C2_OK) {
                        // Don't send error to client, skip frame!
                        *hasPicture = false;
                    }
                } else if (c2err != C2_OK) {
                    // Don't send error to client, skip frame!
                    *hasPicture = false;
                }
            }
            // Otherwise handle deinterlacing in SW context
            // NOTE: Not sure whether sw-deinterlacer would handle y-tiled correctly, so don't use it.
            else {
                c2err = downloadFrame(true);
                if (c2err == C2_OK) {
                    c2err = deinterlaceFrame(hasPicture);
                    if (c2err != C2_OK) {
                        // Don't send error to client, skip frame!
                        *hasPicture = false;
                    }
                } else {
                    // Don't send error to client, skip frame!
                    *hasPicture = false;
                }
            }
        } else {
            c2err = downloadFrame(false);
            if (c2err == C2_OK) {
                *hasPicture = true;
            } else {
                // Don't send error to client, skip frame!
                *hasPicture = false;
            }
        }
    } else if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
        ALOGE("receiveFrame: failed to receive frame from decoder: %s (%08x)",
              av_err2str(err), err);
        // Don't report error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::downloadFrame(bool forceSw) {
    // Don't do anything if the frame is no HW accel.
    if (!mFrame->hw_frames_ctx) {
        return C2_OK;
    }

#if CONFIG_VAAPI
    if (!forceSw
            && mFrame->format == AV_PIX_FMT_VAAPI
            && mUseDrmPrime) {
        return C2_OK;
    }
#endif

    int err = ffmpeg_hwaccel_get_frame(mCtx, mFrame);
    if (err < 0) {
        ALOGE("downloadFrame: failed to receive frame from HW decoder: %s (%08x)",
              av_err2str(err), err);
        return C2_CORRUPTED;
    }

    return C2_OK;
}

std::shared_ptr<C2Buffer> C2FFMPEGVideoDecodeComponent::getOutputBuffer(const std::shared_ptr<C2BlockPool>& pool) {
#if CONFIG_VAAPI
    if (mFrame->format == AV_PIX_FMT_VAAPI) {
        return getOutputBufferVAAPI();
    }
#endif

    std::shared_ptr<C2GraphicBlock> block;
    c2_status_t err;

    err = pool->fetchGraphicBlock(ALIGN(mFrame->width, 16), ALIGN(mFrame->height, 2), HAL_PIXEL_FORMAT_YV12,
                                  { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);
    if (err != C2_OK) {
        ALOGE("getOutputBuffer: failed to fetch graphic block %d x %d (%#x) err = %d",
              mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12, err);
        return NULL;
    }

    C2GraphicView wView = block->map().get();

    err = wView.error();
    if (err != C2_OK) {
        ALOGE("getOutputBuffer: graphic view map failed err = %d", err);
        return NULL;
    }

    uint8_t* data[4];
    int linesize[4];
    C2PlanarLayout layout = wView.layout();
    struct SwsContext* currentImgConvertCtx = mImgConvertCtx;

    data[0] = wView.data()[C2PlanarLayout::PLANE_Y];
    data[1] = wView.data()[C2PlanarLayout::PLANE_U];
    data[2] = wView.data()[C2PlanarLayout::PLANE_V];
    linesize[0] = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    linesize[1] = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
    linesize[2] = layout.planes[C2PlanarLayout::PLANE_V].rowInc;

    mImgConvertCtx = sws_getCachedContext(currentImgConvertCtx,
           mFrame->width, mFrame->height, (AVPixelFormat)mFrame->format,
           mFrame->width, mFrame->height, AV_PIX_FMT_YUV420P,
           SWS_BICUBIC, NULL, NULL, NULL);
    if (mImgConvertCtx && mImgConvertCtx != currentImgConvertCtx) {
        ALOGD("getOutputBuffer: created video converter - %s => %s",
              av_get_pix_fmt_name((AVPixelFormat)mFrame->format), av_get_pix_fmt_name(AV_PIX_FMT_YUV420P));

    } else if (! mImgConvertCtx) {
        ALOGE("getOutputBuffer: cannot initialize the conversion context");
        return NULL;
    }

    sws_scale(mImgConvertCtx, mFrame->data, mFrame->linesize,
              0, mFrame->height, data, linesize);

    return createGraphicBuffer(std::move(block), C2Rect(mFrame->width, mFrame->height));;
}

c2_status_t C2FFMPEGVideoDecodeComponent::reconfigureOutputDelay(std::vector<std::unique_ptr<C2Param>>& configUpdate) {
    uint32_t outputDelay = mIntf->getOutputDelay();
    uint32_t newOutputDelay = outputDelay;
    c2_status_t err = C2_OK;

    switch (mCtx->codec_id) {
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_H264:
            // Increase output delay step-wise.
            if (outputDelay >= 18u) {
                newOutputDelay = 34u;
            } else if (outputDelay >= 8u) {
                newOutputDelay = 18u;
            } else {
                newOutputDelay = 8u;
            }
            break;
        default:
            // Other codecs use constant output delay.
            break;
    }

    if (newOutputDelay != outputDelay) {
        C2PortActualDelayTuning::output delay(newOutputDelay);
        std::vector<std::unique_ptr<C2SettingResult>> failures;

        err = mIntf->config({ &delay }, C2_MAY_BLOCK, &failures);
        if (err == C2_OK) {
            ALOGD("reconfigureOutputDelay: output delay set to %u", newOutputDelay);
            configUpdate.push_back(C2Param::Copy(delay));
        } else {
            ALOGE("reconfigureOutputDelay: output delay update to %u failed err = %d",
                  newOutputDelay, err);
        }
    }

    return err;
}

static void fillEmptyWork(const std::unique_ptr<C2Work>& work) {
    work->worklets.front()->output.flags =
        (C2FrameData::flags_t)(work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
    work->result = C2_OK;
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: drop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
}

static bool comparePendingWork(const PendingWork& w1, const PendingWork& w2) {
    return w1.second < w2.second;
}

void C2FFMPEGVideoDecodeComponent::pushPendingWork(const std::unique_ptr<C2Work>& work) {
    if (mPendingWorkQueue.size() >= mIntf->getOutputDelay()) {
        std::vector<std::unique_ptr<C2Param>> configUpdate;
        auto fillEmptyWorkWithConfigUpdate = [&configUpdate](const std::unique_ptr<C2Work>& work) {
            fillEmptyWork(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
        };

        reconfigureOutputDelay(configUpdate);
        finish(mPendingWorkQueue.front().first, fillEmptyWorkWithConfigUpdate);
        mPendingWorkQueue.pop_front();
    }
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: push idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    mPendingWorkQueue.push_back(PendingWork(work->input.ordinal.frameIndex.peeku(),
                                            work->input.ordinal.timestamp.peeku()));
    std::sort(mPendingWorkQueue.begin(), mPendingWorkQueue.end(), comparePendingWork);
}

void C2FFMPEGVideoDecodeComponent::popPendingWork(const std::unique_ptr<C2Work>& work) {
    uint64_t index = work->input.ordinal.frameIndex.peeku();
    auto it = std::find_if(mPendingWorkQueue.begin(), mPendingWorkQueue.end(),
                           [index](const PendingWork& pWork) { return index == pWork.first; });

#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: pop idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif

    if (it != mPendingWorkQueue.end()) {
        mPendingWorkQueue.erase(it);
    }
#if DEBUG_WORKQUEUE
    else {
        ALOGD("WorkQueue: pop work not found idx=%" PRIu64 ", ts=%" PRIu64,
              work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
    }
#endif
    prunePendingWorksUntil(work);
}

void C2FFMPEGVideoDecodeComponent::prunePendingWorksUntil(const std::unique_ptr<C2Work>& work) {
#if DEBUG_WORKQUEUE
    ALOGD("WorkQueue: prune until idx=%" PRIu64 ", ts=%" PRIu64,
          work->input.ordinal.frameIndex.peeku(), work->input.ordinal.timestamp.peeku());
#endif
    // Drop all works with a PTS earlier than provided argument.
    while (mPendingWorkQueue.size() > 0 &&
           mPendingWorkQueue.front().second < work->input.ordinal.timestamp.peeku()) {
        finish(mPendingWorkQueue.front().first, fillEmptyWork);
        mPendingWorkQueue.pop_front();
    }
}

c2_status_t C2FFMPEGVideoDecodeComponent::onInit() {
    ALOGD("onInit");
    return initDecoder();
}

c2_status_t C2FFMPEGVideoDecodeComponent::onStop() {
    ALOGD("onStop");
    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::onReset() {
    ALOGD("onReset");
    deInitDecoder();
    initDecoder();
}

void C2FFMPEGVideoDecodeComponent::onRelease() {
    ALOGD("onRelease");
    deInitDecoder();
}

c2_status_t C2FFMPEGVideoDecodeComponent::onFlush_sm() {
    ALOGD("onFlush_sm");
    if (mCtx && avcodec_is_open(mCtx)) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        avcodec_flush_buffers(mCtx);
        mEOSSignalled = false;
    }
    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::outputFrame(
    const std::unique_ptr<C2Work>& work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    c2_status_t err;
    std::vector<std::unique_ptr<C2Param>> configUpdate;

#if DEBUG_FRAMES
#if CONFIG_VAAPI
    if (mFrame->format == AV_PIX_FMT_VAAPI) {
        ALOGD("outputFrame: VASurfaceID = %p", mFrame->data[3]);
    }
#endif
    ALOGD("outputFrame: pts=%" PRId64 " dts=%" PRId64 " ts=%" PRId64 " - %d x %d (%#x)",
          mFrame->pts, mFrame->pkt_dts, mFrame->best_effort_timestamp, mFrame->width, mFrame->height, mFrame->format);
#endif

    if (mFrame->width != mIntf->getWidth() || mFrame->height != mIntf->getHeight()) {
        ALOGD("outputFrame: video params changed - %d x %d (%#x)", mFrame->width, mFrame->height, mFrame->format);

        C2StreamPictureSizeInfo::output size(0u, mFrame->width, mFrame->height);
        std::vector<std::unique_ptr<C2SettingResult>> failures;

        err = mIntf->config({ &size }, C2_MAY_BLOCK, &failures);
        if (err == OK) {
            configUpdate.push_back(C2Param::Copy(size));
            mCtx->width = mFrame->width;
            mCtx->height = mFrame->height;
        } else {
            ALOGE("outputFrame: config update failed err = %d", err);
            return C2_CORRUPTED;
        }
    }

#if CONFIG_VAAPI
    if (mFrame->format == AV_PIX_FMT_VAAPI && mIntf->getPixelFormat() != HAL_PIXEL_FORMAT_YCbCr_420_888) {
        ALOGD("outputFrame: pixel format changed - %#x", HAL_PIXEL_FORMAT_YCbCr_420_888);

        C2StreamPixelFormatInfo::output format(0u, HAL_PIXEL_FORMAT_YCbCr_420_888);
        std::vector<std::unique_ptr<C2SettingResult>> failures;

        err = mIntf->config({ &format }, C2_MAY_BLOCK, &failures);
        if (err == C2_OK) {
            configUpdate.push_back(C2Param::Copy(format));
        } else {
            ALOGE("outputFrame: config update failed err = %d", err);
            return C2_CORRUPTED;
        }
    }

    if (mHeldSurfaces.size() >= mIntf->getOutputDelay()) {
        err = reconfigureOutputDelay(configUpdate);
        if (err != C2_OK) {
            return C2_CORRUPTED;
        }
    }
#endif

    std::shared_ptr<C2Buffer> buffer = getOutputBuffer(pool);
    if (buffer) {
        buffer->setInfo(mIntf->getPixelFormatInfo());
    }

    if (work && c2_cntr64_t(mFrame->best_effort_timestamp) == work->input.ordinal.frameIndex) {
        prunePendingWorksUntil(work);
        work->worklets.front()->output.configUpdate = std::move(configUpdate);
        work->worklets.front()->output.buffers.clear();
        if (buffer) {
            work->worklets.front()->output.buffers.push_back(buffer);
        }
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
        work->result = C2_OK;
    } else {
        auto fillWork = [buffer, &configUpdate, this](const std::unique_ptr<C2Work>& work) {
            popPendingWork(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
            work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
            work->worklets.front()->output.buffers.clear();
            if(buffer) {
                work->worklets.front()->output.buffers.push_back(buffer);
            }
            work->worklets.front()->output.ordinal = work->input.ordinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
#if DEBUG_FRAMES
            ALOGD("outputFrame: work(finish) idx=%" PRIu64 ", processed=%u, result=%d",
                  work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
        };

        finish(mFrame->best_effort_timestamp, fillWork);
    }

    return C2_OK;
}

void C2FFMPEGVideoDecodeComponent::process(
    const std::unique_ptr<C2Work> &work,
    const std::shared_ptr<C2BlockPool> &pool
) {
    size_t inSize = 0u;
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM);
    C2ReadView rView = mDummyReadView;
    bool hasInputBuffer = false;

    if (! work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        hasInputBuffer = true;
    }

#if DEBUG_FRAMES
    ALOGD("process: input flags=%08x ts=%lu idx=%lu #buf=%lu[%lu] #conf=%lu #info=%lu",
          work->input.flags, work->input.ordinal.timestamp.peeku(), work->input.ordinal.frameIndex.peeku(),
          work->input.buffers.size(), inSize, work->input.configUpdate.size(), work->input.infoBuffers.size());
#endif

    if (mEOSSignalled) {
        ALOGE("process: ignoring work while EOS reached");
        work->workletsProcessed = 0u;
        work->result = C2_BAD_VALUE;
        return;
    }

    if (hasInputBuffer && rView.error()) {
        ALOGE("process: read view map failed err = %d", rView.error());
        work->workletsProcessed = 0u;
        work->result = rView.error();
        return;
    }

#if CONFIG_VAAPI
    if (!mBlockPool) {
        mBlockPool = pool;
    }
#endif

    // In all cases the work is marked as completed.
    //
    // There is not always a 1:1 mapping between input and output frames, in particular for
    // interlaced content. Keeping the corresponding worklets in the queue quickly fills it
    // in and stalls the decoder. But there's no obvious mechanism to determine, from
    // FFMPEG API, whether a given packet will produce an output frame and the worklet should
    // be kept around so it can be completed when the frame is produced.
    //
    // NOTE: This has an impact on the drain operation.

    work->result = C2_OK;
    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->workletsProcessed = 0u;

    if (inSize || (eos && mCodecAlreadyOpened)) {
        c2_status_t err = C2_OK;

        if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
            work->workletsProcessed = 1u;
            work->result = processCodecConfig(&rView);
            return;
        }

        if (! mCodecAlreadyOpened) {
            err = openDecoder();
            if (err != C2_OK) {
                work->workletsProcessed = 1u;
                work->result = err;
                return;
            }
        }

        bool inputConsumed = false;
        bool outputAvailable = true;
        bool hasPicture = false;
#if DEBUG_FRAMES
        int outputFrameCount = 0;
#endif

        while (!inputConsumed || outputAvailable) {
            if (!inputConsumed) {
                err = sendInputBuffer(&rView, work->input.ordinal.frameIndex.peekll());
                if (err == C2_OK) {
                    inputConsumed = true;
                    outputAvailable = true;
                    work->input.buffers.clear();
                } else if (err != C2_BAD_STATE) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }
            }

            if (outputAvailable) {
                hasPicture = false;
                err = receiveFrame(&hasPicture);
                if (err != C2_OK) {
                    work->workletsProcessed = 1u;
                    work->result = err;
                    return;
                }

                if (hasPicture) {
                    err = outputFrame(work, pool);
                    if (err != C2_OK) {
                        work->workletsProcessed = 1u;
                        work->result = err;
                        return;
                    }
#if DEBUG_FRAMES
                    else {
                        outputFrameCount++;
                    }
#endif
                }
                else {
#if DEBUG_FRAMES
                    if (!outputFrameCount) {
                        ALOGD("process: no frame");
                    }
#endif
                    outputAvailable = false;
                }
            }
        }
    }
#if DEBUG_FRAMES
    else {
        ALOGD("process: empty work");
    }
#endif

    if (eos) {
        mEOSSignalled = true;
        work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        work->workletsProcessed = 1u;
    }

    if (work->workletsProcessed == 0u) {
        pushPendingWork(work);
    }

#if DEBUG_FRAMES
    ALOGD("process: work(end) idx=%" PRIu64 ", processed=%u, result=%d",
          work->input.ordinal.frameIndex.peeku(), work->workletsProcessed, work->result);
#endif
}

c2_status_t C2FFMPEGVideoDecodeComponent::drain(
    uint32_t drainMode,
    const std::shared_ptr<C2BlockPool>& pool
) {
    ALOGD("drain: mode = %u", drainMode);

    if (drainMode == NO_DRAIN) {
        ALOGW("drain: NO_DRAIN is no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("drain: DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }
    if (! mCodecAlreadyOpened) {
        ALOGW("drain: codec not opened yet");
        return C2_OK;
    }

    bool hasPicture = false;
    c2_status_t err = C2_OK;

    err = sendInputBuffer(NULL, 0);
    while (err == C2_OK) {
        hasPicture = false;
        err = receiveFrame(&hasPicture);
        if (hasPicture) {
            // Ignore errors at this point, just drain the decoder.
            outputFrame(nullptr, pool);
        } else {
            err = C2_NOT_FOUND;
        }
    }

    return C2_OK;
}

#if CONFIG_VAAPI

// Implement a buffer pool for FFMPEG backed by an Android IGraphicBufferProducer (IGBP).
// Buffers are requested on-demand from C2BlockPool and mapped to VA surfaces using their
// DRM prime handle. Decoded frame data can then be returned directly to framework.
//
// Because FFMPEG buffer/surface management is different than IGBP, some bookkeeping is
// required in order to maintain consistency between FFMPEG and Android. When a buffer is
// needed, C2BlockPool::fetchGraphicBlock is used to generated a buffer from gralloc. The
// buffer is identified by its IGBP slot number. If the buffer is unknown, a VA surface is
// created from its DRM prime handle, otherwise the existing VA surface is reused. If the
// buffer and VA surface size do not match, the VA surface is destroyed and recreated.
// This can happen for instance during adaptive playback.
//
// When FFMPEG returns a decoded VA surface, the corresponding C2GraphicBlock is returned
// to framework. However the surface is held in memory until FFMPEG actually releases it.
// If the same IGBP slot is returned by fetchGraphicBlock before FFMPEG has released the
// surface, the new block cannot be used right away and is stored into a pool of pending
// blocks, until FFMPEG releases the corresponding VA surface. The pending block is then
// marked as available and used on the next buffer request.
//
// In order to use VA-API also for filtering (deinterlacing), the FFMPEG HW device is
// hot-patched to inject custom frame management methods (there is not equivalent to
// AVCodecContext.get_buffer2 in libavfilter). As we don't have any control on which IGBP
// slot is returned in specific contexts (decoding or filtering), we must make sure all
// VA surfaces are interchangeable and usabled in both contexts. Hence all surfaces must
// have the same dimension and use coded_width/coded_height fields from AVCodecContext.
//
// When the number of held VA surfaces reaches the codec current output delay, the tuning
// parameter is increased to increase the size of the underlying IGBP. The codec2 framework
// uses a margin of 7~8 buffers on top of configured output delay, so it's unlikely the
// codec will stall.

void C2FFMPEGVideoDecodeComponent::SurfaceDescriptor::set(const std::shared_ptr<C2GraphicBlock>& block) {
    android::_UnwrapNativeCodec2GrallocMetadata(block->handle(),
            &width, &height, &format, &usage, &stride, &generation, &igbpId, &igbpSlot);
}

int C2FFMPEGVideoDecodeComponent::getBufferVAAPI(AVHWFramesContext* hwfc, AVFrame* frame) {
    if (!mBlockPool) {
        ALOGE("getBufferVAAPIi[%p]: block pool does not exist.", hwfc);
        return AVERROR(ENOSYS);
    }

    AVVAAPIDeviceContext* hwctx = (AVVAAPIDeviceContext*)hwfc->device_ctx->hwctx;

    if (mCtx->coded_width != mSurfaceWidth || mCtx->coded_height != mSurfaceHeight) {
        ALOGD("getBufferVAAPI[%p]: set surface dimension to %d x %d, surfaces = %zd, held = %zd, pending = %zd, available = %zd",
              hwfc, mCtx->coded_width, mCtx->coded_height, mSurfaces.size(), mHeldSurfaces.size(), mPendingSurfaces.size(), mAvailableSurfaces.size());
        mPendingSurfaces.clear();
        mAvailableSurfaces.clear();
        mSurfaceWidth = mCtx->coded_width;
        mSurfaceHeight = mCtx->coded_height;
    }

    c2_status_t err;
    std::shared_ptr<C2GraphicBlock> block;
    SurfaceDescriptor desc;

    while (!block) {
        if (mAvailableSurfaces.empty()) {
            err = mBlockPool->fetchGraphicBlock(mSurfaceWidth, mSurfaceHeight, HAL_PIXEL_FORMAT_YCbCr_420_888,
                                                { mIntf->getConsumerUsage(), (uint64_t)BufferUsage::VIDEO_DECODER }, &block);
            if (err != C2_OK) {
                ALOGE("getBufferVAAPI[%p]: failed to fetch graphic block %d x %d (%#x) err = %d",
                      hwfc, mSurfaceWidth, mSurfaceHeight, HAL_PIXEL_FORMAT_YCbCr_420_888, err);
                return AVERROR(ENOMEM);
            }
            desc.set(block);

            auto s_it = mSurfaces.find(desc.getId());
            if (s_it != mSurfaces.end()) {
                auto h_it = mHeldSurfaces.find(s_it->second.surfaceId);
                if (h_it != mHeldSurfaces.end()) {
                    if (h_it->second || mPendingSurfaces.find(h_it->first) != mPendingSurfaces.end()) {
                        LOG_ALWAYS_FATAL("getBufferVAAPI[%p]: surface %#x already has a graphic block.",
                                         hwfc, h_it->first);
                    } else {
#if DEBUG_FRAMES
                        ALOGD("getBufferVAAPI[%p]: saving pending block for surface %#x.",
                              hwfc, h_it->first);
#endif
                        mPendingSurfaces.emplace(h_it->first, std::move(block));
                    }
                }
            }
        } else {
            auto a_it = mAvailableSurfaces.begin();
#if DEBUG_FRAMES
            ALOGD("getBufferVAAPI[%p]: using available block for surface %#x.", hwfc, a_it->first);
#endif
            block = std::move(a_it->second);
            mAvailableSurfaces.erase(a_it);
            desc.set(block);
        }
    }

    VASurfaceID surfaceId;
    VAStatus vas;

    auto s_it = mSurfaces.find(desc.getId());
    if (s_it != mSurfaces.end()) {
        if (s_it->second.width != mSurfaceWidth || s_it->second.height != mSurfaceHeight) {
#if DEBUG_FRAMES
            ALOGD("getBufferVAAPI[%p]: destroying incompatible surface %#x (%d x %d) for buffer %d (%d x %d).",
                  hwfc, s_it->second.surfaceId, s_it->second.width, s_it->second.height,
                  desc.getId(), desc.width, desc.height);
#endif
            vas = vaDestroySurfaces(hwctx->display, &s_it->second.surfaceId, 1);
            if (vas != VA_STATUS_SUCCESS) {
                ALOGE("getBufferVAAPI[%p]: failed to destroy surface %#x: %s (%d).",
                      hwfc, s_it->second.surfaceId, vaErrorStr(vas), vas);
            }
            mSurfaces.erase(s_it);
            s_it = mSurfaces.end();
        }
    }

    if (s_it == mSurfaces.end()) {
        uintptr_t bufferPrimeFd = block->handle()->data[0];
        VASurfaceAttribExternalBuffers descriptor;
        VASurfaceAttrib attributes[2] = {
            {
                .type = VASurfaceAttribMemoryType,
                .flags = VA_SURFACE_ATTRIB_SETTABLE,
                .value.type = VAGenericValueTypeInteger,
                .value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME,
            },
            {
                .type = VASurfaceAttribExternalBufferDescriptor,
                .flags = VA_SURFACE_ATTRIB_SETTABLE,
                .value.type = VAGenericValueTypePointer,
                .value.value.p = &descriptor
            }
        };

        descriptor.pixel_format = VA_FOURCC_NV12;
        descriptor.width = mSurfaceWidth;
        descriptor.height = mSurfaceHeight;
        descriptor.num_buffers = 1;
        descriptor.buffers = &bufferPrimeFd;
        descriptor.flags = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
        descriptor.num_planes = 2;
        descriptor.pitches[0] = desc.stride;
        descriptor.pitches[1] = desc.stride;
        descriptor.pitches[2] = 0;
        descriptor.pitches[3] = 0;
        descriptor.offsets[0] = 0;
        descriptor.offsets[1] = desc.stride * ALIGN(desc.height, 32);
        descriptor.offsets[2] = 0;
        descriptor.offsets[3] = 0;
        descriptor.data_size = descriptor.offsets[1] + desc.stride * ALIGN(desc.height / 2, 32);
        descriptor.private_data = NULL;

        vas = vaCreateSurfaces(hwctx->display, VA_RT_FORMAT_YUV420,
                               mSurfaceWidth, mSurfaceHeight, &surfaceId, 1, attributes, 2);

        if (vas != VA_STATUS_SUCCESS) {
            ALOGE("getBufferVAAPI[%p]: failed to allocate VA surface (%d x %d): %s (%d).",
                  hwfc, mSurfaceWidth, mSurfaceHeight, vaErrorStr(vas), vas);
            return AVERROR(ENOMEM);
        }

#if DEBUG_FRAMES
        ALOGD("getBufferVAAPI[%p]: created surface %#x (%d x %d) for buffer %d",
              hwfc, surfaceId, mSurfaceWidth, mSurfaceHeight, desc.getId());
#endif

        desc.surfaceId = surfaceId;
        mSurfaces.emplace(desc.getId(), desc);
    } else {
        surfaceId = s_it->second.surfaceId;
    }

    frame->buf[0] = av_buffer_create((uint8_t*)(uintptr_t)surfaceId, 0,
                                     framesReleaseBufferVAAPI, this, 0);
    if (!frame->buf[0]) {
        ALOGE("getBufferVAAPI[%p]: failed to allocate FFMPEG buffer.", hwfc);
        return AVERROR(ENOMEM);
    }

    frame->data[3] = frame->buf[0]->data;
    frame->format = AV_PIX_FMT_VAAPI;
    frame->width = hwfc->width;
    frame->height = hwfc->height;

    mHeldSurfaces.emplace(surfaceId, std::move(block));

#if DEBUG_FRAMES
    ALOGD("getBufferVAAPI[%p]: using surface %#x (%d x %d) for buffer %d, held = %zd",
          hwfc, surfaceId, mSurfaceWidth, mSurfaceHeight, desc.getId(), mHeldSurfaces.size());
#endif

    return 0;
}

void C2FFMPEGVideoDecodeComponent::releaseBufferVAAPI(VASurfaceID surfaceId) {
    auto it = mHeldSurfaces.find(surfaceId);

    if (it == mHeldSurfaces.end()) {
        LOG_ALWAYS_FATAL("releaseBufferVAAPI: invalid surface %#x.", surfaceId);
    }
    mHeldSurfaces.erase(it);

#if DEBUG_FRAMES
    ALOGD("releaseBufferVAAPI: released surface %#x.", surfaceId);
#endif

    auto p_it = mPendingSurfaces.find(surfaceId);

    if (p_it != mPendingSurfaces.end()) {
#if DEBUG_FRAMES
        ALOGD("releaseBufferVAAPI: pending block for surface %#x is now available.", surfaceId);
#endif
        mAvailableSurfaces.emplace(surfaceId, std::move(p_it->second));
        mPendingSurfaces.erase(p_it);
    }
}

std::shared_ptr<C2Buffer> C2FFMPEGVideoDecodeComponent::getOutputBufferVAAPI() {
    AVHWFramesContext* hwfc = (AVHWFramesContext*)mFrame->hw_frames_ctx->data;
    AVVAAPIDeviceContext* hwctx = (AVVAAPIDeviceContext*)hwfc->device_ctx->hwctx;
    VASurfaceID surfaceId = (uintptr_t)mFrame->data[3];
    std::shared_ptr<C2GraphicBlock> block;

    auto it = mHeldSurfaces.find(surfaceId);
    if (it == mHeldSurfaces.end()) {
        LOG_ALWAYS_FATAL("getOutputBufferVAAPI: invalid surface %#x.", surfaceId);
    } else if (!it->second) {
        LOG_ALWAYS_FATAL("getOutputBufferVAAPI: surface %#x does not have a graphic block.", surfaceId);
    }

    vaSyncSurface(hwctx->display, surfaceId);
    block = std::move(it->second);

    return createGraphicBuffer(std::move(block), C2Rect(mFrame->width, mFrame->height));
}

void C2FFMPEGVideoDecodeComponent::deInitDecoderVAAPI() {
    AVHWFramesContext* hwfc = (AVHWFramesContext*)mCtx->hw_frames_ctx->data;
    AVVAAPIDeviceContext* hwctx = (AVVAAPIDeviceContext*)hwfc->device_ctx->hwctx;
    VAStatus vas;

    mHeldSurfaces.clear();
    mPendingSurfaces.clear();
    mAvailableSurfaces.clear();
    for (auto& entry : mSurfaces) {
        vas = vaDestroySurfaces(hwctx->display, &entry.second.surfaceId, 1);
        if (vas != VA_STATUS_SUCCESS) {
            ALOGE("deInitDecoderVAAPI: failed to destroy surface %#x: %s (%d).",
                  entry.second.surfaceId, vaErrorStr(vas), vas);
        }
#if DEBUG_FRAMES
        else {
            ALOGD("deInitDecoderVAAPI: destroyed surface %#x.", entry.second.surfaceId);
        }
#endif
    }
    mSurfaces.clear();
}

extern "C" struct VAAPIHWContextType {
    HWContextType hw_type;
    const HWContextType* parent_hw_type;
    void(*parent_free)(AVHWDeviceContext* ctx);
    C2FFMPEGVideoDecodeComponent* component;
};

static void freeVAAPIHWContextType(AVHWDeviceContext* ctx) {
    const VAAPIHWContextType* type = (VAAPIHWContextType*)ctx->internal->hw_type;

    ctx->internal->hw_type = type->parent_hw_type;
    ctx->free = type->parent_free;
    ctx->free(ctx);
    av_freep(&type);
}

static int framesInit(AVHWFramesContext* ctx) {
    ctx->initial_pool_size = 0;
    ctx->pool = (AVBufferPool*)1;
    return 0;
}

void C2FFMPEGVideoDecodeComponent::openDecoderVAAPI() {
    AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)mCtx->hw_device_ctx->data;
    VAAPIHWContextType* type = (VAAPIHWContextType*)av_mallocz(sizeof(VAAPIHWContextType));

    type->hw_type = *device_ctx->internal->hw_type;
    type->hw_type.frames_get_buffer = framesGetBufferVAAPI;
    type->hw_type.frames_init = framesInit;
    type->hw_type.frames_uninit = NULL;
    type->parent_hw_type = device_ctx->internal->hw_type;
    type->parent_free = device_ctx->free;
    type->component = this;

    device_ctx->internal->hw_type = &type->hw_type;
    device_ctx->free = freeVAAPIHWContextType;
}

int C2FFMPEGVideoDecodeComponent::framesGetBufferVAAPI(AVHWFramesContext* ctx, AVFrame* frame) {
    const VAAPIHWContextType* type = (VAAPIHWContextType*)ctx->device_ctx->internal->hw_type;
    return type->component->getBufferVAAPI(ctx, frame);
}

void C2FFMPEGVideoDecodeComponent::framesReleaseBufferVAAPI(void* opaque, uint8_t* data) {
    C2FFMPEGVideoDecodeComponent* component = (C2FFMPEGVideoDecodeComponent*)opaque;
    component->releaseBufferVAAPI((uintptr_t)data);
}

#endif

} // namespace android
