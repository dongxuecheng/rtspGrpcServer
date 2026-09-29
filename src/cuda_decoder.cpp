#include "cuda_decoder.hpp"
#include <cuda_runtime.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

bool CudaDecoder::open(const std::string &url)
{
    // 确保在当前目标 GPU 的 CUDA context 下创建 NVDEC parser/decoder。
    // IO 线程可能不是当初初始化 CUDA 的线程，若不显式绑定 context，
    // cuvidCreateVideoParser 内部 cuCtxGetCurrent() 可能拿到 nullptr 或错误的 GPU。
    CUDATools::AutoDevice auto_device_exchange(gpu_id_);

    last_url_ = url;      // 保存 URL 供后续重连使用
    frames_to_skip_ = 0; // GPU 解码器需要更多帧来预热和稳定 BGR 转换

    // 每次尝试前先清理资源
    release();

    // 1. 创建解封装器（auto_reboot=false，由上层 StreamTask 统一控制重连）
    bool low_latency = this->onlyKeyFrames(); // 如果只处理关键帧，可以启用低延迟模式
    demuxer_ = FFHDDemuxer::create_ffmpeg_demuxer(url, false, this->only_key_frames_);
    if (!demuxer_)
    {
        spdlog::warn("[CudaDecoder] Failed to create demuxer for {}", url);
        return false;
    }

    // 2. 创建硬解码器
    // bUseDeviceFrame = true: 数据保留在 GPU，减少不必要的拷贝
    // output_bgr = true : 在 GPU 上完成 NV12→BGR 转换
    // output_bgr = false: 直接输出 NV12（请求 NV12 时用，省掉色彩核）
    const bool output_bgr = (output_pixel_format_ == static_cast<uint32_t>(PixelFormat::BGR));
    decoder_ = FFHDDecoder::create_cuvid_decoder(
        true, // bUseDeviceFrame = true
        FFHDDecoder::ffmpeg2NvCodecId(demuxer_->get_video_codec()),
        low_latency,
        5,       // max_cache
        gpu_id_, // gpu_id
        nullptr,
        nullptr,
        output_bgr);

    if (!decoder_)
    {
        spdlog::warn("[CudaDecoder] Failed to create decoder for {}", url);
        demuxer_.reset(); // 释放 demuxer
        return false;
    }
    spdlog::info("[CudaDecoder] NVDEC output format: {}", output_bgr ? "BGR24" : "NV12(native)");

    // 记录 decoder 创建时间，用于 grab() 判断首帧阶段，避免空包/no-frame 过早失败。
    open_time_ = std::chrono::steady_clock::now();

    // 3. 成功创建！推送额外数据并返回
    uint8_t *extra_data = nullptr;
    int extra_size = 0;
    demuxer_->get_extra_data(&extra_data, &extra_size);
    if (extra_size > 0)
    {
        decoder_->decode(extra_data, extra_size);
    }

    is_opened_ = true;
    spdlog::info("[CudaDecoder] Successfully opened stream: {}", url);
    return true;
}

bool CudaDecoder::reconnect()
{
    // 重连策略统一由上层 StreamTask 控制，底层只负责重新 open 一次
    spdlog::warn("[CudaDecoder] Reconnecting to: {}", last_url_);
    return open(last_url_);
}

bool CudaDecoder::isOpened() const
{
    return is_opened_ && demuxer_ != nullptr && decoder_ != nullptr;
}

bool CudaDecoder::grab()
{
    if (!isOpened())
    {
        spdlog::warn("[CudaDecoder] grab() called but not opened");
        return false;
    }

    // IO 线程与计算线程可能不是同一个线程，每次操作 NVDEC 前必须绑定目标 GPU。
    CUDATools::AutoDevice auto_device_exchange(gpu_id_);

    // 如果内部还有未取完的帧，不要去拉取新包覆盖！
    if (decoded_frames_available_ > 0)
    {
        return true;
    }

    uint8_t *packet_data = nullptr;
    int packet_size = 0;
    bool is_key = false;

    while (true)
    {
        if (!demuxer_->demux(&packet_data, &packet_size, &last_pts_, &is_key))
        {
            // demux 失败直接返回，由上层 StreamTask 统一处理重连
            return false;
        }

        decoded_frames_available_ = decoder_->decode(packet_data, packet_size, last_pts_);

        if (decoded_frames_available_ > 0)
            break;

        // NVDEC 硬错误（会话创建失败 / 显存不足 / 解析器损坏）：
        // 标记解码器为未打开并返回 false，下一次 stepIO 会走完整 open() 重建，
        // 避免在坏掉的解码器上无限喂包导致流僵尸化
        if (decoded_frames_available_ < 0)
        {
            spdlog::warn("[CudaDecoder] decode hard error for {}, marking decoder broken", last_url_);
            is_opened_ = false;
            return false;
        }

        // 首帧阶段：空包或尚未解码出一帧时继续喂数据，避免 NVDEC 初始化期被误判为失败。
        // av_read_frame 本身有 stimeout，不会无限空转。
        if (packet_size == 0)
        {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - open_time_)
                                  .count();
            if (elapsed_ms < 10000) // 10 秒内容忍空包
            {
                continue;
            }
            spdlog::warn("[CudaDecoder] Empty packet persists beyond first-frame window for {}", last_url_);
            return false;
        }
    }
    return true;
}

