#include "stream_task.hpp"
#include <cstdlib>
#include "task_scheduler.hpp"
#include "opencv_encoder.hpp"
#include "turbojpeg_encoder.hpp"
#include <chrono>
#include <thread>
#include <random>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include "stream_service.pb.h"

#ifdef RTSP_ENABLE_CUDA
#include "nvjpeg_encoder.hpp"
#include "cuda_decoder.hpp"
#include "cuda_tools.hpp"
#endif

namespace {
    using EncoderPtr = std::shared_ptr<IImageEncoder>;

    static size_t makeEncoderKey(bool use_gpu_encoder, int gpu_id, int quality)
    {
        uint64_t key = use_gpu_encoder ? (1ULL << 63) : 0ULL;
        key |= (static_cast<uint64_t>(static_cast<uint32_t>(gpu_id)) << 32);
        key |= static_cast<uint64_t>(static_cast<uint32_t>(quality));
        return static_cast<size_t>(key);
    }

    static EncoderPtr getThreadLocalEncoder(bool use_gpu_encoder, int gpu_id, int quality)
    {
        static thread_local std::unordered_map<size_t, EncoderPtr> encoders;
        size_t key = makeEncoderKey(use_gpu_encoder, gpu_id, quality);
        auto it = encoders.find(key);
        if (it != encoders.end())
        {
            return it->second;
        }

        EncoderPtr encoder;
        if (use_gpu_encoder)
        {
#ifdef RTSP_ENABLE_CUDA
            encoder = std::make_shared<NvjpegEncoder>(quality, gpu_id);
#else
            spdlog::warn("[getThreadLocalEncoder] GPU encoder requested but CUDA disabled, using CPU encoder");
            encoder = std::make_shared<TurboJpegEncoder>(quality);
#endif
        }
        else
        {
            encoder = std::make_shared<TurboJpegEncoder>(quality);
        }
        encoders.emplace(key, encoder);
        return encoder;
    }
}

StreamTask::StreamTask(const std::string &url,
                       const std::string &stream_id,
                       int heartbeat_timeout_ms,
                       int decode_interval_ms,
                       int decoder_type,
                       int gpu_id,
                       bool keep_on_failure,
                       bool use_shared_mem,
                       std::unique_ptr<IVideoDecoder> decoder,
                       bool use_gpu_encoder,
                       int jpeg_quality,
                       uint32_t pixel_format)
    : url_(url),
      stream_id_(stream_id),
      heartbeat_timeout_ms_(heartbeat_timeout_ms),
      decode_interval_ms_(decode_interval_ms),
      decoder_type_(decoder_type),
      gpu_id_(gpu_id),
      saved_decoder_type_(decoder_type),
      saved_gpu_id_(gpu_id),
      keep_on_failure_(keep_on_failure),
      use_shared_mem_(use_shared_mem),
      pixel_format_(pixel_format),
      decoder_(std::move(decoder)),
      use_gpu_encoder_(use_gpu_encoder),
      jpeg_quality_(jpeg_quality)
{
    // 非 SHM（gRPC/JPEG）模式不产出原始帧，强制 BGR，避免误配
    if (!use_shared_mem_ && pixel_format_ != static_cast<uint32_t>(PixelFormat::BGR))
    {
        spdlog::warn("[StreamTask] pixel_format {} ignored for stream {}: only effective with use_shared_mem=true",
                     pixelFormatName(pixel_format_), stream_id_);
        pixel_format_ = static_cast<uint32_t>(PixelFormat::BGR);
    }

    // 必须在 decoder_->open() 之前告知期望的原始帧格式：
    // 解码器据此选择最优路径（CpuDecoder 用 sws 直出目标格式；CudaDecoder 让 NVDEC 直出 NV12）
    if (decoder_)
    {
        decoder_->setOutputPixelFormat(pixel_format_);
    }
#ifndef RTSP_ENABLE_CUDA
    // CUDA 未启用时，强制关闭 GPU 编码器，并把请求的 GPU 解码器类型修正为 CPU
    if (use_gpu_encoder_)
    {
        spdlog::warn("[StreamTask] CUDA disabled at build time, forcing CPU encoder for stream {}", stream_id_);
        use_gpu_encoder_ = false;
    }
    if (decoder_type_ == streamingservice::DECODER_GPU_NVCUVID)
    {
        spdlog::warn("[StreamTask] CUDA disabled at build time, decoder_type GPU_NVCUVID falls back to CPU_FFMPEG for stream {}", stream_id_);
        decoder_type_ = streamingservice::DECODER_CPU_FFMPEG;
    }
    if (saved_decoder_type_ == streamingservice::DECODER_GPU_NVCUVID)
    {
        saved_decoder_type_ = streamingservice::DECODER_CPU_FFMPEG;
    }
#endif

    // 初始化内存池
    frame_pool_ = FrameMemoryPool::create(3 * 1024 * 1024);
    updateHeartbeat();
    // 花屏持续多久后自动重连（秒，0 = 关闭）。默认关闭：重连会中断 SHM 客户端。
    if (const char *env_glitch = std::getenv("RTSP_GLITCH_RECONNECT_S"))
    {
        glitch_reconnect_s_ = std::max(0, std::atoi(env_glitch));
    }
    // 自测开关：伪造花屏帧，用于验证 逐帧标记/日志/proto/Web 全链路
    fake_glitch_enabled_ = (std::getenv("RTSP_FAKE_GLITCH") != nullptr);
    last_encode_time_ = std::chrono::steady_clock::now();
    // 首帧必须立即放行：初始截止时间放到过去
    next_process_deadline_ = std::chrono::steady_clock::now() - std::chrono::hours(1);
    first_grab_attempt_time_ = std::chrono::steady_clock::now() - std::chrono::hours(1);

#ifdef RTSP_ENABLE_CUDA
    // 为每路 GPU 流创建独立的 CUDA Stream
    if (gpu_id_ >= 0)
    {
        CUDATools::AutoDevice auto_device_exchange(gpu_id_);
        cudaError_t err = cudaStreamCreate(&cuda_stream_);
        if (err != cudaSuccess)
        {
            spdlog::warn("Failed to create CUDA stream for stream {}: {}", stream_id_, cudaGetErrorString(err));
            cuda_stream_ = nullptr;
        }
        else
        {
            spdlog::info("[StreamTask] CUDA stream created for stream {} on GPU {}", stream_id_, gpu_id_);
        }

        if (cuda_stream_ && decoder_)
        {
            auto *cuda_decoder = dynamic_cast<CudaDecoder *>(decoder_.get());
            if (cuda_decoder)
            {
                cuda_decoder->setStream(cuda_stream_);
            }
        }
    }
#endif

    if (use_shared_mem_)
    {
        // SHM 延迟到首次解码成功后创建，因为那时才知道实际帧分辨率
        // 每个流独立分配，互不影响
        spdlog::info("SharedMemory requested for stream: {} (will init after first frame, pixel_format={})",
                     stream_id_, pixelFormatName(pixel_format_));
    }
}

