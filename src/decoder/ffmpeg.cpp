/*!
 *  Copyright (c) 2019 by Contributors
 * \file ffmpeg.cpp
 * \brief FFMPEG implementations
 */

#include "ffmpeg.h"

#include <dmlc/logging.h>

namespace decord {
namespace ffmpeg {

FrameTransform::FrameTransform(DLDataType dtype, uint32_t h, uint32_t w, uint32_t c, int interp) 
        : height(h), width(w), channel(c), interp(interp) {
    CHECK(c == 3 || c == 1)
        << "Only 3 channel RGB or 1 channel Gray image format supported";
    if (dtype == kUInt8) {
        fmt = c == 3 ? AV_PIX_FMT_RGB24: AV_PIX_FMT_GRAY8;
    } else if (dtype == kUInt16) {
        fmt = c == 3 ? AV_PIX_FMT_RGB48: AV_PIX_FMT_GRAY16;
    } else if (dtype == kFloat16) {
        // FFMPEG has no native support of float formats
        // use fp16 and cast later
        fmt = c == 3 ? AV_PIX_FMT_RGB48: AV_PIX_FMT_GRAY16;
    } else {
        LOG(FATAL) << "Unsupported data type [" 
            << dtype.code << " " << dtype.bits << " " << dtype.lanes
            << " and channel combination: " << channel;
    }
};

// Constructor
FFMPEGVideoReader::FFMPEGVideoReader(std::string& fn)
     : fmt_ctx_(NULL), dec_ctx_(NULL), actv_stm_idx_(-1) {
    // allocate format context
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        LOG(FATAL) << "ERROR allocating memory for Format Context";
    }
    // open file
    if(avformat_open_input(&fmt_ctx_, fn.c_str(), NULL, NULL) != 0 ) {
        LOG(FATAL) << "ERROR opening file: " << fn;
    }

    // find stream info
    if (avformat_find_stream_info(fmt_ctx_,  NULL) < 0) {
        LOG(FATAL) << "ERROR getting stream info of file" << fn;
    }

    // initialize all video streams and store codecs info
    for (uint32_t i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream *st = fmt_ctx_->streams[i];
        AVCodec *local_codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // store video stream codecs only
            codecs_.emplace_back(local_codec);
        } else {
            // audio, subtitle... skip
            codecs_.emplace_back(NULL);
        }
    }
    // find best video stream (-1 means auto, relay on FFMPEG)
    SetVideoStream(-1);

    // allocate AVFrame buffer
    frame_ = av_frame_alloc();
    CHECK(frame_) << "ERROR failed to allocated memory for AVFrame";

    // allocate AVPacket buffer
    pkt_ = av_packet_alloc();
    CHECK(pkt_) << "ERROR failed to allocated memory for AVPacket";
}

void FFMPEGVideoReader::SetVideoStream(int stream_nb) {
    CHECK(fmt_ctx_ != NULL);
    int st_nb = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    CHECK_GE(st_nb, 0) << "ERROR cannot find video stream with wanted index: " << stream_nb;
    // initialize the mem for codec context
    dec_ctx_ = avcodec_alloc_context3(codecs_[st_nb]);
    CHECK(dec_ctx_) << "ERROR allocating memory for AVCodecContext";
    // copy codec parameters to context
    CHECK_GE(avcodec_parameters_to_context(dec_ctx_, fmt_ctx_->streams[st_nb]->codecpar), 0)
        << "ERROR copying codec parameters to context";
    // initialize AVCodecContext to use given AVCodec
    CHECK_GE(avcodec_open2(dec_ctx_, codecs_[st_nb], NULL), 0)
        << "ERROR open codec through avcodec_open2";
    actv_stm_idx_ = st_nb;
}

unsigned int FFMPEGVideoReader::QueryStreams() {
    CHECK(fmt_ctx_ != NULL);
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        // iterate and print stream info
        // feel free to add more if needed
        AVStream *st = fmt_ctx_->streams[i];
        AVCodec *codec = codecs_[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            LOG(INFO) << "Video Stream [" << i << "]:"
                << " Average FPS: " 
                << static_cast<float>(st->avg_frame_rate.num) / st->avg_frame_rate.den
                << " Start time: "
                << st->start_time
                << " Duration: "
                << st->duration
                << " Codec Type: "
                << codec->name
                << " ID: "
                << codec->id
                << " bit_rate: "
                << st->codecpar->bit_rate
                << " Resolution: "
                << st->codecpar->width << "x" << st->codecpar->height;
        } else {
            const char *codec_type = av_get_media_type_string(st->codecpar->codec_type);
            codec_type = codec_type? codec_type : "unknown type";
            LOG(INFO) << codec_type << " Stream [" << i << "]:";
        }
        
    }
    return fmt_ctx_->nb_streams;
}

bool FFMPEGVideoReader::NextFrame(NDArray* arr, DLDataType dtype) {
    if (arr == NULL) {
        // Create NDArray with frame shape and default dtype
        auto ndarray = NDArray::Empty({dec_ctx_->height, dec_ctx_->width, dec_ctx_->channels}, kUInt8, kCPU);
        arr = &ndarray;
    }
    // read next packet which belongs to the desired stream
    while (av_read_frame(fmt_ctx_, pkt_) >= 0) {
        if (pkt_->stream_index == actv_stm_idx_) {
            int got_picture;
            // decode frame from packet
            avcodec_decode_video2(dec_ctx_, frame_, &got_picture, pkt_);
            if (got_picture) {
                // convert raw image(e.g. YUV420, YUV422) to RGB image
                out_fmt = 
                struct SwsContext *sws_ctx = GetSwsContext(out_fmt);
                return true;
            }
        }
    }
    return false;
}

struct SwsContext* FFMPEGVideoReader::GetSwsContext(FrameTransform out_fmt) {
    auto sws_ctx = sws_ctx_map_.find(out_fmt);
    if ( sws_ctx == sws_ctx_map_.end()) {
        // already initialized sws context
        return sws_ctx->second;
    } else {
        // create new sws context
        struct SwsContext *ctx = sws_getContext(dec_ctx_->width,
                                                dec_ctx_->height,
                                                dec_ctx_->pix_fmt,
                                                out_fmt.width,
                                                out_fmt.height,
                                                out_fmt.fmt,
                                                out_fmt.interp,
                                                NULL,
                                                NULL,
                                                NULL
                                                );
        sws_ctx_map_[out_fmt] = ctx;
        return ctx;
    }
}

bool ToNDArray(AVFrame *frame, NDArray *arr, DLDataType dtype) {
    struct SwsContext *sws_ctx = NULL;
    
}
}  // namespace ffmpeg
}  // namespace decord