#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <opencv2/opencv.hpp>
#include "pixel_format.hpp"

// 一帧原始画面（共享内存零拷贝通道使用）。
//
// buffer 始终是**连续紧密排列**的缓冲（无行末 padding），其形状随 pixel_format 变化：
//   BGR     : (H,        W,   CV_8UC3)
//   NV12    : (H + H/2,  W,   CV_8UC1)   ← 上半部 Y，下半部交错 UV
//   I420    : (H + H/2,  W,   CV_8UC1)   ← 上半部 Y，再 U、V 两个 1/4 平面
//   YUYV422 : (H,        W,   CV_8UC2)   ← 每元素 [Y,U,Y,V]
// 注意 width/height 始终是**图像**宽高，与 buffer 的行数不一定相等（YUV 420 的缓冲更高）。
struct RawFrame
{
    cv::Mat buffer;
    int width = 0;
    int height = 0;
    // 一行的字节数（缓冲紧密排列，无行末 padding）：
    //   BGR     -> width*3，NV12/I420 -> width，YUYV422 -> width*2
    uint32_t step = 0;
    uint32_t pixel_format = static_cast<uint32_t>(PixelFormat::BGR);

    bool empty() const { return buffer.empty() || width <= 0 || height <= 0; }
    void release()
    {
        buffer.release();
        width = 0;
        height = 0;
        step = 0;
        pixel_format = static_cast<uint32_t>(PixelFormat::BGR);
    }
};

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

    // 以指定像素格式取出一帧原始画面（共享内存零拷贝通道使用）。
    //
    // 默认实现：先按 retrieve() 拿到 BGR，再在 CPU 上转成目标 YUV（通用、无额外风险，
    // 但多一次转换）。解码器应尽量覆写以获得原生/零转换输出：
    //   - CpuDecoder  : sws_scale 直接输出目标格式，无 BGR 往返
    //   - CudaDecoder : NVDEC 原生 NV12，跳过 NV12→BGR 色彩核
    // 返回值语义与 retrieve() 一致：false 表示本帧不可用（无新帧/跳帧）。
    virtual bool retrieveRaw(RawFrame &out, uint32_t pixel_format)
    {
        out.release();
        if (!retrieve(out.buffer, true) || out.buffer.empty())
        {
            return false;
        }

        out.width = out.buffer.cols;
        out.height = out.buffer.rows;
        out.step = static_cast<uint32_t>(out.buffer.step[0]);
        out.pixel_format = pixel_format;

        if (pixel_format == static_cast<uint32_t>(PixelFormat::BGR))
        {
            return true;
        }

        // 回退路径：BGR → I420（OpenCV 原生支持），再按需重排成 NV12 / YUYV422
        cv::Mat i420;
        cv::cvtColor(out.buffer, i420, cv::COLOR_BGR2YUV_I420);
        if (i420.empty())
        {
            out.release();
            return false;
        }
        return repackYuv420(i420, out, pixel_format);
    }

    // 提示解码器“原始帧期望的输出像素格式”（见 PixelFormat）。
    // 必须在 open() 之前调用；默认忽略（解码器走 retrieveRaw 的通用回退路径）。
    // 主要用途：让 NVDEC 直接输出 NV12（跳过色彩转换）。
    virtual void setOutputPixelFormat(uint32_t pixel_format) { (void)pixel_format; }

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

    // 最近一次 grab() 得到的**那一帧本身**是否花屏。
    // 与 getDecodeHealth()（流的累计统计）不同：这个是逐帧的，用来回答
    // “我拿到的这一帧能不能用”。
    // 注意：NVDEC 路径会把花屏帧直接丢弃（不会下发），因此这里恒为 false——
    // 想知道“有没有丢过帧”请看 getDecodeHealth().corrupted_frames。
    virtual bool lastFrameCorrupted() const { return false; }

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

protected:
    // 把 OpenCV 的 I420 缓冲（Y + U + V 三个平面）重排成目标 YUV 格式。
    //
    // 这里的转换是 4:2:0 → 4:2:2 的色度上采样，只用于**回退路径**
    // （原生支持目标格式的解码器不会走到这里）。上采样采用最近邻，
    // 目的是“格式正确、可直接喂给下游”，不追求色彩质量。
    static bool repackYuv420(const cv::Mat &i420, RawFrame &out, uint32_t pixel_format)
    {
        const int w = out.width;
        const int h = out.height;
        const int cw = (w + 1) / 2;              // 色度宽
        const int ch = (h + 1) / 2;              // 色度高
        const uint8_t *y = i420.data;
        const uint8_t *u = y + static_cast<size_t>(w) * h;
        const uint8_t *v = u + static_cast<size_t>(cw) * ch;

        switch (pixel_format)
        {
        case static_cast<uint32_t>(PixelFormat::NV12):
        {
            // NV12 = Y 平面 + 交错 UV 平面
            out.buffer.create(h + ch, w, CV_8UC1);
            out.step = static_cast<uint32_t>(w);
            uint8_t *dst = out.buffer.data;
            std::memcpy(dst, y, static_cast<size_t>(w) * h);
            uint8_t *uv = dst + static_cast<size_t>(w) * h;
            for (int r = 0; r < ch; ++r)
            {
                for (int c = 0; c < cw; ++c)
                {
                    uv[static_cast<size_t>(r) * w + c * 2] = u[r * cw + c];
                    uv[static_cast<size_t>(r) * w + c * 2 + 1] = v[r * cw + c];
                }
            }
            return true;
        }
        case static_cast<uint32_t>(PixelFormat::I420):
        {
            // OpenCV 的 COLOR_BGR2YUV_I420 已经是目标布局，直接拷贝
            out.buffer.create(h + ch, w, CV_8UC1);
            out.step = static_cast<uint32_t>(w);
            std::memcpy(out.buffer.data, i420.data, static_cast<size_t>(w) * h +
                                                            static_cast<size_t>(cw) * ch * 2);
            return true;
        }
        case static_cast<uint32_t>(PixelFormat::YUYV422):
        {
            // YUYV: 每 2 像素 4 字节 [Y0,U,Y1,V]，色度取 4:2:0 平面上对应位置
            out.buffer.create(h, w, CV_8UC2);
            out.step = static_cast<uint32_t>(w * 2);
            uint8_t *dst = out.buffer.data;
            for (int r = 0; r < h; ++r)
            {
                const uint8_t *y_row = y + static_cast<size_t>(r) * w;
                const uint8_t *u_row = u + static_cast<size_t>(r / 2) * cw;
                const uint8_t *v_row = v + static_cast<size_t>(r / 2) * cw;
                uint8_t *d_row = dst + static_cast<size_t>(r) * w * 2;
                for (int c = 0; c < w; c += 2)
                {
                    d_row[c * 2] = y_row[c];
                    d_row[c * 2 + 1] = u_row[c / 2];
                    if (c + 1 < w)
                    {
                        d_row[c * 2 + 2] = y_row[c + 1];
                    }
                    d_row[c * 2 + 3] = v_row[c / 2];
                }
            }
            return true;
        }
        default:
            out.release();
            return false;
        }
    }
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