StreamTask::~StreamTask()
{
    spdlog::warn("==== ~StreamTask DESTROYED: {} ====", url_);
    
    // 1. 停止 IO 线程
    running_ = false;
    {
        std::unique_lock<std::mutex> sleep_lock(sleep_mutex_);
        sleep_cv_.notify_all();
    }
    if (io_thread_.joinable()) 
    {
        try
        {
            // 如果当前线程就是 io_thread_ 自己，这里会死锁，所以要判断
            if (std::this_thread::get_id() != io_thread_.get_id()) 
            {
                io_thread_.join();
            }
            else
            {
                // 防止在 io_thread_ 自身中析构 StreamTask 时，joinable 的 std::thread
                // 被成员析构函数销毁而触发 std::terminate。
                // detach 后该线程会立即结束（ioLoop 已因 running_=false 退出）。
                io_thread_.detach();
            }
        }
        catch (const std::system_error &e)
        {
            spdlog::error("[StreamTask] io_thread join/detach failed in destructor: {}", e.what());
        }
    }
    
    // 2. 析构时必须显式清理 SHM（即使 stop() 已经被调用过，cleanup() 是幂等的）
    //    同样先等待计算任务结束，避免并发清理
    {
        std::unique_lock<std::mutex> lock(compute_done_mutex_);
        compute_done_cv_.wait_for(lock, std::chrono::seconds(5), [this]
                                  { return compute_in_flight_.load() == 0; });
    }
    if (shm_channel_)
    {
        shm_channel_->cleanup();
        shm_channel_.reset();
    }
    
    // 3. 释放 decoder
    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        if (decoder_)
        {
            decoder_->release();
        }
    }
    
#ifdef RTSP_ENABLE_CUDA
    if (cuda_stream_)
    {
        CUDATools::AutoDevice auto_device_exchange(gpu_id_);
        cudaStreamDestroy(cuda_stream_);
        cuda_stream_ = nullptr;
        spdlog::info("[StreamTask] CUDA stream destroyed for stream {}", stream_id_);
    }
#endif
    // 清理最新的引用，引用计数减一，可能触发内存归还或释放
    {
        std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
        latest_encoded_frame_.reset();
    }
}

// ---------------------------------------------------------
// 可中断的休眠函数，取代死循环或阻塞式 sleep
// ---------------------------------------------------------
bool StreamTask::interruptibleSleep(int ms)
{
    if (ms <= 0)
        return true;

    std::unique_lock<std::mutex> lock(sleep_mutex_);
    sleep_cv_.wait_for(lock, std::chrono::milliseconds(ms), [this]()
                      { return !running_.load() || io_wakeup_; });

    return running_.load();
}

void StreamTask::ioLoop()
{
    while (running_)
    {
        try
        {
            stepIO();
        }
        catch (const std::exception &e)
        {
            spdlog::error("[ioLoop] Exception in stepIO for {}: {}", url_, e.what());
        }
        catch (...)
        {
            spdlog::error("[ioLoop] Unknown exception in stepIO for {}", url_);
        }

        if (!running_)
            break;

        int delay_ms = 0;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            delay_ms = next_io_delay_ms_;
            next_io_delay_ms_ = 0;
            io_wakeup_ = false;
        }

        if (delay_ms <= 0)
            delay_ms = 1; // 避免 busy loop

        if (!interruptibleSleep(delay_ms))
            break;
    }
}

void StreamTask::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
    {
        return;
    }

    stopped_ = false;
    status_ = StreamStatus::CONNECTING;
    consecutive_failures_ = 0;
    resetFirstFrameState();
    spdlog::info("StreamTask started: {} (first-frame grace period: {} ms)", url_, FIRST_FRAME_GRACE_PERIOD_MS);

    std::weak_ptr<StreamTask> weak_self = shared_from_this();
    io_thread_ = std::thread([weak_self](){
        if (auto self = weak_self.lock())
        {
            self->ioLoop();
        } }
    );
}

void StreamTask::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
    {
        return;
    }
    
    if (io_thread_.joinable()) 
    {
        try
        {
            if (std::this_thread::get_id() != io_thread_.get_id()) 
            {
                sleep_cv_.notify_all();
                io_thread_.join();
            }
            else
            {
                // 在 IO 线程内部调用 stop() 时无法 join 自己；如果此时 self 是最后一个
                // shared_ptr，StreamTask 会在 io_thread_ 中析构，导致 joinable 的
                // std::thread 被销毁而触发 std::terminate。detach 可避免该问题。
                io_thread_.detach();
            }
        }
        catch (const std::system_error &e)
        {
            spdlog::error("[StreamTask] io_thread join/detach failed in stop: {}", e.what());
        }
    }

    spdlog::info("StreamTask stopping: {}", url_);
    stopped_ = true;

    {
        std::unique_lock<std::mutex> sleep_lock(sleep_mutex_);
        sleep_cv_.notify_all();
    }

    // 等待所有已投递的计算任务完成，避免 SHM 在 write_frame 中途被 cleanup
    {
        std::unique_lock<std::mutex> lock(compute_done_mutex_);
        compute_done_cv_.wait(lock, [this]
                              { return compute_in_flight_.load() == 0; });
        spdlog::info("[StreamTask] All compute tasks finished for {}", url_);
    }

    std::shared_ptr<std::string> old_frame_to_release;
    {
        std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
        old_frame_to_release = std::move(latest_encoded_frame_); // 此时 latest_encoded_frame_ 变为空
        frame_cv_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        if (decoder_)
        {
            decoder_->release();
        }
    }

    // 5. 共享内存清理
    if (shm_channel_)
    {
        shm_channel_->cleanup();
        shm_channel_.reset(); // 安全重置
    }

    // 6. 状态重置
    status_ = StreamStatus::DISCONNECTED;
    connected_ = false;
    consecutive_failures_ = 0;
    resetFirstFrameState();
}

