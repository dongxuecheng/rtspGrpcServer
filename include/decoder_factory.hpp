#pragma once
#include <memory>
#include "interfaces.hpp"
#include "stream_service.grpc.pb.h"

class DecoderFactory {
public:
    // 根据 Protobuf 枚举创建对应的解码器
    // gpu_id: 仅对 GPU 解码有效，默认 0
    //
    // actual_type（可选，非空时写入）：**实际**生效的解码器类型。当请求的类型因构建配置
    // （ENABLE_CUDA=OFF）或硬件不可用（gpu_id 非法）而无法满足时会降级为 CPU_FFMPEG。
    // 调用方据此上报/提示，避免出现“请求 GPU、实际在跑 CPU”却无人知晓的情况。
    static std::unique_ptr<IVideoDecoder> create(streamingservice::DecoderType type, int gpu_id = 0,
                                                bool only_key_frames = false,
                                                streamingservice::DecoderType *actual_type = nullptr);
};