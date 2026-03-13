#include "decoder_factory.hpp"
#include "cuda_decoder.hpp"
#include "cpu_decoder.hpp"
#include <iostream>

std::unique_ptr<IVideoDecoder> DecoderFactory::create(streamingservice::DecoderType type, int gpu_id, bool only_key_frames)
{
    switch (type)
    {
    case streamingservice::DECODER_CPU_FFMPEG:
        return std::make_unique<CpuDecoder>(only_key_frames);

    case streamingservice::DECODER_GPU_NVCUVID:
        return std::make_unique<CudaDecoder>(gpu_id, only_key_frames);
        
    default:
        return std::make_unique<CpuDecoder>();
    }
}