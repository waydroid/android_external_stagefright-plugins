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
#include <log/log.h>
#include <algorithm>

#include <SimpleC2Interface.h>
#include "C2FFMPEGVideoDecodeComponent.h"
#include "ffmpeg_hwaccel.h"

#define DEBUG_FRAMES 0
#define DEBUG_WORKQUEUE 0
#define DEBUG_EXTRADATA 0

#define DEINTERLACE_MODE_NONE 0
#define DEINTERLACE_MODE_SOFTWARE 1
#define DEINTERLACE_MODE_AUTO 2

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
      mFFMPEGInitialized(false),
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
    if (! mFFMPEGInitialized) {
        if (initFFmpeg() != C2_OK) {
            ALOGE("initDecoder: FFMPEG initialization failed.");
            return C2_NO_INIT;
        }
        mFFMPEGInitialized = true;
    }

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

    const FFMPEGVideoCodecInfo* codecInfo = mIntf->getCodecInfo();

    if (codecInfo) {
        ALOGD("initDecoder: use codec info from extractor");
        mCtx->codec_id = (enum AVCodecID)codecInfo->codec_id;
    }

    mDeinterlaceMode = getDeinterlaceMode();
    mDeinterlaceIndicator = 0;

    ALOGD("initDecoder: %p [%s], %d x %d, %s, deinterlace = %d",
          mCtx, avcodec_get_name(mCtx->codec_id), size.width, size.height, mInfo->mediaType,
          mDeinterlaceMode);

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

        if (settings->width != mFrame->width || settings->height != mFrame->height || settings->format != mFrame->format) {
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
        if (mFrame->format == AV_PIX_FMT_VAAPI) {
            args = base::StringPrintf(
                    "deinterlace_vaapi=mode=%s",
                    base::GetProperty("debug.ffmpeg-codec2.deinterlace.vaapi", "default").c_str());
        } else {
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

    *hasPicture = false;
    if (err == 0) {
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
            // Check whether deinterlacing should be done in HW context
            if (mDeinterlaceMode == DEINTERLACE_MODE_AUTO && mFrame->format == AV_PIX_FMT_VAAPI) {
                err = deinterlaceFrame(hasPicture);
                if (err == 0 && *hasPicture) {
                    err = ffmpeg_hwaccel_get_frame(mCtx, mFrame);
                    if (err < 0) {
                        ALOGE("receiveFrame: failed to receive frame from HW decoder: %s (%08x)",
                              av_err2str(err), err);
                        // Don't send error to client, skip frame!
                        *hasPicture = false;
                    }
                } else if (err < 0) {
                    ALOGE("receiveFrame: failed to deinterlace frame: %s (%08x)",
                          av_err2str(err), err);
                    // Don't send error to client, skip frame!
                }
            }
            // Otherwise handle deinterlacing in SW context
            // NOTE: Not sure whether sw-deinterlacer would handle y-tiled correctly, so don't use it.
            else {
                err = ffmpeg_hwaccel_get_frame(mCtx, mFrame);
                if (err == 0) {
                    err = deinterlaceFrame(hasPicture);
                    if (err < 0) {
                        ALOGE("receiveFrame: failed to deinterlace frame: %s (%08x)",
                              av_err2str(err), err);
                        // Don't send error to client, skip frame!
                        *hasPicture = false;
                    }
                } else {
                    ALOGE("receiveFrame: failed to receive frame from HW decoder: %s (%08x)",
                          av_err2str(err), err);
                    // Don't send error to client, skip frame!
                }
            }
        } else {
            err = ffmpeg_hwaccel_get_frame(mCtx, mFrame);
            if (err == 0) {
                *hasPicture = true;
            } else {
                ALOGE("receiveFrame: failed to receive frame from HW decoder: %s (%08x)",
                      av_err2str(err), err);
                // Don't send error to client, skip frame!
            }
        }
    } else if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
        ALOGE("receiveFrame: failed to receive frame from decoder: %s (%08x)",
              av_err2str(err), err);
        // Don't report error to client.
    }

    return C2_OK;
}