void StreamTask::scheduleNext(int force_delay_ms)
{
    if (!running_)
        return;

    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        next_io_delay_ms_ = force_delay_ms;
        io_wakeup_ = true;
    }

    sleep_cv_.notify_all();
}

void StreamTask::stepIO()
{
    if (!running_)
    {
        spdlog::info("IO step skipped (not running): {}", url_);
        return;
    }

    auto prof_t_wait_lock = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(decoder_mutex_);
    profileAdd(prof_lock_wait_us_, prof_t_wait_lock);

    // 处理解码器切换（协议变化，如 RTSP -> hik:// 或反向）
    if (decoder_changed_)
    {
        spdlog::info("IO Thread: Switching decoder to type {}, URL: {}", pending_decoder_type_, pending_url_);
        url_ = pending_url_;
        decoder_type_ = pending_decoder_type_;
        use_gpu_encoder_ = pending_use_gpu_encoder_;

        if (decoder_)
        {
            decoder_->release();
        }
        decoder_ = std::move(pending_decoder_);

        decoder_changed_ = false;
        url_changed_ = false; // 切换 decoder 时 URL 已同步更新
        status_ = StreamStatus::CONNECTING;

        // 再次清空缓存帧，防止 switchDecoder 清空后、替换 decoder 前又有旧 stepCompute 写入旧帧
        {
            std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
            latest_encoded_frame_.reset();
            frame_cv_.notify_all();
        }
    }
    else if (url_changed_)
    {
        spdlog::info("IO Thread: Switching to new URL: {}", pending_url_);
        url_ = pending_url_;
        url_changed_ = false;
        if (decoder_)
        {
            if (decoder_->releaseOnUrlChange())
            {
                decoder_->release(); // 在 IO 线程释放，不卡 gRPC 线程
            }
            else
            {
                // 不释放 decoder（如海康 SDK 希望保持登录），强制重新 open 让 decoder 自己判断参数是否变化
                force_reopen_ = true;
            }
        }

        // 再次清空缓存帧，防止 updateUrl 清空后、处理切换前又有旧 stepCompute 写入旧帧
        {
            std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
            latest_encoded_frame_.reset();
            frame_cv_.notify_all();
        }
    }

    if (!decoder_)
    {
        spdlog::error("Decoder not initialized for stream: {}", url_);
        return;
    }
        

    // 1. 处理连接断开重连逻辑
    bool need_open = !decoder_->isOpened() || force_reopen_.exchange(false);
    if (need_open)
    {
        // 如果已经离线过久且不要求持续重连，则停止任务
        if (shouldGiveUpReconnection())
        {
            spdlog::error("Stream offline too long, stopping task: {}", url_);
            lock.unlock(); // 尽早释放锁
            stop();
            return;
        }

        status_ = StreamStatus::CONNECTING;
        connected_ = false;

        spdlog::warn("Attempting to open/reconnect (failures={}): {}", consecutive_failures_, url_);

        if (decoder_->open(url_))
        {
            spdlog::info("Connected successfully: {}", url_);
            status_ = StreamStatus::CONNECTED;
            consecutive_failures_ = 0;
            // 连接成功后，重置编码时间，准备立即出第一帧
            last_encode_time_ = std::chrono::steady_clock::now() - std::chrono::hours(1);
            // 重连后源时间轴已变，抽帧截止时间重新对齐
            next_process_deadline_ = std::chrono::steady_clock::now() - std::chrono::hours(1);
            // 新解码器从 0 开始计数，花屏基线同步归零
            glitch_last_seen_total_ = 0;
            // 记录首次尝试抓取的时间，用于首帧宽限期判断
            first_grab_attempt_time_ = std::chrono::steady_clock::now();
            first_frame_seen_.store(false, std::memory_order_release);
        }
        else
        {
            spdlog::warn("Connection attempt failed: {}", url_);
            markConnectionFailure();
            lock.unlock();
            // 连接失败，指数退避后重试，不阻塞线程池
            int delay_ms = calculateReconnectDelayMs();
            spdlog::debug("Retry open after {} ms", delay_ms);
            scheduleNext(delay_ms);
            return;
        }
    }

    // 2. 抓取网络包 (grab 本身是阻塞的，由摄像头帧率控制节奏)
    auto grab_start = std::chrono::steady_clock::now();
    if (!decoder_->grab())
    {
        // 首帧宽限期：open 成功后的一段时间内，grab/demux 临时失败通常只是摄像头还没出帧，
        // 特别是关键帧模式或高并发时。此时不应记为连接失败，而是继续尝试读取。
        if (!first_frame_seen_.load(std::memory_order_acquire) && inFirstFrameGracePeriod())
        {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - first_grab_attempt_time_)
                                  .count();
            spdlog::warn("Frame grab failed during first-frame grace period: {} (elapsed={} ms, first_frame_seen={}, connected={}), retrying...",
                         url_, elapsed_ms, first_frame_seen_.load(std::memory_order_acquire), connected_);
            lock.unlock();
            scheduleNext(100); // 短暂重试，不增加 failure 计数，不 release decoder
            return;
        }

        spdlog::warn("Frame grab failed: {}", url_);
        markConnectionFailure();

        if (shouldGiveUpReconnection())
        {
            lock.unlock();
            stop();
            return;
        }

        if (decoder_->releaseOnGrabFailure())
        {
            decoder_->release();
        }
        lock.unlock();

        // 抓取失败，指数退避后重试
        int delay_ms = calculateReconnectDelayMs();
        spdlog::debug("Retry grab after {} ms", delay_ms);
        scheduleNext(delay_ms);
        return;
    }
    auto grab_end = std::chrono::steady_clock::now();
    profileAdd(prof_grab_us_, grab_start);

    // 成功拿到第一帧后，标记首帧已到达
    bool expected = false;
    first_frame_seen_.compare_exchange_strong(expected, true);

    connected_ = true;
    status_ = StreamStatus::CONNECTED;
    consecutive_failures_ = 0;
    last_grab_end_ = grab_end;

    // 记录帧到达服务端的时间戳（在 grab 成功时立即记录，比编码完成时间更准确）
    auto sys_now = std::chrono::system_clock::now();
    last_grab_timestamp_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        sys_now.time_since_epoch()).count();

    // 媒体时间轴 vs 墙钟：漂移持续变大 = 服务端已经落后于源（接收缓冲在堆积）
    {
        const int64_t pts_ms = decoder_->lastFramePtsMs();
        if (pts_ms != 0)
        {
            if (media_pts_base_ms_ == 0)
            {
                media_pts_base_ms_ = pts_ms;
                media_wall_base_ms_ = last_grab_timestamp_ms_;
            }
            media_drift_ms_.store((last_grab_timestamp_ms_ - media_wall_base_ms_) - (pts_ms - media_pts_base_ms_),
                                  std::memory_order_relaxed);
        }
    }

    // 花屏检测：读取解码器自报的错误帧计数，维护 1 秒窗口占比
    updateGlitchStats();

    // 3. 抽帧逻辑判断 (Frame dropping)
    //
    // 注意：判断基准是“帧到达时刻”(grab 完成)，不是“上一帧发布完成时刻”。
    // 之前用 last_encode_time_（编码+发布之后才赋值）作基准，会把编码耗时和
    // 线程池排队时间算进间隔里，导致源帧间隔 50ms、decode_interval=50ms 时
    // 每次判定都差几毫秒而白丢一整帧（丢一帧要等下一个源帧，实际出帧率直接
    // 掉到 ~8.5 FPS 而不是 20 FPS）。改为按固定间隔累加的截止时间 + 少量容差。
    bool should_process = true;
    int64_t decode_interval_us = decode_interval_ms_ * 1000LL;

    if (decode_interval_us > 0)
    {
        auto now = std::chrono::steady_clock::now();
        // 容差：源帧间隔与目标间隔接近时，吸收抓帧抖动/线程调度抖动，
        // 避免“差 1ms 就丢一帧”；取目标间隔的 1/4，保证不会超发。
        auto tolerance = std::chrono::microseconds(std::max<int64_t>(1000, decode_interval_us / 4));

        if (now + tolerance < next_process_deadline_)
        {
            should_process = false;
        }
        else
        {
            // 累加而非 now + interval，保持与源帧率长期锁相
            next_process_deadline_ += std::chrono::microseconds(decode_interval_us);
            // 已经落后（源帧率低于目标间隔，或长时间调度抖动）时重新对齐，
            // 否则会在恢复时连续补帧
            if (next_process_deadline_ <= now)
            {
                next_process_deadline_ = now + std::chrono::microseconds(decode_interval_us);
            }
        }
    }

    spdlog::debug("[stepIO] should_process={}, interval_ms={}, gpu_id={}, use_shm={}",
                   should_process, decode_interval_ms_, gpu_id_, use_shared_mem_);

    if (should_process)
    {
        // 投递任务前再检查一次 running_，避免 stop() 已经调用后还往线程池塞任务。
        if (!running_.load(std::memory_order_acquire))
        {
            lock.unlock();
            spdlog::debug("[stepIO] Task stopped before enqueue, skipping compute");
            return;
        }

        // 同一路流同一时刻只允许一个计算任务在跑：
        //   1) reusable_frame_ 的安全性：retrieve() 会覆写它，而上一个任务的编码
        //      （转 BGR → JPEG）可能还在读它；
        //   2) 实时性：一旦计算（尤其 JPEG 编码）跟不上源帧率，宁可丢帧也不能让 IO
        //      线程排队等待——否则 RTSP 接收缓冲会持续堆积，表现为“播着播着延迟越来越高”。
        //      丢帧只降低帧率，不会累积延迟。
        bool expected = false;
        if (!compute_scheduled_.compare_exchange_strong(expected, true))
        {
            prof_drop_busy_.fetch_add(1, std::memory_order_relaxed);
            cv::Mat dummy;
            decoder_->retrieve(dummy, false);
            lock.unlock();
            scheduleNext(1);
            return;
        }

        // 需要解码：释放锁，将任务派发给计算线程池
        lock.unlock();
        spdlog::debug("[stepIO] Enqueueing stepCompute to pool gpu_id={}", gpu_id_);
        std::weak_ptr<StreamTask> weak_self = shared_from_this();
        TaskScheduler::instance().getComputePool(gpu_id_).enqueue(
            [weak_self]()
            {
                auto self = weak_self.lock();
                if (!self)
                {
                    // 任务入队后、执行前 StreamTask 已被正常释放（如 Ctrl+C 停止流），
                    // 这是预期行为，用 debug 级别避免停止时刷屏。
                    spdlog::debug("[stepCompute] weak_ptr expired, task destroyed before compute");
                    return;
                }

                // 无论从下面哪条路径返回，都必须清掉“已投递”标志，否则该流会永久丢帧
                struct ScheduledGuard
                {
                    std::atomic<bool> &flag;
                    ~ScheduledGuard() { flag.store(false, std::memory_order_release); }
                } scheduled_guard{self->compute_scheduled_};

                // 任务开始执行时 task 仍在，但可能已经被 stop() 标记为 not running。
                // 直接返回，避免在关闭期间做无意义的编码/写 SHM。
                if (!self->running_.load(std::memory_order_acquire))
                {
                    spdlog::debug("[stepCompute] Task already stopped, skipping compute");
                    return;
                }

                // 任务真正开始执行时才增加计数，确保 stop()/析构只等待实际在运行的计算任务
                self->compute_in_flight_++;
                try
                {
                    spdlog::debug("[stepCompute] Executing for {}", self->url_);
                    self->stepCompute();
                }
                catch (const std::exception &e)
                {
                    spdlog::error("[stepCompute] Exception for {}: {}", self->url_, e.what());
                }
                catch (...)
                {
                    spdlog::error("[stepCompute] Unknown exception for {}", self->url_);
                }

                // 计数器递减，确保 stop()/析构不会无限等待
                self->compute_in_flight_--;
                self->compute_done_cv_.notify_all();
            });
    }
    else
    {
        prof_drop_interval_.fetch_add(1, std::memory_order_relaxed);
        // 不需要解码（抽帧丢弃）：仅仅把刚才 grab 的数据从内部缓冲区清空
        cv::Mat dummy;
        decoder_->retrieve(dummy, false); // false 可能代表 fast/dummy retrieve

        lock.unlock();
        // 丢弃完毕，立即准备抓取下一帧。
        // 因为 grab() 是网络阻塞的，所以传 0 也不会导致 CPU 100% 空转。
        scheduleNext(1);
    }
}

