#include "video_decoder.h"

#include <fcntl.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    GoVideoDecoder base;
    const AVCodec* codec;
    AVCodecContext* context;
    AVPacket* packet;
    AVFrame* frame;
    int max_width;
    int max_height;
    int last_width;
    int last_height;
    int last_format;
    int last_strides[3];
    char error[256];
} GoV4l2M2MVideoDecoder;

static void copy_error(char* destination, size_t capacity, const char* message) {
    if (destination && capacity > 0)
        snprintf(destination, capacity, "%s", message);
}

static void set_ffmpeg_error(GoV4l2M2MVideoDecoder* decoder, const char* operation, int result) {
    char detail[128];
    if (av_strerror(result, detail, sizeof(detail)) < 0)
        snprintf(detail, sizeof(detail), "error %d", result);
    snprintf(decoder->error, sizeof(decoder->error), "%s: %s", operation, detail);
}

static int visible_height(const GoV4l2M2MVideoDecoder* decoder, int width, int height) {
    int64_t scaled_height = (int64_t)width * decoder->max_height;
    if (scaled_height % decoder->max_width != 0)
        return height;

    int expected_height = (int)(scaled_height / decoder->max_width);
    if (height > expected_height && height - expected_height <= 32)
        return expected_height;
    return height;
}

static int video_device_supports_h264_m2m(int descriptor) {
    struct v4l2_capability capability;
    memset(&capability, 0, sizeof(capability));
    if (ioctl(descriptor, VIDIOC_QUERYCAP, &capability) < 0)
        return 0;

    uint32_t capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                                ? capability.device_caps
                                : capability.capabilities;
    if ((capabilities & V4L2_CAP_STREAMING) == 0)
        return 0;

    struct v4l2_fmtdesc format;
    memset(&format, 0, sizeof(format));
    if ((capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) != 0)
        format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    else if ((capabilities & V4L2_CAP_VIDEO_M2M) != 0)
        format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    else
        return 0;

    while (ioctl(descriptor, VIDIOC_ENUM_FMT, &format) == 0) {
        if (format.pixelformat == V4L2_PIX_FMT_H264)
            return 1;
        ++format.index;
    }
    return 0;
}

static int has_usable_h264_m2m_decoder(void) {
    char path[32];
    for (int index = 0; index < 64; ++index) {
        snprintf(path, sizeof(path), "/dev/video%d", index);
        int descriptor = open(path, O_RDWR | O_CLOEXEC);
        if (descriptor < 0)
            continue;
        int supported = video_device_supports_h264_m2m(descriptor);
        close(descriptor);
        if (supported)
            return 1;
    }
    return 0;
}

static GoVideoDecoderResult v4l2_m2m_submit(GoVideoDecoder* base, const uint8_t* data,
                                            size_t length) {
    GoV4l2M2MVideoDecoder* decoder = (GoV4l2M2MVideoDecoder*)base;
    if (length > INT_MAX) {
        snprintf(decoder->error, sizeof(decoder->error), "H.264 access unit is too large");
        return GO_VIDEO_DECODER_RESULT_FATAL;
    }

    decoder->packet->data = (uint8_t*)data;
    decoder->packet->size = (int)length;
    int result = avcodec_send_packet(decoder->context, decoder->packet);
    decoder->packet->data = NULL;
    decoder->packet->size = 0;
    if (result == 0)
        return GO_VIDEO_DECODER_RESULT_OK;
    if (result == AVERROR(EAGAIN))
        return GO_VIDEO_DECODER_RESULT_AGAIN;
    set_ffmpeg_error(decoder, "V4L2 M2M packet submission failed", result);
    return GO_VIDEO_DECODER_RESULT_FATAL;
}

static GoVideoDecoderResult v4l2_m2m_receive(GoVideoDecoder* base, GoDecodedVideoFrame* output) {
    GoV4l2M2MVideoDecoder* decoder = (GoV4l2M2MVideoDecoder*)base;
    int result = avcodec_receive_frame(decoder->context, decoder->frame);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        return GO_VIDEO_DECODER_RESULT_AGAIN;
    if (result < 0) {
        set_ffmpeg_error(decoder, "V4L2 M2M frame receive failed", result);
        return GO_VIDEO_DECODER_RESULT_FATAL;
    }
    result = av_frame_apply_cropping(decoder->frame, 0);
    if (result < 0) {
        set_ffmpeg_error(decoder, "V4L2 M2M frame cropping failed", result);
        av_frame_unref(decoder->frame);
        return GO_VIDEO_DECODER_RESULT_FATAL;
    }

    if (decoder->frame->format == AV_PIX_FMT_NV12)
        output->format = GO_VIDEO_PIXEL_FORMAT_NV12;
    else if (decoder->frame->format == AV_PIX_FMT_YUV420P)
        output->format = GO_VIDEO_PIXEL_FORMAT_YUV420P;
    else {
        snprintf(decoder->error, sizeof(decoder->error),
                 "V4L2 M2M decoder returned unsupported pixel format %d", decoder->frame->format);
        av_frame_unref(decoder->frame);
        return GO_VIDEO_DECODER_RESULT_FATAL;
    }
    int width = decoder->frame->width;
    int height = visible_height(decoder, width, decoder->frame->height);
    if (width <= 0 || height <= 0 || width > decoder->max_width || height > decoder->max_height) {
        snprintf(decoder->error, sizeof(decoder->error),
                 "V4L2 M2M decoder returned invalid dimensions %dx%d", width, height);
        av_frame_unref(decoder->frame);
        return GO_VIDEO_DECODER_RESULT_FATAL;
    }

    output->color_range = decoder->frame->color_range == AVCOL_RANGE_JPEG
                              ? GO_VIDEO_COLOR_RANGE_FULL
                              : GO_VIDEO_COLOR_RANGE_LIMITED;
    output->width = width;
    output->height = height;
    output->coded_width = decoder->context->coded_width;
    output->coded_height = decoder->context->coded_height;
    for (int plane = 0; plane < 3; ++plane) {
        output->planes[plane] = decoder->frame->data[plane];
        output->strides[plane] = decoder->frame->linesize[plane];
    }
    output->info_changed =
        decoder->last_width != output->width || decoder->last_height != output->height ||
        decoder->last_format != decoder->frame->format ||
        memcmp(decoder->last_strides, output->strides, sizeof(decoder->last_strides)) != 0;
    decoder->last_width = output->width;
    decoder->last_height = output->height;
    decoder->last_format = decoder->frame->format;
    memcpy(decoder->last_strides, output->strides, sizeof(decoder->last_strides));
    output->corrupt = (decoder->frame->flags & AV_FRAME_FLAG_CORRUPT) != 0;
    output->backend_frame = decoder->frame;
    return GO_VIDEO_DECODER_RESULT_OK;
}

