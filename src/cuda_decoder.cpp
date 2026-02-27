#include "cuda_decoder.h"

#include "cuda_decoder.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

bool CudaDecoder::open(const std::string &url)
{
    last_url_ = url;      // 保存 URL 供后续重连使用
    frames_to_skip_ = 15; // 重置丢弃帧计数

    const int MAX_ATTEMPTS = 3;

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt)
    {
        // 每次尝试前先清理资源
        release();

        // 1. 创建解封装器
        demuxer_ = FFHDDemuxer::create_ffmpeg_demuxer(url, true);
        if (!demuxer_)
        {
            std::cerr << "[WARN] Attempt " << attempt << ": Failed to create demuxer for " << url << std::endl;
        }
        else
        {
            // 2. 创建硬解码器
            decoder_ = FFHDDecoder::create_cuvid_decoder(
                false,
                FFHDDecoder::ffmpeg2NvCodecId(demuxer_->get_video_codec()),
                -1,
                0,
                nullptr,
                nullptr,
                true);

            if (!decoder_)
            {
                spdlog::info("[WARN] Attempt {}: Failed to create decoder for {}", attempt, url);
                demuxer_.reset(); // 释放 demuxer
            }
            else
            {
                // 3. 成功创建！推送额外数据并返回
                uint8_t *extra_data = nullptr;
                int extra_size = 0;
                demuxer_->get_extra_data(&extra_data, &extra_size);
                if (extra_size > 0)
                {
                    decoder_->decode(extra_data, extra_size);
                }

                is_opened_ = true;
                spdlog::info("[INFO] Successfully opened stream: {}", url);
                return true;
            }
        }

        // 如果未成功，且还没到最后一次尝试，则等待后再试
        if (attempt < MAX_ATTEMPTS)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    spdlog::error("[ERROR] Failed to open stream after {} attempts.", MAX_ATTEMPTS);
    return false;
}

bool CudaDecoder::reconnect()
{
    spdlog::info("[WARN] Stream disconnected, attempting to reconnect to: {}", last_url_);
    release();
    // 简单的重连逻辑：尝试重新 open
    for (int i = 0; i < 3; ++i)
    { // 尝试3次
        if (open(last_url_))
        {
            spdlog::info("[INFO] Reconnection successful.");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return false;
}

bool CudaDecoder::isOpened() const
{
    return is_opened_ && demuxer_ != nullptr && decoder_ != nullptr;
}

bool CudaDecoder::grab()
{
    if (!isOpened())
    {
        if (!reconnect())
            return false;
    }

    uint8_t *packet_data = nullptr;
    int packet_size = 0;
    bool is_key = false;

    while (true)
    {
        if (!demuxer_->demux(&packet_data, &packet_size, &last_pts_, &is_key))
        {
            // 如果 demux 失败，触发重连逻辑
            if (reconnect())
                continue;
            if (decoded_frames_available_ <= 0)
                return false;
            break;
        }

        decoded_frames_available_ = decoder_->decode(packet_data, packet_size, last_pts_);

        if (decoded_frames_available_ > 0)
            break;
        if (packet_size == 0)
            return false;
    }
    return true;
}

bool CudaDecoder::retrieve(cv::Mat &frame, bool need_data)
{
    if (!isOpened() || decoded_frames_available_ <= 0)
        return false;

    void *ptr = decoder_->get_frame(&last_pts_, &last_frame_index_);
    if (!ptr)
        return false;

    if (need_data && frames_to_skip_ > 0)
    {
        frames_to_skip_--;
        decoded_frames_available_--;
        return false; // 返回 false，让外部主循环继续 grab 下一帧
    }

    // 逻辑：如果是需要丢弃的帧，则跳过并直接递减计数
    if (frames_to_skip_ > 0)
    {
        frames_to_skip_--;
        decoded_frames_available_--;
        return false; // 返回 false，让外部主循环继续 grab 下一帧
    }

    cv::Mat decoded_mat(decoder_->get_height(), decoder_->get_width(), CV_8UC3, ptr);
    frame = decoded_mat.clone();

    decoded_frames_available_--;
    return true;
}

void CudaDecoder::release()
{
    is_opened_ = false;
    decoder_.reset();
    demuxer_.reset();
    decoded_frames_available_ = 0;
}