// 按需创建 / 扩容共享内存通道。
//
// - 创建：首次解码成功后才调，此时才知道真实帧尺寸（每路流可不一样）；
// - 扩容：后续帧比当前容量大时（UpdateStream 切到更高分辨率、解码器换流等）重建。
//
// 扩容采用“unlink 旧对象 + 创建新对象”的方式（新 inode），因此已经映射了旧对象的
// 客户端会一直读到冻结的最后一帧，必须由客户端自行检测（文件 dev/ino/大小变化）并重连：
//   - Python: client/remote_capture.py::_ShmReader._maybe_reconnect()
//   - C++   : tools/save_frames.cpp::ShmReader::refreshIfStale()
void StreamTask::ensureShmChannel(size_t frame_bytes, int width, int height, int channels)
{
    if (shm_channel_ && frame_bytes <= shm_channel_->maxFrameBytes())
    {
        return; // 现有容量够用
    }

    // 预留 25% 余量，尽量避免分辨率小幅波动就重建（重建会打断已连接的客户端）
    const size_t capacity = frame_bytes + frame_bytes / 4;

    if (shm_channel_)
    {
        spdlog::warn("[StreamTask] Frame {} bytes exceeds SHM capacity {} for stream {}, rebuilding SHM ({} bytes/slot)",
                     frame_bytes, shm_channel_->maxFrameBytes(), stream_id_, capacity);
        shm_channel_->cleanup(); // unlink 旧对象：已连接的客户端需要重新连接
        shm_channel_.reset();
    }

    try
    {
        shm_channel_ = std::make_unique<ZeroCopyChannel>(stream_id_, 0, capacity);
        spdlog::info("SharedMemory ready for stream: {} ({}x{}x{}, fmt={}, {} bytes/frame, capacity {} bytes/slot)",
                     stream_id_, width, height, channels, pixelFormatName(pixel_format_), frame_bytes, capacity);
    }
    catch (const std::exception &e)
    {
        shm_channel_.reset();
        spdlog::error("Failed to init SHM for stream {}: {}", stream_id_, e.what());
    }
}

