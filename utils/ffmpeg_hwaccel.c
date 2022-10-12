#define DEBUG_HWACCEL 0
#define DEBUG_HWACCEL_DRM 0
#define DEBUG_HWACCEL_DRM_FRAMES 0
#define LOG_TAG "HWACCEL"
#include <cutils/log.h>
#include <cutils/properties.h>
#include <sys/mman.h>
#include <time.h>

#include "config.h"
#include "ffmpeg_hwaccel.h"
#include "libavutil/hwcontext.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/hwcontext_drm.h"

#if DEBUG_HWACCEL_DRM_FRAMES
#include <fcntl.h>
#endif

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

typedef struct HWAccelMap {
    AVFrame* source;
    int nb_objects;
    void* address[AV_DRM_MAX_PLANES];
    size_t length[AV_DRM_MAX_PLANES];
    int object[AV_DRM_MAX_PLANES];
} HWAccelMap;

static void ffmpeg_hwaccel_unmap_from_drm(void* opaque __unused, uint8_t* data) {
    HWAccelMap* map = (HWAccelMap*)data;
#if DEBUG_HWACCEL_DRM
    ALOGD("[drm:%p]: unmap", map);
#endif
    av_frame_free(&map->source);
    for (int i = 0; i < map->nb_objects; i++) {
        munmap(map->address[i], map->length[i]);
    }
}

static int ffmpeg_hwaccel_map_from_drm(AVFrame *frame) {
    AVDRMFrameDescriptor *drm = (AVDRMFrameDescriptor*)frame->data[0];
    AVFrame *output = NULL;
    HWAccelMap *map = NULL;
    void* addr;
    int err = 0;
#if DEBUG_HWACCEL_DRM_FRAMES
    static int counter = 0;
#endif

    map = (HWAccelMap*)av_mallocz(sizeof(HWAccelMap));
    if (!map) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

#if DEBUG_HWACCEL_DRM
    ALOGD("[drm:%p] %d x %d", map, frame->width, frame->height);
#endif

    output = av_frame_alloc();
    map->source = av_frame_alloc();
    if (!output || !map->source) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    // Keep source frame.
    av_frame_ref(map->source, frame);

    // Mmap all DRM objects.
    for (int i = 0; i < drm->nb_objects; i++) {
#if DEBUG_HWACCEL_DRM
        ALOGD("[drm:%p] object[%d] = (%d, %zu, %" PRIx64 ")", map, i,
              drm->objects[i].fd, drm->objects[i].size, drm->objects[i].format_modifier);
#endif
        addr = mmap(NULL, drm->objects[i].size, PROT_READ, MAP_SHARED, drm->objects[i].fd, 0);
        if (addr == MAP_FAILED) {
            err = AVERROR(errno);
            goto fail;
        }
        map->address[i] = addr;
        map->length[i] = drm->objects[i].size;
        map->object[i] = drm->objects[i].fd;
    }
    map->nb_objects = drm->nb_objects;

#if DEBUG_HWACCEL_DRM_FRAMES
    if ((++counter % 100) == 0) {
        char s[128] = {0};
        snprintf(s, sizeof(s), "/data/local/tmp/frame-%06d", counter);
        int outFd = open(s, O_CREAT|O_WRONLY, 0664);
        if (outFd != -1) {
            write(outFd, map->address[0], map->length[0]);
            close(outFd);
        } else {
            ALOGE("unable to write frame to %s: %d", s, errno);
        }
    }
#endif

    // Map DRM layers to AVFrame planes.
    int plane = 0;
    for (int i = 0; i < drm->nb_layers; i++) {
        const AVDRMLayerDescriptor *layer = &drm->layers[i];
#if DEBUG_HWACCEL_DRM
        ALOGD("[drm:%p] layer[%d] = %s", map, i, av_fourcc2str(layer->format));
#endif
        for (int j = 0; j < layer->nb_planes; j++) {
#if DEBUG_HWACCEL_DRM
            ALOGD("[drm:%p] layer[%d].plane[%d] = (%d, %ld, %ld)", map, i, j,
                  layer->planes[j].object_index, layer->planes[j].offset, layer->planes[j].pitch);
#endif
            output->data[plane] = (uint8_t*)map->address[layer->planes[j].object_index] +
                                            layer->planes[j].offset;
            output->linesize[plane] = layer->planes[j].pitch;
#if DEBUG_HWACCEL_DRM
            ALOGD("[drm:%p] frame.plane[%d] = (%p, %d)", map, plane, output->data[plane], output->linesize[plane]);
#endif
            plane++;
        }
    }

    // Create frame refcount'ed buffer
    output->buf[0] = av_buffer_create((uint8_t*)map, sizeof(HWAccelMap),
                                      &ffmpeg_hwaccel_unmap_from_drm, NULL, 0);
    if (!output->buf[0]) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    // Set mapped frame properties
    output->format = AV_PIX_FMT_NV12;
    output->width = frame->width;
    output->height = frame->height;

    // Transfer output to frame
    av_frame_unref(frame);
    av_frame_move_ref(frame, output);
    av_frame_free(&output);

#if DEBUG_HWACCEL_DRM
    ALOGD("[drm:%p] map", map);
#endif

    return 0;

fail:
    if (map) {
        av_frame_free(&map->source);
        for (int i = 0; i < drm->nb_objects; i++) {
            if (map->address[i]) {
                munmap(map->address[i], map->length[i]);
            }
        }
    }
    av_freep(&map);
    av_frame_free(&output);
    return err;
}

