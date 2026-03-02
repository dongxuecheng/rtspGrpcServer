#include "ffmpeg_decoder.h"
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

FFmpegDecoder::~FFmpegDecoder()
{
    release();
}

bool FFmpegDecoder::open(const std::string &url)
{
    last_url_ = url;
    frames_to_skip_ = 15;

    cleanup();

    // 1. 打开输入流
    AVDictionary *options = nullptr;
    if (url.find("rtsp://") == 0)
    {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "buffer_size", "1024000", 0);
        av_dict_set(&options, "stimeout", "2000000", 0);
        av_dict_set(&options, "max_delay", "1000000", 0);
    }

    int ret = avformat_open_input(&fmt_ctx_, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0)
    {
        spdlog::error("FFmpegDecoder: Failed to open input: {}", url);
        return false;
    }

    // 2. 获取流信息
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0)
    {
        spdlog::error("FFmpegDecoder: Failed to find stream info");
        cleanup();
        return false;
    }

    // 3. 找到视频流
    video_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; i++)
    {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            video_stream_idx_ = i;
            break;
        }
    }

    if (video_stream_idx_ < 0)
    {
        spdlog::error("FFmpegDecoder: No video stream found");
        cleanup();
        return false;
    }

    // 4. 创建解码器上下文
    AVCodecParameters *codecpar = fmt_ctx_->streams[video_stream_idx_]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        spdlog::error("FFmpegDecoder: Unsupported codec");
        cleanup();
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_)
    {
        spdlog::error("FFmpegDecoder: Failed to allocate codec context");
        cleanup();
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx_, codecpar) < 0)
    {
        spdlog::error("FFmpegDecoder: Failed to copy codec parameters");
        cleanup();
        return false;
    }

    // 5. 打开解码器
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0)
    {
        spdlog::error("FFmpegDecoder: Failed to open codec");
        cleanup();
        return false;
    }

    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    // 6. 分配帧
    frame_ = av_frame_alloc();
    frame_bgr_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!frame_ || !frame_bgr_ || !packet_)
    {
        spdlog::error("FFmpegDecoder: Failed to allocate frame/packet");
        cleanup();
        return false;
    }

    // 7. 分配 BGR 帧缓冲区
    int bgr_size = av_image_get_buffer_size(AV_PIX_FMT_BGR24, width_, height_, 1);
    uint8_t *bgr_buffer = (uint8_t *)av_malloc(bgr_size);
    if (!bgr_buffer)
    {
        spdlog::error("FFmpegDecoder: Failed to allocate BGR buffer");
        cleanup();
        return false;
    }
    av_image_fill_arrays(frame_bgr_->data, frame_bgr_->linesize, bgr_buffer,
                         AV_PIX_FMT_BGR24, width_, height_, 1);

    // 8. 创建颜色转换上下文
    sws_ctx_ = sws_getContext(
        width_, height_, codec_ctx_->pix_fmt,
        width_, height_, AV_PIX_FMT_BGR24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx_)
    {
        spdlog::error("FFmpegDecoder: Failed to create SwsContext");
        cleanup();
        return false;
    }

    is_opened_ = true;
    spdlog::info("FFmpegDecoder: Successfully opened stream: {} ({}x{})", url, width_, height_);
    return true;
}

bool FFmpegDecoder::isOpened() const
{
    return is_opened_ && fmt_ctx_ != nullptr && codec_ctx_ != nullptr;
}

bool FFmpegDecoder::grab()
{
    frame_ready_ = false;
    if (!isOpened())
        return false;

    // 循环读取并解码，直到获得一帧或出错
    while (true)
    {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0)
        {
            // 流结束或读取失败
            if (ret == AVERROR_EOF)
            {
                spdlog::warn("FFmpegDecoder: End of stream");
            }
            else
            {
                spdlog::warn("FFmpegDecoder: Read frame failed, attempting reconnect...");
                // 尝试重连
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (open(last_url_))
                {
                    continue;
                }
            }
            return false;
        }

        // 只处理视频流
        if (packet_->stream_index != video_stream_idx_)
        {
            av_packet_unref(packet_);
            continue;
        }

        // 发送到解码器
        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);

        if (ret < 0)
        {
            spdlog::warn("FFmpegDecoder: Send packet failed");
            continue;
        }

        // 接收解码帧
        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN))
        {
            // 需要更多数据
            continue;
        }
        else if (ret < 0)
        {
            spdlog::warn("FFmpegDecoder: Receive frame failed");
            return false;
        }

        // 跳过初始不稳定帧
        if (frames_to_skip_ > 0)
        {
            frames_to_skip_--;
            av_frame_unref(frame_);
            continue;
        }

        // 成功获取帧
        frame_ready_ = true;
        return true;
    }
}

bool FFmpegDecoder::retrieve(cv::Mat &frame, bool need_data)
{
    if (!frame_ready_ || !frame_)
    {
        return false;
    }

    // 防御编程：宽高为0时跳过
    if (width_ == 0 || height_ == 0)
    {
        spdlog::warn("FFmpegDecoder: Video format not ready, width={}, height={}", width_, height_);
        av_frame_unref(frame_);
        frame_ready_ = false;
        return false;
    }

    if (need_data)
    {
        // 颜色空间转换: YUV -> BGR
        sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, height_,
                  frame_bgr_->data, frame_bgr_->linesize);

        // 复制到 cv::Mat
        frame.create(height_, width_, CV_8UC3);
        for (int y = 0; y < height_; y++)
        {
            memcpy(frame.ptr(y), frame_bgr_->data[0] + y * frame_bgr_->linesize[0], width_ * 3);
        }
    }

    av_frame_unref(frame_);
    frame_ready_ = false;
    return true;
}

int FFmpegDecoder::getWidth() const
{
    return width_;
}

int FFmpegDecoder::getHeight() const
{
    return height_;
}

void FFmpegDecoder::release()
{
    cleanup();
}

void FFmpegDecoder::cleanup()
{
    is_opened_ = false;
    frame_ready_ = false;

    if (sws_ctx_)
    {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (frame_bgr_)
    {
        if (frame_bgr_->data[0])
        {
            av_free(frame_bgr_->data[0]);
        }
        av_frame_free(&frame_bgr_);
        frame_bgr_ = nullptr;
    }

    if (frame_)
    {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (packet_)
    {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    if (codec_ctx_)
    {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    if (fmt_ctx_)
    {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    width_ = 0;
    height_ = 0;
    video_stream_idx_ = -1;
}