void StreamTask::stepCompute()
{
    auto compute_start = std::chrono::steady_clock::now();
    auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(compute_start - last_grab_end_).count();

    // spdlog::debug("[stepCompute] Entered for {}, running={}, gpu_id={}", url_, running_.load(), gpu_id_);

    if (!running_)
    {
        spdlog::info("Compute step skipped (not running): {}", url_);
        return;
    }
        
#ifdef RTSP_ENABLE_CUDA
    if (gpu_id_ >= 0)
    {
        CUDATools::AutoDevice auto_device_exchange(gpu_id_);
    }
#endif

    std::unique_lock<std::mutex> lock(decoder_mutex_);
    if (!decoder_ || !decoder_->isOpened())
    {
        spdlog::warn("Decoder not ready in compute step, skipping: {}", url_);
        lock.unlock();
        scheduleNext(0);
        return;
    }

    bool frame_ready = false;
    // 本帧是否花屏（解码器自报，逐帧）：随帧一起发布，消费端据此判断“这一帧能不能用”
    bool frame_corrupted = false;

    // 说明：decoder_mutex_ 只用于保护 decoder_ 指针切换（updateUrl/switchDecoder）和
    // 解码器内部状态（含 retrieve 里的 sws 上下文）。编码（JPEG）、写 SHM 都不需要它，
    // 所以下面在这些耗时操作前就释放锁——IO 线程的 grab() 也要这把锁，持锁做重活会把
    // 编码耗时串进取流循环，使取流速度低于源帧率，接收缓冲持续堆积（延迟越来越高）。
    //
    // === 分支 1: 共享内存模式 -> 直接传原始 Mat / 原始 YUV ===
    if (use_shared_mem_)
    {
        auto t_sws = std::chrono::steady_clock::now();
        // retrieveRaw 支持全部像素格式：BGR 与旧行为完全一致，YUV 则优先走解码器原生输出
        // （CpuDecoder 用 sws 直出目标格式；CudaDecoder 让 NVDEC 直出 NV12，跳过色彩核）
        bool retrieved = decoder_->retrieveRaw(reusable_raw_frame_, pixel_format_) && !reusable_raw_frame_.empty();
        frame_corrupted = decoder_->lastFrameCorrupted(); // 拿锁内快照，避免被下一次 grab 覆盖
        // 自测开关（RTSP_FAKE_GLITCH=1）：每 100 个发布帧伪造一次花屏，
        // 用于在没有真实坏流的情况下验证「逐帧标记 → gRPC/SHM → 客户端/Web」全链路
        if (fake_glitch_enabled_ && (++fake_pub_counter_ % 100 == 0))
        {
            frame_corrupted = true;
            fake_glitch_frames_.fetch_add(1, std::memory_order_relaxed);
        }
        profileAdd(prof_sws_us_, t_sws);
        lock.unlock(); // ← 写 SHM（memcpy + 可能的建/扩容）不持锁

        if (retrieved)
        {
            auto t_pub = std::chrono::steady_clock::now();
            const RawFrame &rf = reusable_raw_frame_;
            const size_t frame_bytes = rf.buffer.total() * rf.buffer.elemSize();

            // 首次解码成功后按实际帧大小创建 SHM；后续帧变大（如分辨率切换）时自动扩容重建
            ensureShmChannel(frame_bytes, rf.width, rf.height, rf.buffer.channels());

            const uint32_t flags = frame_corrupted ? SHM_FRAME_FLAG_CORRUPTED : 0u;
            if (shm_channel_ && shm_channel_->write_frame_raw(rf.buffer.data, frame_bytes,
                                                             static_cast<uint64_t>(rf.width),
                                                             static_cast<uint64_t>(rf.height),
                                                             static_cast<uint32_t>(rf.buffer.channels()),
                                                             static_cast<uint32_t>(rf.buffer.depth()),
                                                             rf.step, rf.pixel_format,
                                                             last_grab_timestamp_ms_, flags))
            {
                frame_ready = true;
                prof_frames_.fetch_add(1, std::memory_order_relaxed);
                recordPublishedFrame();
            }
            profileAdd(prof_enc_us_, t_pub);
        }
    }
    else
    {
        auto encode_buffer = frame_pool_->acquire();

        // 优先透传解码器已编码好的帧（如海康 SDK 抓图返回 JPEG），避免二次编解码
        if (decoder_->getEncodedFrame(*encode_buffer))
        {
            frame_ready = true;
            lock.unlock();
        }
        else
        {
            auto encoder = getThreadLocalEncoder(use_gpu_encoder_, gpu_id_, jpeg_quality_);

#ifdef RTSP_ENABLE_CUDA
            if (cuda_stream_)
            {
                auto *nvjpeg_enc = dynamic_cast<NvjpegEncoder *>(encoder.get());
                if (nvjpeg_enc)
                {
                    nvjpeg_enc->setStream(cuda_stream_);
                }
            }
#endif

            if (decoder_->isGpuFrame() && encoder->supportsGpuEncode())
            {
                // GPU 路径：指纹帧（显存）由解码器持有，且 NVJPEG 编码本身很快，
                // 这里保持持锁编码，避免与解码器复用同一块显存缓冲。
                if (decoder_->retrieve(reusable_frame_, true))
                {
                    uint8_t *gpu_ptr = decoder_->getGpuFramePtr();
                    if (gpu_ptr)
                    {
                        frame_ready = encoder->encodeGpu(gpu_ptr, decoder_->getWidth(),
                                                         decoder_->getHeight(), *encode_buffer);
                    }
                }
                lock.unlock();
            }
            else
            {
                auto t_sws = std::chrono::steady_clock::now();
                bool retrieved = decoder_->retrieve(reusable_frame_, true) && !reusable_frame_.empty();
                frame_corrupted = decoder_->lastFrameCorrupted(); // 拿锁内快照
                // 自测开关：每 100 个发布帧伪造一次花屏（与 SHM 分支同一套逻辑）
                if (fake_glitch_enabled_ && (++fake_pub_counter_ % 100 == 0))
                {
                    frame_corrupted = true;
                    fake_glitch_frames_.fetch_add(1, std::memory_order_relaxed);
                }
                profileAdd(prof_sws_us_, t_sws);
                lock.unlock(); // ← 编码不持锁：IO 线程可以立即 grab 下一帧
                if (retrieved)
                {
                    auto t_enc = std::chrono::steady_clock::now();
                    frame_ready = encoder->encode(reusable_frame_, *encode_buffer);
                    profileAdd(prof_enc_us_, t_enc);
                }
            }
        }

        // 编码/透传成功后通知订阅者
        if (frame_ready)
        {
            prof_frames_.fetch_add(1, std::memory_order_relaxed);
            recordPublishedFrame();
            std::shared_ptr<std::string> prev_frame;
            {
                std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
                prev_frame = std::move(latest_encoded_frame_);
                latest_encoded_frame_ = encode_buffer;
                latest_frame_corrupted_ = frame_corrupted;
                last_encode_time_ = std::chrono::steady_clock::now();
                // 使用 grab 时记录的时间戳，而非编码完成时间
                // 这样即使只解码关键帧（间隔几秒）或线程池排队，时间戳仍反映帧到达时刻
                frame_seq_.store(last_grab_timestamp_ms_, std::memory_order_release);
            }
            frame_cv_.notify_all();
        }
    }
    
    // 各分支已在重活前释放锁，这里只做保险
    if (lock.owns_lock())
    {
        lock.unlock();
    }

    profileLogIfDue();

    // 立即调度下一帧（避免硬编码 sleep）
    scheduleNext(0);
}

