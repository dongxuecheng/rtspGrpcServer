#include "turbojpeg_encoder.hpp"
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <cstdlib>

// 剖析：区分 tjCompress2 本身的耗时与包装层的耗时（RTSP_PROFILE=1 时每秒打印）
namespace
{
    std::atomic<uint64_t> g_tj_us{0};
    std::atomic<uint64_t> g_copy_us{0};
    std::atomic<uint64_t> g_calls{0};
    std::atomic<uint64_t> g_bytes{0};
    std::chrono::steady_clock::time_point g_last_log = std::chrono::steady_clock::now();

    void tjProfile(uint64_t tj_us, uint64_t copy_us, uint64_t bytes)
    {
        static const bool enabled = (std::getenv("RTSP_PROFILE") != nullptr);
        if (!enabled)
            return;
        g_tj_us.fetch_add(tj_us, std::memory_order_relaxed);
        g_copy_us.fetch_add(copy_us, std::memory_order_relaxed);
        g_calls.fetch_add(1, std::memory_order_relaxed);
        g_bytes.fetch_add(bytes, std::memory_order_relaxed);

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_log).count();
        if (elapsed_ms < 1000)
            return;
        uint64_t n = g_calls.exchange(0, std::memory_order_relaxed);
        uint64_t tj = g_tj_us.exchange(0, std::memory_order_relaxed);
        uint64_t cp = g_copy_us.exchange(0, std::memory_order_relaxed);
        uint64_t b = g_bytes.exchange(0, std::memory_order_relaxed);
        g_last_log = now;
        if (n == 0)
            return;
        spdlog::info("[profile/tj] {} 次/s | tjCompress2 {:.2f}ms  assign {:.2f}ms | 平均 JPEG {:.0f}KB",
                     n * 1000.0 / elapsed_ms, tj / (double)n / 1000.0, cp / (double)n / 1000.0,
                     b / (double)n / 1024.0);
    }
}

TurboJpegEncoder::TurboJpegEncoder(int quality)
    : quality_(quality), handle_(nullptr)
{
    handle_ = tjInitCompress();
    if (!handle_)
    {
        spdlog::error("TurboJPEG encoder init failed: {}", tjGetErrorStr2(nullptr));
    }
}

TurboJpegEncoder::~TurboJpegEncoder()
{
    if (handle_)
    {
        tjDestroy(handle_);
        handle_ = nullptr;
    }
}

bool TurboJpegEncoder::encode(const cv::Mat& frame, std::string& out_buffer)
{
    if (!handle_)
    {
        spdlog::error("TurboJPEG encoder not initialized");
        return false;
    }
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0)
    {
        return false;
    }
    if (frame.channels() != 3)
    {
        spdlog::error("TurboJPEG encoder only supports 3-channel BGR images");
        return false;
    }

    unsigned char* jpeg_buf = nullptr;
    unsigned long jpeg_size = 0;

    int width = frame.cols;
    int height = frame.rows;
    int pitch = static_cast<int>(frame.step[0]);

    const auto t_tj_start = std::chrono::steady_clock::now();
    int ret = tjCompress2(
        handle_,
        frame.data,
        width,
        pitch,
        height,
        TJPF_BGR,
        &jpeg_buf,
        &jpeg_size,
        TJSAMP_420,
        quality_,
        TJFLAG_FASTDCT);

    if (ret < 0)
    {
        spdlog::error("TurboJPEG encode failed: {}", tjGetErrorStr2(handle_));
        return false;
    }

    const auto t_copy = std::chrono::steady_clock::now();
    out_buffer.assign(reinterpret_cast<const char*>(jpeg_buf), jpeg_size);
    const auto t_end = std::chrono::steady_clock::now();
    tjProfile(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t_copy - t_tj_start).count()),
              static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_copy).count()),
              jpeg_size);
    tjFree(jpeg_buf);
    return true;
}