int ffmpeg_hwaccel_get_frame(AVCodecContext *avctx __unused, AVFrame *frame,
                             int use_drm_prime) {
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

    // The frame transfer from GPU uses 3 different methods, from fast
    // to slow:
    // 1. Map hw-frame to DRM prime fd
    //    This is the fasted method, which provides direct access to the
    //    raw buffer from the GPU. For Intel GPU however, the buffer is not
    //    linear, and is generally Y-tiled. This then requires additional
    //    support from gralloc to support the format. This method can be
    //    enabled by use_drm_prime argument.
    // 2. Map hw-frame to YV12
    //    This method maps the GPU buffer into a linear buffer in system
    //    memory. Because the Intel GPU uses Y-tiled for decoding, this
    //    requires internal conversion (detiling), which slows down the
    //    process. However the mapped system memory is not copied, which
    //    makes it faster than next method.
    // 3. Transfer hw-frame to YV12
    //    This method does the same as the previous one, but also copy the
    //    mapped memory into FFMPEG-owned buffers. This additional copy makes
    //    it the slowest. This would typically not be used, as method 2
    //    would be successful (and if it fails, then method 3 will also
    //    likely fail).

    // DRM prime mapping (fast)

    if (use_drm_prime) {
        output->format = AV_PIX_FMT_DRM_PRIME;

        err = av_hwframe_map(output, frame, AV_HWFRAME_MAP_READ);
        if (err == 0) {
            err = ffmpeg_hwaccel_map_from_drm(output);
            if (err == 0) {
                goto finish;
            }
        }
        av_frame_unref(output);
    }

    // YUV420P mapping (slower)

    output->format = AV_PIX_FMT_YUV420P;

    err = av_hwframe_map(output, frame, AV_HWFRAME_MAP_READ);
    if (err == 0) {
        goto finish;
    }
    av_frame_unref(output);

    // HW frame download (slowest)

    output->format = AV_PIX_FMT_YUV420P;

    err = av_hwframe_transfer_data(output, frame, 0);
    if (err < 0) {
        ALOGE("ffmpeg_hwaccel_get_frame failed to transfer data: %s (%08x)",
              av_err2str(err), err);
        goto fail;
    }
    goto finish;

finish:
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