void StreamTask::updateHeartbeat()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    last_access_time_.store(now);
}

// ==================== 流水线耗时剖析 ====================

void StreamTask::profileAdd(std::atomic<uint64_t> &counter, std::chrono::steady_clock::time_point from)
{
    counter.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - from)
                                                 .count()),
                      std::memory_order_relaxed);
}

void StreamTask::profileLogIfDue()
{
    static const bool enabled = (std::getenv("RTSP_PROFILE") != nullptr);
    if (!enabled)
        return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - prof_last_log_).count();
    if (elapsed_ms < 1000)
        return;

    uint64_t n = prof_frames_.exchange(0, std::memory_order_relaxed);
    uint64_t lock_wait = prof_lock_wait_us_.exchange(0, std::memory_order_relaxed);
    uint64_t grab = prof_grab_us_.exchange(0, std::memory_order_relaxed);
    uint64_t sws = prof_sws_us_.exchange(0, std::memory_order_relaxed);
    uint64_t enc = prof_enc_us_.exchange(0, std::memory_order_relaxed);
    uint64_t drop_busy = prof_drop_busy_.exchange(0, std::memory_order_relaxed);
    uint64_t drop_int = prof_drop_interval_.exchange(0, std::memory_order_relaxed);
    prof_last_log_ = now;

    if (n == 0 && drop_busy == 0 && drop_int == 0)
        return;
    const double d = n ? static_cast<double>(n) : 1.0;
    spdlog::info("[profile] {} 发布 {:5.2f} FPS | 每帧: 等锁 {:.1f}ms 解码 {:.1f}ms 转BGR {:.1f}ms 编码/写SHM {:.1f}ms | 落后源 {:+.0f}ms | 丢帧 busy={} interval={}",
                 stream_id_, n * 1000.0 / elapsed_ms, lock_wait / d / 1000.0, grab / d / 1000.0,
                 sws / d / 1000.0, enc / d / 1000.0, media_drift_ms_.load(std::memory_order_relaxed) / 1000.0,
                 drop_busy, drop_int);
}

