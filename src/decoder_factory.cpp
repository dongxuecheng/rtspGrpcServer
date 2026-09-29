#include "decoder_factory.hpp"
#include "cpu_decoder.hpp"
#include <iostream>
#include <spdlog/spdlog.h>

#ifdef RTSP_ENABLE_CUDA
#include "cuda_decoder.hpp"
#include "cuda_tools.hpp"
#endif

#ifdef HAS_HIKVISION_SDK
#include "hik_decoder.hpp"
#endif

std::unique_ptr<IVideoDecoder> DecoderFactory::create(streamingservice::DecoderType type, int gpu_id, bool only_key_frames,
                                                      streamingservice::DecoderType *actual_type)
{
    // 默认：请求什么就是什么；只有真的降级时才改写
    if (actual_type)
    {
        *actual_type = type;
    }

    // 降级时记录“实际生效类型”，供上层如实上报
    auto fallback_to_cpu = [&]() {
        if (actual_type)
        {
            *actual_type = streamingservice::DECODER_CPU_FFMPEG;
        }
        return std::make_unique<CpuDecoder>(only_key_frames);
    };

    switch (type)
    {
    case streamingservice::DECODER_CPU_FFMPEG:
        return std::make_unique<CpuDecoder>(only_key_frames);

    case streamingservice::DECODER_GPU_NVCUVID:
#ifdef RTSP_ENABLE_CUDA
        if (!CUDATools::check_device_id(gpu_id))
        {
            spdlog::error("[DecoderFactory] Invalid gpu_id {}, falling back to CPU decoder", gpu_id);
            return fallback_to_cpu();
        }
        return std::make_unique<CudaDecoder>(gpu_id, only_key_frames);
#else
        spdlog::error("[DecoderFactory] GPU_NVCUVID requested but CUDA support disabled at build time, falling back to CPU decoder");
        return fallback_to_cpu();
#endif

    case streamingservice::DECODER_HIK_SDK:
#ifdef HAS_HIKVISION_SDK
        return std::make_unique<HikDecoder>();
#else
        spdlog::error("[DecoderFactory] HIK_SDK decoder requested but Hikvision SDK not available at build time, falling back to CPU decoder");
        return fallback_to_cpu();
#endif

    default:
        spdlog::warn("[DecoderFactory] Unknown decoder type {}, falling back to CPU decoder", type);
        return fallback_to_cpu();
    }
}
