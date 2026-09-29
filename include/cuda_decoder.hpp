#pragma once
#include "interfaces.hpp"
#include <opencv2/opencv.hpp>
#include <memory>
#include <queue>

// 假设这些头文件在您的包含路径中
#include "ffmpeg_demuxer.hpp"
#include "cuvid_decoder.hpp"
#include "cuda_tools.hpp"

class CudaDecoder : public IVideoDecoder
{
public:
    CudaDecoder(int gpu_id = 0, bool only_key_frames_ = false) : gpu_id_(gpu_id), only_key_frames_(only_key_frames_) {}
    virtual ~CudaDecoder() { release(); }

    bool open(const std::string &url) override;
    bool isOpened() const override;
    bool grab() override;
    bool retrieve(cv::Mat &frame, bool need_data = true) override;
    // 原生 NV12：NVDEC 输出本就是 NV12，这里直接 D2H 拷贝，跳过 NV12→BGR 色彩核
    bool retrieveRaw(RawFrame &out, uint32_t pixel_format) override;
    void release() override;

    // 提示期望的原始帧格式（需在 open() 之前调用）：
    //   NV12 → 创建解码器时 output_bgr=false，get_frame() 直接返回 NV12
    //   其他 → 保持原有 BGR 输出
    void setOutputPixelFormat(uint32_t pixel_format) override { output_pixel_format_ = pixel_format; }
    
    // GPU 帧支持
    bool isGpuFrame() const override { return true; }
    uint8_t* getGpuFramePtr() override { return last_gpu_frame_ptr_; }
    int getWidth() const override;
    int getHeight() const override;
    
    // GPU ID
    int getGpuId() const { return gpu_id_; }

    bool onlyKeyFrames() const override { return only_key_frames_; }

    void setStream(cudaStream_t stream) { cuda_stream_ = stream; }

    // 花屏信号（NVDEC 自报的错误掩盖帧计数）
    IVideoDecoder::DecodeHealth getDecodeHealth() const override
    {
        return decoder_ ? decoder_->getDecodeHealth() : IVideoDecoder::DecodeHealth{};
    }
    void resetDecodeHealth() override
    {
        if (decoder_)
            decoder_->resetDecodeHealth();
    }

private:
    std::shared_ptr<FFHDDemuxer::FFmpegDemuxer> demuxer_;
    std::shared_ptr<FFHDDecoder::CUVIDDecoder> decoder_;

    int gpu_id_ = 0;              // GPU ID
    bool is_opened_ = false;
    int64_t last_pts_ = 0;
    unsigned int last_frame_index_ = 0;
    uint8_t* last_gpu_frame_ptr_ = nullptr;  // 最后一帧的 GPU 指针

    // 用于处理解码器延迟（多个输入包才产出一帧，或一个包产出多帧的情况）
    int decoded_frames_available_ = 0;
    bool only_key_frames_ = false; // 是否只处理关键帧（可选）
    cudaStream_t cuda_stream_ = nullptr; // 每路流独立的 CUDA Stream

    std::chrono::steady_clock::time_point open_time_; // decoder 创建时间，用于首帧阶段判断

private:
    std::string last_url_;    // 保存 URL 用于重连
    int frames_to_skip_ = 15; // 待丢弃帧计数
    bool reconnect();         // 重连辅助函数

    // 期望的原始帧像素格式（PixelFormat），影响 NVDEC 是否做 NV12→BGR 转换。
    // 必须在 open() 之前通过 setOutputPixelFormat() 设置。
    uint32_t output_pixel_format_ = static_cast<uint32_t>(PixelFormat::BGR);
};