c2_status_t C2FFMPEGVideoDecodeComponent::getOutputBuffer(C2GraphicView* outBuffer) {
    uint8_t* data[4];
    int linesize[4];
    C2PlanarLayout layout = outBuffer->layout();
    struct SwsContext* currentImgConvertCtx = mImgConvertCtx;

    data[0] = outBuffer->data()[C2PlanarLayout::PLANE_Y];
    data[1] = outBuffer->data()[C2PlanarLayout::PLANE_U];
    data[2] = outBuffer->data()[C2PlanarLayout::PLANE_V];
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
        return C2_NO_MEMORY;
    }

    sws_scale(mImgConvertCtx, mFrame->data, mFrame->linesize,
              0, mFrame->height, data, linesize);

    return C2_OK;
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
    uint32_t outputDelay = mIntf->getOutputDelay();

    if (mPendingWorkQueue.size() >= outputDelay) {
        uint32_t newOutputDelay = outputDelay;
        std::vector<std::unique_ptr<C2Param>> configUpdate;

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
            int err;

            err = mIntf->config({ &delay }, C2_MAY_BLOCK, &failures);
            if (err == C2_OK) {
                ALOGD("WorkQueue: queue full, output delay set to %u", newOutputDelay);
                configUpdate.push_back(C2Param::Copy(delay));
            } else {
                ALOGE("WorkQueue: output delay update to %u failed err = %d",
                      newOutputDelay, err);
            }
        }

        auto fillEmptyWorkWithConfigUpdate = [&configUpdate](const std::unique_ptr<C2Work>& work) {
            fillEmptyWork(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
        };

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
    if (mFFMPEGInitialized) {
        deInitFFmpeg();
        mFFMPEGInitialized = false;
    }
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
    ALOGD("outputFrame: pts=%" PRId64 " dts=%" PRId64 " ts=%" PRId64 " - %d x %d (%x)",
          mFrame->pts, mFrame->pkt_dts, mFrame->best_effort_timestamp, mFrame->width, mFrame->height, mFrame->format);
#endif

    if (mFrame->width != mIntf->getWidth() || mFrame->height != mIntf->getHeight()) {
        ALOGD("outputFrame: video params changed - %d x %d (%x)", mFrame->width, mFrame->height, mFrame->format);

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

    std::shared_ptr<C2GraphicBlock> block;

    err = pool->fetchGraphicBlock(mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12,
                                  { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE }, &block);

    if (err != C2_OK) {
        ALOGE("outputFrame: failed to fetch graphic block %d x %d (%x) err = %d",
              mFrame->width, mFrame->height, HAL_PIXEL_FORMAT_YV12, err);
        return C2_CORRUPTED;
    }

    C2GraphicView wView = block->map().get();

    err = wView.error();
    if (err != C2_OK) {
        ALOGE("outputFrame: graphic view map failed err = %d", err);
        return C2_CORRUPTED;
    }

    err = getOutputBuffer(&wView);
    if (err == C2_OK) {
        std::shared_ptr<C2Buffer> buffer = createGraphicBuffer(std::move(block), C2Rect(mFrame->width, mFrame->height));

        buffer->setInfo(mIntf->getPixelFormatInfo());

        if (work && c2_cntr64_t(mFrame->best_effort_timestamp) == work->input.ordinal.frameIndex) {
            prunePendingWorksUntil(work);
            work->worklets.front()->output.configUpdate = std::move(configUpdate);
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.buffers.push_back(buffer);
            work->worklets.front()->output.ordinal = work->input.ordinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
        } else {
            auto fillWork = [buffer, &configUpdate, this](const std::unique_ptr<C2Work>& work) {
                popPendingWork(work);
                work->worklets.front()->output.configUpdate = std::move(configUpdate);
                work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
                work->worklets.front()->output.buffers.clear();
                work->worklets.front()->output.buffers.push_back(buffer);
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
    } else {
        return err;
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

} // namespace android