// 每次成功发布一帧（写入 SHM 或编码为 JPEG）时调用。
// 用 1 秒滚动窗口计算速率：窗口期满时用 (本次计数 - 窗口起点计数) / 实际时长。
void StreamTask::recordPublishedFrame()
{
    const auto now = std::chrono::steady_clock::now();
    const int64_t now_ns = now.time_since_epoch().count();
    last_publish_ns_.store(now_ns, std::memory_order_relaxed);
    const uint64_t total = publish_count_.fetch_add(1, std::memory_order_relaxed) + 1;

    const int64_t start_ns = publish_window_start_ns_.load(std::memory_order_relaxed);
    if (start_ns == 0)
    {
        // 首个发布点：开窗，等有足够时长再结算
        publish_window_start_ns_.store(now_ns, std::memory_order_relaxed);
        publish_window_count_.store(total, std::memory_order_relaxed);
        return;
    }

    constexpr int64_t WINDOW_NS = 1000000000LL; // 1s
    const int64_t elapsed_ns = now_ns - start_ns;
    if (elapsed_ns >= WINDOW_NS)
    {
        const uint64_t start_count = publish_window_count_.load(std::memory_order_relaxed);
        publish_fps_.store(static_cast<double>(total - start_count) * 1e9 / static_cast<double>(elapsed_ns),
                           std::memory_order_relaxed);
        publish_window_start_ns_.store(now_ns, std::memory_order_relaxed);
        publish_window_count_.store(total, std::memory_order_relaxed);
    }
}

double StreamTask::getPublishFps() const
{
    const int64_t last_ns = last_publish_ns_.load(std::memory_order_relaxed);
    if (last_ns == 0)
        return 0.0;

    const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    // 长时间没有新帧（断流/重新连接中）时不要继续报旧值
    if (now_ns - last_ns > 3000000000LL) // 3s
        return 0.0;

    return publish_fps_.load(std::memory_order_relaxed);
}

// ==================== 花屏（解码质量）判定 ====================
//
// 只使用解码器自报的硬信号，不做像素域猜测：
//   - CPU (FFmpeg): AVFrame::decode_error_flags 里的
//       MISSING_REFERENCE / CONCEALMENT_ACTIVE / INVALID_BITSTREAM
//   - GPU (NVDEC) : cuvidGetDecodeStatus 的 Error / Error_Concealed
// 这些都是“解码器明确知道这一帧坏了”，零误报；像素域那套（灰块率、块效应）
// 在夜间红外切黑白、低照度、纯色场景下会大量误报，只在解码器不报错但画面确有
// 伪影时才有必要作为兜底。
void StreamTask::updateGlitchStats()
{
    if (!decoder_)
        return;

    const auto health = decoder_->getDecodeHealth();

    // “真实 + 自测伪造”的合计计数：窗口增量与上报值都基于它，保证日志与前端一致
    const uint64_t combined = health.corrupted_frames + fake_glitch_frames_.load(std::memory_order_relaxed);

    uint64_t new_errors = 0;
    if (combined >= glitch_last_seen_total_)
    {
        new_errors = combined - glitch_last_seen_total_;
    }
    glitch_last_seen_total_ = combined;

    glitch_total_.store(combined, std::memory_order_relaxed);
    glitch_win_errors_ += new_errors;
    ++glitch_win_frames_;

    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    if (glitch_win_start_ms_ == 0)
    {
        glitch_win_start_ms_ = now_ms;
    }

    // 一次“花屏事件”只告警一条，恢复时再补一条，避免刷屏
    if (new_errors > 0 && !glitch_logged_)
    {
        glitch_logged_ = true;
        spdlog::warn("[glitch] {} 检测到花屏：累计 {} 帧（缺参考帧 {} / 码流非法 {} / 错误掩盖 {}）；"
                     "RTSP 走 TCP 时通常意味着相机侧码流异常或解码器丢参考帧",
                     stream_id_, combined, health.missing_reference, health.invalid_bitstream,
                     health.error_concealed);
    }

    // 每秒结算一次窗口占比
    const int64_t elapsed_ms = now_ms - glitch_win_start_ms_;
    if (elapsed_ms < 1000)
    {
        return;
    }

    const double ratio = glitch_win_frames_ ? static_cast<double>(glitch_win_errors_) / static_cast<double>(glitch_win_frames_) : 0.0;
    glitch_ratio_.store(ratio, std::memory_order_relaxed);

    if (glitch_logged_ && glitch_win_errors_ == 0)
    {
        glitch_logged_ = false;
        spdlog::info("[glitch] {} 花屏已恢复（累计 {} 帧）", stream_id_, combined);
    }

    // 可选恢复手段：持续花屏时重连（现有 FFmpeg 封装没有暴露 RTCP FIR/PLI 请求 IDR 的能力，
    // 重连后服务端会重新出 IDR，是唯一可靠的自愈路径）。默认关闭（RTSP_GLITCH_RECONNECT_S=0），
    // 因为重连本身会中断 SHM 客户端。
    if (glitch_reconnect_s_ > 0 && ratio > 0.3)
    {
        if (glitch_high_since_ms_ == 0)
        {
            glitch_high_since_ms_ = now_ms;
        }
        else if (now_ms - glitch_high_since_ms_ > glitch_reconnect_s_ * 1000LL)
        {
            glitch_high_since_ms_ = 0;
            glitch_logged_ = false;
            spdlog::warn("[glitch] {} 持续花屏（占比 {:.0f}%）超过 {}s，重连以强制获取新 IDR",
                         stream_id_, ratio * 100.0, glitch_reconnect_s_);
            force_reopen_ = true;
        }
    }
    else
    {
        glitch_high_since_ms_ = 0;
    }

    glitch_win_start_ms_ = now_ms;
    glitch_win_frames_ = 0;
    glitch_win_errors_ = 0;
}

int StreamTask::calculateReconnectDelayMs() const
{
    // 指数退避：500ms, 1s, 2s, 4s, 8s, ..., 上限 30s
    if (consecutive_failures_ <= 0)
        return 100;
    int shift = std::min(consecutive_failures_ - 1, 6); // 2^6 = 64
    int delay = std::min(500 * (1 << shift), 30000);

    // 加 ±25% 随机抖动：大量流同时失败时，确定性退避会让它们按完全相同的
    // 节拍重连，而 NVDEC 会话创建在驱动内是串行的，同步重试会反复形成创建风暴
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(0.75, 1.25);
    return static_cast<int>(delay * jitter(rng));
}

