#include "rtsp_service.hpp"
#include "decoder_factory.hpp"
#include "opencv_encoder.hpp"
#include "nvjpeg_encoder.hpp"
#include "utils.hpp"
#include "task_scheduler.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

// =============================================================
// 构造函数：启动清理线程
// =============================================================
RTSPServiceImpl::RTSPServiceImpl() : manager_running_(true)
{
    cleanup_thread_ = std::thread(&RTSPServiceImpl::cleanupLoop, this);
}

// =============================================================
// 析构函数：停止清理线程和所有流
// =============================================================
RTSPServiceImpl::~RTSPServiceImpl()
{
    manager_running_ = false;
    if (cleanup_thread_.joinable())
    {
        cleanup_thread_.join();
    }
    // 停止所有流任务
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto &pair : streams_)
        {
            if (pair.second)
            {
                pair.second->stop();
            }
        }
    }
}

// =============================================================
// StartStream：开启拉流（包含查重、注入解码器/编码器）
// =============================================================
grpc::Status RTSPServiceImpl::StartStream(grpc::ServerContext *context, const streamingservice::StartRequest *request, streamingservice::StartResponse *response)
{
    std::string req_url = request->rtsp_url();

    // 1. 查重逻辑：检查是否已经有相同的 URL 在拉流
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        for (auto &pair : streams_)
        {
            if (pair.second->getUrl() == req_url)
            {
                // 复用已存在的流
                std::string existing_id = pair.first;
                pair.second->keepAlive(); // 刷新心跳保活

                spdlog::info("[REUSE] URL already exists. Returning ID: {}", existing_id);

                response->set_success(true);
                response->set_stream_id(existing_id);
                response->set_message("Stream already exists, reusing existing task.");
                return grpc::Status::OK;
            }
        }
    }

    // 2. 创建解码器 (Decoder)
    auto decoder_type = request->decoder_type();
    int gpu_id = request->gpu_id();  // 默认为 0
    // 0 ffmpeg 1 nvcuvid
    std::string decode_type_str = (decoder_type == streamingservice::DECODER_CPU_FFMPEG) ? "FFmpeg" : (decoder_type == streamingservice::DECODER_GPU_NVCUVID) ? "GPU" : "UNKONW";
    spdlog::info("Decoder type: {}, GPU ID: {}", decode_type_str, gpu_id);
    auto decoder = DecoderFactory::create(decoder_type, gpu_id);
    if (!decoder)
    {
        response->set_success(false);
        response->set_message("Failed to create requested decoder type");
        return grpc::Status::OK;
    }

    // 3. 创建编码器 (Encoder)
    std::shared_ptr<IImageEncoder> encoder;
    if (decoder_type == streamingservice::DECODER_GPU_NVCUVID)
    {
        // nvjpegEncoder 内部会调用 cudaSetDevice(gpu_id)，确保在正确的 GPU 上编码
        encoder = std::make_shared<NvjpegEncoder>(85, gpu_id);
        spdlog::info("Using NVJPEG GPU encoder");
    }
    else
    {
        encoder = std::make_shared<OpencvEncoder>(85);
        spdlog::info("Using OpenCV CPU encoder");
    }

    // 4. 创建流任务 (StreamTask)
    // 注意：构造函数不再启动线程，仅初始化数据结构
    auto task = std::make_shared<StreamTask>(
        req_url,
        request->heartbeat_timeout_ms(),
        request->decode_interval_ms(),
        static_cast<int>(decoder_type),
        request->keep_on_failure(),
        std::move(decoder),
        encoder
    );

    std::string stream_id = generate_uuid();

    // 5. 再次加锁存入 Map (双重检查)
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        // 防止在创建 Task 的间隙，别的线程插入了同样的流
        for (auto &pair : streams_)
        {
            if (pair.second->getUrl() == req_url)
            {
                task->stop(); // 停止刚才新创建的冗余任务
                response->set_success(true);
                response->set_stream_id(pair.first);
                response->set_message("Stream created by another request concurrently.");
                return grpc::Status::OK;
            }
        }
        streams_[stream_id] = task;
    }

    // 【修改】显式启动任务
    // 这会将第一个任务投递到 IO 线程池，开始运行
    task->start();

    response->set_success(true);
    response->set_stream_id(stream_id);
    response->set_message("Stream started successfully");
    spdlog::info("[START] New Stream ID: {} | URL: {}", stream_id, req_url);
    return grpc::Status::OK;
}

