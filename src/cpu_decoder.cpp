#include "cpu_decoder.hpp"
#include <thread>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace
{
    // PixelFormat（本项目的像素格式枚举）→ FFmpeg 的 AVPixelFormat
    bool toAvPixelFormat(uint32_t pixel_format, AVPixelFormat &out)
    {
        switch (pixel_format)
        {
        case static_cast<uint32_t>(PixelFormat::BGR):
            out = AV_PIX_FMT_BGR24;
            return true;
        case static_cast<uint32_t>(PixelFormat::NV12):
            out = AV_PIX_FMT_NV12;
            return true;
        case static_cast<uint32_t>(PixelFormat::I420):
            out = AV_PIX_FMT_YUV420P;
            return true;
        case static_cast<uint32_t>(PixelFormat::YUYV422):
            out = AV_PIX_FMT_YUYV422;
            return true;
        default:
            return false;
        }
    }
}

// grab() 内的耗时拆分：demux（等网络/等下一个包） vs 解码本身。
// 仅用于诊断（RTSP_PROFILE=1 时每秒打印一次）：“解码慢”和“在等源帧”是完全不同的结论。
namespace
{
    struct GrabProfile
    {
        std::mutex m;
        std::atomic<uint64_t> frames{0};
        std::atomic<uint64_t> demux_us{0};
        std::atomic<uint64_t> decode_us{0};
        std::chrono::steady_clock::time_point last_log{std::chrono::steady_clock::now()};
    };
    GrabProfile g_grab_prof;

    void profile_grab(uint64_t demux_us, uint64_t decode_us)
    {
        static const bool enabled = (std::getenv("RTSP_PROFILE") != nullptr);
        if (!enabled)
            return;

        g_grab_prof.frames.fetch_add(1, std::memory_order_relaxed);
        g_grab_prof.demux_us.fetch_add(demux_us, std::memory_order_relaxed);
        g_grab_prof.decode_us.fetch_add(decode_us, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lk(g_grab_prof.m);
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_grab_prof.last_log).count();
        if (elapsed_ms < 1000)
            return;
        uint64_t n = g_grab_prof.frames.exchange(0, std::memory_order_relaxed);
        uint64_t dm = g_grab_prof.demux_us.exchange(0, std::memory_order_relaxed);
        uint64_t dc = g_grab_prof.decode_us.exchange(0, std::memory_order_relaxed);
        g_grab_prof.last_log = now;
        if (n == 0)
            return;
        spdlog::info("[profile/grab] {:5.2f} 次/s | 每帧: demux(等包) {:.1f}ms  解码+取帧 {:.1f}ms",
                     n * 1000.0 / elapsed_ms, dm / (double)n / 1000.0, dc / (double)n / 1000.0);
    }
}

bool CpuDecoder::open(const std::string &url)
{
    last_url_ = url;

    // 每次尝试前先彻底清理资源，防止重连时泄漏
    release();

    // 1. 创建解封装器（auto_reboot=false，由上层 StreamTask 统一控制重连）
    spdlog::info("[CpuDecoder] Opening stream: {}", url);
    demuxer_ = FFHDDemuxer::create_ffmpeg_demuxer(url, false, this->only_key_frames_);
    if (!demuxer_)
    {
        spdlog::warn("[CpuDecoder] Failed to create demuxer for {}", url);
        return false;
    }

    // 获取头部额外数据 (SPS/PPS 等)
    uint8_t *extra_data = nullptr;
    int extra_size = 0;
    demuxer_->get_extra_data(&extra_data, &extra_size);

    // 2. 创建软解码器
    spdlog::info("[CpuDecoder] codec={}, extra_data_size={}, url={}",
                 static_cast<int>(demuxer_->get_video_codec()), extra_size, url);
    decoder_ = FFHDDecoder::create_ffmpeg_decoder(
        static_cast<AVCodecID>(demuxer_->get_video_codec()),
        extra_data,
        extra_size);

    if (!decoder_)
    {
        spdlog::warn("[CpuDecoder] Failed to create decoder for {}", url);
        demuxer_.reset(); // 释放 demuxer
        return false;
    }

    is_opened_ = true;
    open_time_ = std::chrono::steady_clock::now();
    spdlog::info("[CpuDecoder] Successfully opened stream: {}", url);
    return true;
}

