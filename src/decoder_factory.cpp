#include "decoder_factory.h"
#include "opencv_decoder.h"
#include "cuda_decoder.h"
#include "ffmpeg_decoder.h"
#include <iostream>

std::unique_ptr<IVideoDecoder> DecoderFactory::create(streamingservice::DecoderType type, int gpu_id)
{
    switch (type)
    {
    case streamingservice::DECODER_CPU_OPENCV:
        return std::make_unique<OpencvDecoder>();

    case streamingservice::DECODER_GPU_CUDA:
        return std::make_unique<CudaDecoder>(gpu_id);

    case streamingservice::DECODER_FFMPEG_NATIVE:
        return std::make_unique<FFmpegDecoder>();
        
    default:
        return std::make_unique<OpencvDecoder>();
    }
}