// =============================================================
// StopStream：停止拉流
// =============================================================
grpc::Status RTSPServiceImpl::StopStream(grpc::ServerContext *context, const streamingservice::StopRequest *request, streamingservice::StopResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task_to_stop;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task_to_stop = it->second;
            streams_.erase(it); // 从 Map 中移除，引用计数 -1
        }
    }

    if (task_to_stop)
    {
        // 这里的 stop() 只是设置标志位并尝试释放资源
        // 真正的任务会在下一次线程池调度时结束
        task_to_stop->stop(); 
        spdlog::info("[STOP] Stream manually stopped: {}", stream_id);
        response->set_success(true);
        response->set_message("Stream stopped successfully");
    }
    else
    {
        response->set_success(false);
        response->set_message("Stream ID not found");
    }
    return grpc::Status::OK;
}

// =============================================================
// GetLatestFrame：获取最新帧
// =============================================================
grpc::Status RTSPServiceImpl::GetLatestFrame(grpc::ServerContext *context, const streamingservice::FrameRequest *request, streamingservice::FrameResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task = it->second;
        }
    }

    if (!task)
    {
        response->set_success(false);
        response->set_message("Stream ID not found or expired");
        return grpc::Status::OK;
    }

    std::string buffer;
    if (task->getLatestEncodedFrame(buffer))
    {
        response->set_success(true);
        response->set_image_data(buffer); 
        response->set_message("OK");
    }
    else
    {
        response->set_success(false);
        response->set_message("No frame available yet or stream disconnected");
    }
    return grpc::Status::OK;
}