bool CpuDecoder::reconnect()
{
    // 重连策略统一由上层 StreamTask 控制，底层只负责重新 open 一次
    spdlog::warn("[CpuDecoder] Reconnecting to: {}", last_url_);
    return open(last_url_);
}

bool CpuDecoder::isOpened() const
{
    return is_opened_ && demuxer_ != nullptr && decoder_ != nullptr;
}

bool CpuDecoder::grab()
{
    if (!isOpened())
    {
        spdlog::warn("[CpuDecoder] grab() called but not opened");
        return false;
    }

    uint8_t *packet_data = nullptr;
    int packet_size = 0;
    bool is_key = false;

    uint64_t demux_us_total = 0;
    uint64_t decode_us_total = 0;

    while (true)
    {
        // 1. 优先尝试从解码器内部缓存区拉取已解码的帧（因为1个Packet可能解出多个Frame，或者B帧导致延迟）
        auto t_decode = std::chrono::steady_clock::now();
        if (decoder_->receive_frame(&current_frame_))
        {
            decode_us_total += std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - t_decode)
                                   .count();
            // 逐帧花屏标记：这一帧的解码错误标志（供客户端判断“我拿到的这帧能不能用”）
            const int err_flags = current_frame_ ? current_frame_->decode_error_flags : 0;
            constexpr int CORRUPT_MASK = FF_DECODE_ERROR_MISSING_REFERENCE |
                                         FF_DECODE_ERROR_CONCEALMENT_ACTIVE |
                                         FF_DECODE_ERROR_INVALID_BITSTREAM;
            last_frame_corrupted_.store((err_flags & CORRUPT_MASK) != 0, std::memory_order_relaxed);

            frame_ready_.store(true, std::memory_order_release);
            profile_grab(demux_us_total, decode_us_total);
            return true; // 成功拿到一帧，退出 grab
        }
        decode_us_total += std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - t_decode)
                               .count();

        // 2. 解码器内没有帧了，去 Demuxer 读新的数据包（通常这里是“等下一个源帧”的网络等待）
        auto t_demux = std::chrono::steady_clock::now();
        bool demux_ok = demuxer_->demux(&packet_data, &packet_size, &last_pts_, &is_key);
        demux_us_total += std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t_demux)
                              .count();
        if (!demux_ok)
        {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - open_time_)
                                  .count();
            spdlog::warn("[CpuDecoder] demux failed after {} ms for {}, packet_size={}, key={}, opened={}",
                         elapsed_ms, last_url_, packet_size, is_key, isOpened());
            return false;
        }

        spdlog::trace("[CpuDecoder] demux ok: packet_size={}, pts={}, key={}", packet_size, last_pts_, is_key);

        // 3. 将新读取的 Packet 送入解码器
        // 送入失败时跳过当前包，继续读取下一个包
        auto t_send = std::chrono::steady_clock::now();
        if (!decoder_->send_packet(packet_data, packet_size, last_pts_))
        {
            spdlog::warn("Invalid or non-decodable packet skipped for stream {}", last_url_);
            continue;
        }
        decode_us_total += std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - t_send)
                               .count();
    }

    return true;
}

AVFrame *CpuDecoder::takePendingFrame()
{
    frames_to_skip_ = 0;
    if (!isOpened() || !frame_ready_.load(std::memory_order_acquire))
        return nullptr;

    // 消费掉这一帧，标记为未准备，等待下一次 grab
    frame_ready_.store(false, std::memory_order_release);
    // 处理跳帧逻辑（预热防花屏）
    if (frames_to_skip_ > 0)
    {
        frames_to_skip_--;
        return nullptr;
    }

    return current_frame_;
}