void StreamTask::markConnectionFailure()
{
    // 首帧宽限期内不累计连接失败，避免摄像头尚未出帧就被判定为断线。
    if (!first_frame_seen_.load(std::memory_order_acquire) && inFirstFrameGracePeriod())
    {
        spdlog::debug("markConnectionFailure skipped during first-frame grace period: {}", url_);
        return;
    }

    connected_ = false;
    // 失败不代表彻底断开，服务端仍在指数退避重连，因此状态保持 CONNECTING。
    // 只有 shouldGiveUpReconnection() 决定停止时，stop() 才会把状态设为 DISCONNECTED。
    status_ = StreamStatus::CONNECTING;
    consecutive_failures_++;
    if (consecutive_failures_ == 1)
    {
        first_failure_time_ = std::chrono::steady_clock::now();
    }
}

bool StreamTask::shouldGiveUpReconnection()
{
    if (keep_on_failure_)
        return false;
    if (consecutive_failures_ <= 0)
        return false;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - first_failure_time_)
                          .count();
    constexpr int64_t MAX_OFFLINE_MS = 60000; // 60s
    return elapsed_ms > MAX_OFFLINE_MS;
}

bool StreamTask::inFirstFrameGracePeriod() const
{
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - first_grab_attempt_time_)
                          .count();
    return elapsed_ms <= FIRST_FRAME_GRACE_PERIOD_MS;
}

void StreamTask::resetFirstFrameState()
{
    first_grab_attempt_time_ = std::chrono::steady_clock::now() - std::chrono::hours(1);
    first_frame_seen_.store(false, std::memory_order_release);
}

bool StreamTask::getLatestEncodedFrame(std::shared_ptr<std::string> &out_buffer, bool *out_corrupted)
{
    updateHeartbeat();
    std::shared_lock<std::shared_mutex> lock(frame_mutex_);

    if (!latest_encoded_frame_)
    {
        return false;
    }
    out_buffer = latest_encoded_frame_;
    if (out_corrupted)
    {
        *out_corrupted = latest_frame_corrupted_;
    }
    return true;
}

bool StreamTask::waitForNextFrame(std::shared_ptr<std::string> &out_buffer, uint64_t &current_seq, int timeout_ms,
                                  bool *out_corrupted)
{
    updateHeartbeat();
    std::unique_lock<std::shared_mutex> lock(frame_mutex_);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (running_.load() && frame_seq_.load(std::memory_order_acquire) <= current_seq)
    {
        if (frame_cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            break;
        }
    }

    if (!running_.load() || !latest_encoded_frame_)
        return false;

    if (frame_seq_.load(std::memory_order_acquire) > current_seq)
    {
        out_buffer = latest_encoded_frame_;
        current_seq = frame_seq_.load(std::memory_order_acquire);
        if (out_corrupted)
        {
            *out_corrupted = latest_frame_corrupted_;
        }
        return true;
    }
    return false;
}

bool StreamTask::isConnected()
{
    updateHeartbeat();
    return connected_;
}

void StreamTask::keepAlive()
{
    updateHeartbeat();
}

bool StreamTask::isTimeout()
{
    if (heartbeat_timeout_ms_ <= 0)
        return false;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto last_access = last_access_time_.load();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::duration(now - last_access))
                           .count();
    return duration_ms > heartbeat_timeout_ms_;
}

void StreamTask::updateUrl(const std::string &new_url)
{
    // 这里只设置 pending 状态，不执行耗时的 release
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    if (url_ != new_url)
    {
        pending_url_ = new_url;
        url_changed_ = true; // 原子标记 URL 已经改变，等待 stepIO() 循环自然处理
        spdlog::info("StreamTask URL updated: {} -> {}", url_, new_url);
        status_ = StreamStatus::CONNECTING;
        consecutive_failures_ = 0;

        // 清空上一路流的缓存帧，避免新 URL 还未出图时客户端读到旧帧
        std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
        latest_encoded_frame_.reset();
        // 把 frame_seq_ 推进到当前时间戳，确保 waitForNextFrame 在切换后能正常等待新帧
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
        frame_seq_.store(static_cast<uint64_t>(now_ms), std::memory_order_release);
        frame_cv_.notify_all();
    }
}

void StreamTask::switchDecoder(int decoder_type,
                               std::unique_ptr<IVideoDecoder> decoder,
                               const std::string &new_url,
                               bool use_gpu_encoder)
{
    std::lock_guard<std::mutex> lock(decoder_mutex_);
    if (!decoder)
    {
        spdlog::error("switchDecoder received null decoder, ignore");
        return;
    }

    pending_decoder_ = std::move(decoder);
    pending_decoder_type_ = decoder_type;

    // 新解码器同样要在 open() 之前知道期望的原始帧格式，否则 YUV 流切换后会退回 BGR 路径
    if (pending_decoder_)
    {
        pending_decoder_->setOutputPixelFormat(pixel_format_);
    }

#ifndef RTSP_ENABLE_CUDA
    // CUDA 未启用时禁止切换到 GPU 路径
    if (pending_decoder_type_ == streamingservice::DECODER_GPU_NVCUVID)
    {
        spdlog::warn("[switchDecoder] CUDA disabled at build time, requested GPU_NVCUVID falls back to CPU_FFMPEG");
        pending_decoder_type_ = streamingservice::DECODER_CPU_FFMPEG;
    }
    pending_use_gpu_encoder_ = false;
#else
    pending_use_gpu_encoder_ = use_gpu_encoder;
#endif

    pending_url_ = new_url;
    decoder_changed_ = true;
    spdlog::info("StreamTask decoder switch scheduled: {} -> {}, url: {}",
                 decoder_type_, pending_decoder_type_, new_url);
    status_ = StreamStatus::CONNECTING;
    consecutive_failures_ = 0;

    // 清空上一路流的缓存帧，避免新 decoder/URL 还未出图时客户端读到旧帧
    std::unique_lock<std::shared_mutex> frame_lock(frame_mutex_);
    latest_encoded_frame_.reset();
    // 把 frame_seq_ 推进到当前时间戳，确保 waitForNextFrame 在切换后能正常等待新帧
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
    frame_seq_.store(static_cast<uint64_t>(now_ms), std::memory_order_release);
    frame_cv_.notify_all();
}