// =============================================================
// StreamFrames：流式传输视频帧
// =============================================================
grpc::Status RTSPServiceImpl::StreamFrames(grpc::ServerContext *context, 
                                            const streamingservice::StreamRequest *request, 
                                            grpc::ServerWriter<streamingservice::FrameResponse>* writer)
{
    std::string stream_id = request->stream_id();
    int max_fps = request->max_fps();
    
    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task = it->second;
        }
    }

    if (!task)
    {
        streamingservice::FrameResponse response;
        response.set_success(false);
        response.set_message("Stream ID not found");
        writer->Write(response);
        return grpc::Status::OK;
    }

    int64_t frame_interval_us = 0;
    if (max_fps > 0)
    {
        frame_interval_us = 1000000LL / max_fps;
    }

    auto last_send_time = std::chrono::steady_clock::now();
    std::string last_frame_data;

    spdlog::info("[STREAM] Client connected to stream: {}, max_fps: {}", stream_id, max_fps);

    while (!context->IsCancelled())
    {
        // 检查任务是否仍在运行
        if (task->isStopped()) {
            streamingservice::FrameResponse response;
            response.set_success(false);
            response.set_message("Stream has been stopped");
            writer->Write(response);
            break;
        }

        // 帧率控制
        if (frame_interval_us > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_send_time).count();
            if (elapsed_us < frame_interval_us)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(frame_interval_us - elapsed_us));
            }
            last_send_time = std::chrono::steady_clock::now();
        }

        std::string encoded_frame;
        if (task->getLatestEncodedFrame(encoded_frame))
        {
            if (encoded_frame != last_frame_data)
            {
                streamingservice::FrameResponse response;
                response.set_success(true);
                response.set_image_data(encoded_frame);
                response.set_message("OK");

                if (!writer->Write(response))
                {
                    break;
                }

                last_frame_data = std::move(encoded_frame);
                task->keepAlive();
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        else
        {
            // 流可能正在连接中或暂时无数据
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    spdlog::info("[STREAM] Client disconnected from stream: {}", stream_id);
    return grpc::Status::OK;
}

// =============================================================
// CheckStream：查询流状态
// =============================================================
grpc::Status RTSPServiceImpl::CheckStream(grpc::ServerContext *context, const streamingservice::CheckRequest *request, streamingservice::CheckResponse *response)
{
    std::string stream_id = request->stream_id();
    std::shared_ptr<StreamTask> task;

    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = streams_.find(stream_id);
        if (it != streams_.end())
        {
            task = it->second;
        }
    }

    if (task)
    {
        response->set_status(static_cast<streamingservice::StreamStatus>(task->getStatus()));
        response->set_rtsp_url(task->getUrl());
        response->set_decoder_type(static_cast<streamingservice::DecoderType>(task->getDecoderType()));
        response->set_width(task->getWidth());
        response->set_height(task->getHeight());
        response->set_decode_interval_ms(task->getDecodeIntervalMs());
        
        switch (task->getStatus())
        {
            case StreamStatus::CONNECTED:
                response->set_message("已连接");
                break;
            case StreamStatus::CONNECTING:
                response->set_message("连接中");
                break;
            case StreamStatus::DISCONNECTED:
                response->set_message("无法连接");
                break;
        }
    }
    else
    {
        response->set_status(streamingservice::STATUS_NOT_FOUND);
        response->set_message("流不存在");
    }
    return grpc::Status::OK;
}

// =============================================================
// ListStreams：查询所有流信息
// =============================================================
grpc::Status RTSPServiceImpl::ListStreams(grpc::ServerContext *context, const streamingservice::ListStreamsRequest *request, streamingservice::ListStreamsResponse *response)
{
    std::lock_guard<std::mutex> lock(map_mutex_);

    response->set_total_count(static_cast<int>(streams_.size()));

    for (const auto &pair : streams_)
    {
        const std::string &stream_id = pair.first;
        const std::shared_ptr<StreamTask> &task = pair.second;

        auto *stream_info = response->add_streams();
        stream_info->set_stream_id(stream_id);
        stream_info->set_rtsp_url(task->getUrl());
        stream_info->set_status(static_cast<streamingservice::StreamStatus>(task->getStatus()));
        stream_info->set_decoder_type(static_cast<streamingservice::DecoderType>(task->getDecoderType()));
        stream_info->set_width(task->getWidth());
        stream_info->set_height(task->getHeight());
        stream_info->set_decode_interval_ms(task->getDecodeIntervalMs());
    }

    spdlog::info("[LIST] Listed {} streams", streams_.size());
    return grpc::Status::OK;
}

// =============================================================
// cleanupLoop：后台线程，清理心跳超时或已停止的流
// =============================================================
void RTSPServiceImpl::cleanupLoop()
{
    while (manager_running_)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::vector<std::shared_ptr<StreamTask>> tasks_to_stop;

        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            for (auto it = streams_.begin(); it != streams_.end();)
            {
                bool should_remove = false;
                std::string reason;
                
                if (it->second->isStopped() && !it->second->shouldKeepOnFailure())
                {
                    // 任务已停止且未设置保留，自动清理
                    should_remove = true;
                    reason = "STOPPED";
                }
                else if (it->second->isTimeout())
                {
                    // 心跳超时，无论是否设置保留都清理
                    should_remove = true;
                    reason = "TIMEOUT";
                }
                
                if (should_remove)
                {
                    spdlog::info("[{}] Auto-cleaning stream ID: {}", reason, it->first);
                    // 添加到停止列表，稍后在锁外停止
                    tasks_to_stop.push_back(it->second);
                    it = streams_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // 在锁外停止任务，防止死锁
        for (auto &task : tasks_to_stop)
        {
            task->stop();
        }
    }
}