bool CpuDecoder::retrieve(cv::Mat &frame, bool need_data)
{
    AVFrame *av_frame = takePendingFrame();
    if (!av_frame)
        return false;

    if (!need_data)
        return true;

    int width = decoder_->get_width();
    int height = decoder_->get_height();

    // 防御编程：视频格式尚未解析时跳过
    if (width <= 0 || height <= 0)
    {
        spdlog::warn("Video format not yet parsed, width={}, height={}", width, height);
        return false;
    }

    // 创建对应大小的 cv::Mat
    frame.create(height, width, CV_8UC3);

    // 利用 FFmpegDecoder 内部的 sws_scale 将 YUV 转为 BGR 写进 cv::Mat 的内存中
    if (!decoder_->convert_to_bgr(av_frame, frame.data, width * 3))
    {
        spdlog::error("Failed to convert frame to BGR");
        return false;
    }
    return true;
}

// 以指定像素格式取出原始帧（共享内存零拷贝通道使用）。
// 与 retrieve() 共用同一套取帧/跳帧逻辑，区别只在于 sws_scale 的目标格式：
// 不经过 BGR 中转，因此没有色度损失，也不多一次转换。
bool CpuDecoder::retrieveRaw(RawFrame &out, uint32_t pixel_format)
{
    out.release();

    AVPixelFormat dst_fmt = AV_PIX_FMT_NONE;
    if (!toAvPixelFormat(pixel_format, dst_fmt))
    {
        spdlog::error("[CpuDecoder] Unsupported pixel format {} for stream", pixel_format);
        return false;
    }

    AVFrame *av_frame = takePendingFrame();
    if (!av_frame)
        return false;

    const int width = decoder_->get_width();
    const int height = decoder_->get_height();
    if (width <= 0 || height <= 0)
    {
        spdlog::warn("[CpuDecoder] Video format not yet parsed, width={}, height={}", width, height);
        return false;
    }

    // 按目标格式准备 CV Mat 缓冲（紧密排列，无行末 padding）。
    // 注意：YUV420 的缓冲行数是 H*3/2，但 out.width/height 仍记录图像宽高。
    const int chroma_h = (height + 1) / 2;
    int rows = height;
    int cols = width;
    int type = CV_8UC3;
    int linesize = width * 3;
    switch (pixel_format)
    {
    case static_cast<uint32_t>(PixelFormat::NV12):
    case static_cast<uint32_t>(PixelFormat::I420):
        rows = height + chroma_h;
        cols = width;
        type = CV_8UC1;
        linesize = width;
        break;
    case static_cast<uint32_t>(PixelFormat::YUYV422):
        rows = height;
        cols = width;
        type = CV_8UC2;
        linesize = width * 2;
        break;
    default: // BGR24
        break;
    }
    out.buffer.create(rows, cols, type);

    // 紧凑布局自检：宽/高为奇数时 sws 的平面步长可能与我们分配的 Mat 不一致，
    // 此时退化为“按 sws 要求的一维缓冲”，保证数据正确优先于形状好看。
    const int expect = av_image_get_buffer_size(dst_fmt, width, height, 1);
    const size_t got = out.buffer.total() * out.buffer.elemSize();
    if (expect < 0 || static_cast<size_t>(expect) != got)
    {
        spdlog::warn("[CpuDecoder] {} buffer size mismatch (expect {} got {}) for {}x{}, use flat buffer",
                     pixelFormatName(pixel_format), expect, got, width, height);
        out.buffer.create(expect > 0 ? expect : 1, 1, CV_8UC1);
    }

    if (!decoder_->convert_to_format(av_frame, out.buffer.data, linesize, dst_fmt))
    {
        spdlog::error("[CpuDecoder] Failed to convert frame to {}", pixelFormatName(pixel_format));
        out.release();
        return false;
    }

    out.width = width;
    out.height = height;
    out.step = static_cast<uint32_t>(linesize);
    out.pixel_format = pixel_format;
    return true;
}

int CpuDecoder::getWidth() const
{
    return decoder_ ? decoder_->get_width() : 0;
}

int CpuDecoder::getHeight() const
{
    return decoder_ ? decoder_->get_height() : 0;
}

void CpuDecoder::release()
{
    is_opened_ = false;
    frame_ready_.store(false, std::memory_order_release);
    current_frame_ = nullptr;
    if (decoder_)
    {
        decoder_->send_packet(nullptr, 0);
        decoder_.reset();
    }

    if (demuxer_)
    {
        demuxer_.reset();
    }
}