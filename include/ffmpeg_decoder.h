#pragma once
#include "interfaces.h"
#include <opencv2/opencv.hpp>
#include <memory>
#include <string>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * FFmpeg 原生软解码器
 * 使用 FFmpeg libavcodec 进行 CPU 软解码
 */
class FFmpegDecoder : public IVideoDecoder
{
public:
    FFmpegDecoder() = default;
    virtual ~FFmpegDecoder();

    bool open(const std::string &url) override;
    bool isOpened() const override;
    bool grab() override;
    bool retrieve(cv::Mat &frame, bool need_data = true) override;
    void release() override;

    // 宽高查询
    int getWidth() const override;
    int getHeight() const override;

private:
    void cleanup();
    bool decodeFrame();

private:
    AVFormatContext *fmt_ctx_ = nullptr;
    AVCodecContext *codec_ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVFrame *frame_bgr_ = nullptr;
    AVPacket *packet_ = nullptr;
    SwsContext *sws_ctx_ = nullptr;

    int video_stream_idx_ = -1;
    int width_ = 0;
    int height_ = 0;
    bool is_opened_ = false;
    bool frame_ready_ = false;  // grab 后帧是否就绪

    std::string last_url_;      // 保存 URL 用于重连
    int frames_to_skip_ = 15;   // 待丢弃帧计数（跳过初始不稳定帧）
};
