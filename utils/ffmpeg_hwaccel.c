#define DEBUG_HWACCEL 0
#define LOG_TAG "HWACCEL"
#include <cutils/log.h>
#include <cutils/properties.h>

#include "config.h"
#include "ffmpeg_hwaccel.h"
#include "libavutil/hwcontext.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

typedef struct {
    const char* name;
    enum AVPixelFormat pix_fmt;
    enum AVHWDeviceType device_type;
} HWAccel;

const HWAccel hwaccels[] = {
#if CONFIG_VAAPI
    { "vaapi", AV_PIX_FMT_VAAPI, AV_HWDEVICE_TYPE_VAAPI },
#endif
    { NULL, AV_PIX_FMT_NONE, AV_HWDEVICE_TYPE_NONE },
};

static const HWAccel *get_hwaccel(enum AVPixelFormat pix_fmt) {
    int i;

    for (i = 0; hwaccels[i].name; i++)
        if (hwaccels[i].pix_fmt == pix_fmt)
            return &hwaccels[i];
    return NULL;
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    int ret = -1;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        ALOGD_IF(DEBUG_HWACCEL, "check %td pix_fmts=%d", p - pix_fmts, *p);
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const HWAccel *hwaccel;

        // Hw-accel pixel formats are expected to be listed first. When encountering
        // the first non hw-accel, all hw codecs have been exhausted, so use this format.
        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        // Find hw-accel for this pixel format.
        hwaccel = get_hwaccel(*p);
        if (!hwaccel)
            // Skip if there's no hw-accel for this pixel format.
            continue;

        // Initialize hw device for this hw-accel.
        ret = av_hwdevice_ctx_create(&s->hw_device_ctx, hwaccel->device_type, "android", NULL, 0);
        if (ret < 0) {
            // Hw-accel device could not be initialized, skip.
            continue;
        }

        // Multi-threaded hw decoding is not supported well.
        s->thread_count = 1;

        // Hw-accel found and initialized, stop here.
        break;
    }

    ALOGI("hw codec %s %sabled: s=%p pix_fmts=%d", avcodec_get_name(s->codec_id), ret ? "dis" : "en", s, *p);
    return *p;
}

int ffmpeg_hwaccel_init(AVCodecContext *avctx) {
    if (!property_get_bool("media.sf.hwaccel", 0))
        return 0;

    // Use custom format negotiation.
    avctx->get_format = get_format;
    // Use refcounted frames.
    av_opt_set_int(avctx, "refcounted_frames", 1, 0);

    ALOGD_IF(DEBUG_HWACCEL, "ffmpeg_hwaccel_init ctx=%p", avctx);
    return 0;
}

void ffmpeg_hwaccel_deinit(AVCodecContext *avctx __unused) {
}

int ffmpeg_hwaccel_get_frame(AVCodecContext *avctx __unused, AVFrame *frame) {
    if (!frame->hw_frames_ctx) {
        // Frame is not hw-accel
        return 0;
    }

    AVFrame* output;
    int err;

    output = av_frame_alloc();
    if (!output) {
        return AVERROR(ENOMEM);
    }

    output->format = AV_PIX_FMT_YUV420P;

    err = av_hwframe_transfer_data(output, frame, 0);
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_get_frame failed to transfer data: %s (%08x)",
              av_err2str(err), err);
        goto fail;
    }

    err = av_frame_copy_props(output, frame);
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_get_frame failed to copy frame properties: %s (%08x)",
              av_err2str(err), err);
        goto fail;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, output);
    av_frame_free(&output);

    return 0;

fail:
    av_frame_free(&output);
    return err;
}