bool CudaDecoder::retrieve(cv::Mat &frame, bool need_data)
{
    if (!isOpened() || decoded_frames_available_ <= 0)
    {
        return false;
    }
        

    void *gpu_ptr = decoder_->get_frame(&last_pts_, &last_frame_index_);
    if (!gpu_ptr)
    {
        return false;
    }
        

    // 保存 GPU 帧指针
    last_gpu_frame_ptr_ = static_cast<uint8_t *>(gpu_ptr);

    if (frames_to_skip_ > 0)
    {
        frames_to_skip_--;
        decoded_frames_available_--;
        return false;
    }

    if (need_data)
    {
        // 需要数据时，从 GPU 拷贝到 CPU
        int width = decoder_->get_width();
        int height = decoder_->get_height();
        int frame_bytes = decoder_->get_frame_bytes();

        // 防御编程：视频格式尚未解析时跳过
        if (width == 0 || height == 0 || frame_bytes == 0)
        {
            spdlog::warn("Video format not yet parsed, width={}, height={}, frame_bytes={}",
                         width, height, frame_bytes);
            decoded_frames_available_--;
            return false;
        }

        // 调试信息
        // spdlog::info("Frame info: {}x{}, frame_bytes={}, expected_bgr_bytes={}",
        //              width, height, frame_bytes, width * height * 3);

        // 使用解码器报告的实际大小
        // NVDEC 在 output_bgr=false 时输出紧密排列的 NV12（pitch == width），
        // 因此这里按“图像宽高”分配对应形状的缓冲，行数与图像高度不一定相等。
        if (output_pixel_format_ == static_cast<uint32_t>(PixelFormat::NV12))
        {
            const int chroma_h = (height + 1) / 2;
            frame.create(height + chroma_h, width, CV_8UC1);
        }
        else
        {
            frame.create(height, width, CV_8UC3);
        }

        // 防御：解码器报告的字节数与目标缓冲不一致时不要越界写
        const size_t dst_bytes = frame.total() * frame.elemSize();
        const size_t copy_bytes = std::min<size_t>(static_cast<size_t>(frame_bytes), dst_bytes);
        if (static_cast<size_t>(frame_bytes) != dst_bytes)
        {
            spdlog::warn("[CudaDecoder] frame_bytes {} != buffer bytes {} (fmt={}) for {}x{}",
                         frame_bytes, dst_bytes, pixelFormatName(output_pixel_format_), width, height);
        }

        cudaError_t err;
        if (cuda_stream_)
        {
            err = cudaMemcpyAsync(frame.data, gpu_ptr, copy_bytes, cudaMemcpyDeviceToHost, cuda_stream_);
            if (err != cudaSuccess)
            {
                spdlog::error("cudaMemcpyAsync failed: {}", cudaGetErrorString(err));
                decoded_frames_available_--;
                return false;
            }
            cudaStreamSynchronize(cuda_stream_);
        }
        else
        {
            err = cudaMemcpy(frame.data, gpu_ptr, copy_bytes, cudaMemcpyDeviceToHost);
            if (err != cudaSuccess)
            {
                spdlog::error("cudaMemcpy failed: {}", cudaGetErrorString(err));
                decoded_frames_available_--;
                return false;
            }
            cudaDeviceSynchronize();
        }
    }

    decoded_frames_available_--;
    return true;
}

// 原生 NV12 路径：解码器以 output_bgr=false 创建时，get_frame() 返回的就是 NV12，
// 只需一次 D2H 拷贝，不做任何色彩转换。
// 其他格式（I420/YUYV422/BGR）或解码器按 BGR 创建时退回通用实现（BGR→YUV）。
bool CudaDecoder::retrieveRaw(RawFrame &out, uint32_t pixel_format)
{
    const uint32_t nv12 = static_cast<uint32_t>(PixelFormat::NV12);
    if (pixel_format != nv12 || output_pixel_format_ != nv12)
    {
        return IVideoDecoder::retrieveRaw(out, pixel_format);
    }

    out.release();
    if (!retrieve(out.buffer, true) || out.buffer.empty())
        return false;

    out.width = decoder_ ? decoder_->get_width() : 0;
    out.height = decoder_ ? decoder_->get_height() : 0;
    out.step = static_cast<uint32_t>(out.width); // 紧密排列：NV12 的 Y 平面行字节数 = 宽度
    out.pixel_format = pixel_format;
    return out.width > 0 && out.height > 0;
}

int CudaDecoder::getWidth() const
{
    return decoder_ ? decoder_->get_width() : 0;
}

int CudaDecoder::getHeight() const
{
    return decoder_ ? decoder_->get_height() : 0;
}

void CudaDecoder::release()
{
    is_opened_ = false;

    // 🔧 关键修复：确保 decoder 和 demuxer 被彻底释放
    // shared_ptr::reset() 会调用引用计数，当计数为 0 时会自动调用析构函数
    if (decoder_)
    {
        decoder_->decode(nullptr, 0);
        decoder_.reset();
        decoder_ = nullptr;
    }
    if (demuxer_)
    {
        demuxer_.reset();
        demuxer_ = nullptr;
    }

    decoded_frames_available_ = 0;
    last_gpu_frame_ptr_ = nullptr;
}