static void v4l2_m2m_release(GoVideoDecoder* base, GoDecodedVideoFrame* frame) {
    GoV4l2M2MVideoDecoder* decoder = (GoV4l2M2MVideoDecoder*)base;
    if (frame->backend_frame == decoder->frame)
        av_frame_unref(decoder->frame);
}

static int v4l2_m2m_reset(GoVideoDecoder* base) {
    GoV4l2M2MVideoDecoder* decoder = (GoV4l2M2MVideoDecoder*)base;
    av_frame_unref(decoder->frame);
    avcodec_flush_buffers(decoder->context);
    decoder->last_width = 0;
    decoder->last_height = 0;
    decoder->last_format = 0;
    memset(decoder->last_strides, 0, sizeof(decoder->last_strides));
    return 0;
}

static const char* v4l2_m2m_last_error(const GoVideoDecoder* base) {
    const GoV4l2M2MVideoDecoder* decoder = (const GoV4l2M2MVideoDecoder*)base;
    return decoder->error[0] ? decoder->error : "V4L2 M2M H.264 decoder failure";
}

static void v4l2_m2m_destroy(GoVideoDecoder* base) {
    GoV4l2M2MVideoDecoder* decoder = (GoV4l2M2MVideoDecoder*)base;
    av_frame_free(&decoder->frame);
    av_packet_free(&decoder->packet);
    avcodec_free_context(&decoder->context);
    free(decoder);
}

static const GoVideoDecoderOps v4l2_m2m_ops = {
    .name = "v4l2-m2m",
    .backend = GO_VIDEO_DECODER_BACKEND_V4L2_M2M,
    .submit_access_unit = v4l2_m2m_submit,
    .receive_frame = v4l2_m2m_receive,
    .release_frame = v4l2_m2m_release,
    .reset = v4l2_m2m_reset,
    .last_error = v4l2_m2m_last_error,
    .destroy = v4l2_m2m_destroy,
};

GoVideoDecoder* go_video_decoder_v4l2_m2m_create(int max_width, int max_height, char* error,
                                                  size_t error_capacity) {
    if (max_width <= 0 || max_height <= 0) {
        copy_error(error, error_capacity, "invalid V4L2 M2M decoder dimensions");
        return NULL;
    }
    if (!has_usable_h264_m2m_decoder()) {
        copy_error(error, error_capacity, "no writable H.264 V4L2 M2M decoder");
        return NULL;
    }

    GoV4l2M2MVideoDecoder* decoder = calloc(1, sizeof(*decoder));
    if (!decoder) {
        copy_error(error, error_capacity, "V4L2 M2M decoder allocation failed");
        return NULL;
    }
    go_video_decoder_initialize(&decoder->base, &v4l2_m2m_ops);
    decoder->max_width = max_width;
    decoder->max_height = max_height;
    decoder->codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (decoder->codec)
        decoder->context = avcodec_alloc_context3(decoder->codec);
    if (decoder->context) {
        decoder->context->width = max_width;
        decoder->context->height = max_height;
        decoder->context->coded_width = max_width;
        decoder->context->coded_height = max_height;
        decoder->context->thread_count = 1;
        decoder->context->thread_type = 0;
        decoder->context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        int result = avcodec_open2(decoder->context, decoder->codec, NULL);
        if (result < 0) {
            set_ffmpeg_error(decoder, "V4L2 M2M H.264 initialization failed", result);
        } else {
            decoder->packet = av_packet_alloc();
            decoder->frame = av_frame_alloc();
        }
    }
    if (!decoder->codec || !decoder->context || !decoder->packet || !decoder->frame) {
        if (!decoder->error[0])
            snprintf(decoder->error, sizeof(decoder->error), "V4L2 M2M H.264 decoder unavailable");
        copy_error(error, error_capacity, decoder->error);
        v4l2_m2m_destroy(&decoder->base);
        return NULL;
    }
    return &decoder->base;
}
