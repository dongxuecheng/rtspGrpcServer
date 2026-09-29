#pragma once

#include <cstddef>
#include <cstdint>

// 原始帧（共享内存 payload）的像素格式。
//
// ⚠️ 数值必须与 stream_service.proto 的 PixelFormat 枚举保持一致，
//    同时也会写进 ShmMeta::pixel_format 供消费端解析。
//
// 各格式的内存布局约定（缓冲一律**紧密排列**，无行末 padding）：
//   BGR24    : H 行 × W 像素 × 3 通道（B,G,R 交错），共 W*H*3 字节
//   NV12     : Y 平面 (W*H) 后紧跟交错的 UV 平面 (W*H/2)【YUV420SP】，共 W*H*3/2 字节
//   I420     : Y (W*H) + U (W*H/4) + V (W*H/4) 三个平面【YUV420P】，共 W*H*3/2 字节
//   YUYV422  : 每 2 像素 4 字节（Y0,U,Y1,V 交错）【packed 4:2:2】，共 W*H*2 字节
enum class PixelFormat : uint32_t
{
    BGR = 0,     // 默认：BGR24，兼容旧客户端
    NV12 = 1,    // YUV420SP
    I420 = 2,    // YUV420P
    YUYV422 = 3, // packed 4:2:2
};

// 该格式是否属于 YUV 家族（非 BGR）
inline bool isYuvPixelFormat(uint32_t fmt)
{
    return fmt != static_cast<uint32_t>(PixelFormat::BGR);
}

// 格式名称（日志/调试用）
inline const char *pixelFormatName(uint32_t fmt)
{
    switch (fmt)
    {
    case static_cast<uint32_t>(PixelFormat::BGR):
        return "BGR24";
    case static_cast<uint32_t>(PixelFormat::NV12):
        return "NV12(YUV420SP)";
    case static_cast<uint32_t>(PixelFormat::I420):
        return "I420(YUV420P)";
    case static_cast<uint32_t>(PixelFormat::YUYV422):
        return "YUYV422";
    default:
        return "UNKNOWN";
    }
}

// 给定图像宽高，返回该格式一帧的字节数（紧密排列，不含任何 padding）。
// 宽高非法时返回 0。
inline size_t pixelFormatFrameBytes(uint32_t fmt, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    switch (fmt)
    {
    case static_cast<uint32_t>(PixelFormat::BGR):
        return w * h * 3;
    case static_cast<uint32_t>(PixelFormat::NV12):
    case static_cast<uint32_t>(PixelFormat::I420):
        return w * h * 3 / 2;
    case static_cast<uint32_t>(PixelFormat::YUYV422):
        return w * h * 2;
    default:
        return 0;
    }
}

// 像素格式对应的“通道数”，仅用于填充 ShmMeta::channels（消费端应以 pixel_format 为准）：
//   BGR24   -> 3，NV12/I420 -> 1（单缓冲，平面信息由 pixel_format 决定），YUYV422 -> 2
inline uint32_t pixelFormatChannels(uint32_t fmt)
{
    switch (fmt)
    {
    case static_cast<uint32_t>(PixelFormat::BGR):
        return 3;
    case static_cast<uint32_t>(PixelFormat::YUYV422):
        return 2;
    default:
        return 1;
    }
}
