#pragma once
#include <string>
#include <opencv2/opencv.hpp>
#include <cstdint>

// 视频解码器接口
class IVideoDecoder
{
public:
    virtual ~IVideoDecoder() = default;
    virtual bool open(const std::string &url) = 0;
    virtual bool isOpened() const = 0;
    virtual bool grab() = 0;
    virtual bool retrieve(cv::Mat &frame, bool need_data = true) = 0;
    virtual void release() = 0;

    // 如果解码器本身已输出编码后的帧（如海康 SDK 抓图返回 JPEG），
    // 可实现此方法直接透传，避免解码再编码的二次损耗。默认不支持。
    virtual bool getEncodedFrame(std::string &out_buffer) { (void)out_buffer; return false; }

    // 最近一帧的媒体时间轴时间戳（毫秒）。用于判断“服务端是否已经落后于源”：
    // 把媒体时间轴推进量与实际墙钟推进量对比，差值持续变大即说明接收缓冲在堆积
    // （表现为：看的画面越来越滞后于实时）。不支持时返回 0。
    virtual int64_t lastFramePtsMs() const { return 0; }

    // ==================== 花屏（解码质量）判定 ====================
    //
    // 思路：优先用“解码器自报的错误”，而不是去猜像素。
    // 解码器知道哪一帧缺了参考帧、哪一帧做了错误掩盖（concealment），
    // 这类帧在画面上就是灰块/绿块/马赛克（即“花屏”）。
    // 这个信号零误报、几乎零开销，而且比任何像素域启发式都准。
    //
    // 各实现提供的信号：
    //   - CpuDecoder  (FFmpeg)  : AVFrame::decode_error_flags
    //                             （MISSING_REFERENCE / CONCEALMENT_ACTIVE / INVALID_BITSTREAM）
    //   - CudaDecoder (NVDEC)   : cuvidGetDecodeStatus -> cuvidDecodeStatus_Error /
    //                             cuvidDecodeStatus_Error_Concealed
    //   - HikDecoder  (SDK 抓图): 由 SDK 出图，不适用（恒为 0）
    struct DecodeHealth
    {
        uint64_t corrupted_frames = 0;   // 累计：解码器判定“出帧了，但解码过程有错/做了错误掩盖”
        uint64_t missing_reference = 0;  // 其中：缺参考帧（必然花屏）
        uint64_t invalid_bitstream = 0;  // 其中：码流非法/损坏
        uint64_t error_concealed = 0;    // 其中：解码器明确标记为“错误掩盖”（NVDEC）
        int64_t last_error_wall_ms = 0;  // 最近一次错误的墙钟时间（ms），0 表示从未出现
    };
    virtual DecodeHealth getDecodeHealth() const { return {}; }

    // 清空花屏计数（重连/换流后调用，避免新旧流的统计混在一起）
    virtual void resetDecodeHealth() {}

    // GPU 帧支持（可选实现）
    virtual bool isGpuFrame() const { return false; }
    virtual uint8_t* getGpuFramePtr() { return nullptr; }
    virtual int getWidth() const { return 0; }
    virtual int getHeight() const { return 0; }
    virtual bool onlyKeyFrames() const { return false; }

    // grab() 失败后是否立即 release() 并重新 open()。默认 true（RTSP/FFmpeg 行为）。
    // 海康 SDK 等短连接抓图模式可返回 false，由解码器内部决定是否重连，避免重复登录。
    virtual bool releaseOnGrabFailure() const { return true; }

    // updateUrl() 触发切换时，StreamTask 是否应在 IO 线程先调用 release()。
    // 默认 true。海康 SDK 可返回 false，由 open(new_url) 内部判断参数是否变化，
    // 从而避免相同参数下反复 Login/Logout。
    virtual bool releaseOnUrlChange() const { return true; }
};

// 图像压缩编码器接口
class IImageEncoder
{
public:
    virtual ~IImageEncoder() = default;
    virtual bool encode(const cv::Mat &frame, std::string &out_buffer) = 0;
    
    // GPU 帧编码支持（可选实现）
    virtual bool encodeGpu(uint8_t* gpu_bgr_ptr, int width, int height, std::string &out_buffer) {
        return false; // 默认不支持
    }
    virtual bool supportsGpuEncode() const { return false; }
};