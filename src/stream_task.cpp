#include "stream_task.hpp"
#include "task_scheduler.hpp"
#include "timer_scheduler.hpp"
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

StreamTask::StreamTask(const std::string &url,
                       int heartbeat_timeout_ms,
                       int decode_interval_ms,
                       int decoder_type,
                       bool keep_on_failure,
                       std::unique_ptr<IVideoDecoder> decoder,
                       std::shared_ptr<IImageEncoder> encoder)
    : url_(url),
      heartbeat_timeout_ms_(heartbeat_timeout_ms),
      decode_interval_ms_(decode_interval_ms),
      decoder_type_(decoder_type),
      keep_on_failure_(keep_on_failure),
      decoder_(std::move(decoder)),
      encoder_(std::move(encoder))
{
    updateHeartbeat();
    last_encode_time_ = std::chrono::steady_clock::now();
}

StreamTask::~StreamTask()
{
    stop();
}

void StreamTask::start()
{
    // 防止重复启动
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return; 
    }
    
    stopped_ = false;
    spdlog::info("StreamTask started: {}", url_);
    
    // 投递第一个 IO 任务
    // 使用 shared_from_this() 确保对象存活
    auto self = shared_from_this();
    TaskScheduler::instance().getIOPool().enqueue([self](){
        self->stepIO();
    });
}

void StreamTask::stop()
{
    // CAS 操作将 running_ 置为 false
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false))
    {
        spdlog::info("StreamTask stopping: {}", url_);
        stopped_ = true;
        
        // 加锁确保当前没有正在进行的 stepIO/stepCompute 操作 decoder
        std::lock_guard<std::mutex> lock(decoder_mutex_);
        if (decoder_) {
            decoder_->release();
        }
        
        status_ = StreamStatus::DISCONNECTED;
        connected_ = false;
    }
}

// 调度辅助函数：决定是立即执行 IO，还是延迟执行 IO
void StreamTask::scheduleNext(int force_delay_ms)
{
    if (!running_) return;

    auto self = shared_from_this();

    if (force_delay_ms > 0) {
        // 使用定时器调度器，避免创建独立线程
        TimerScheduler::instance().schedule(force_delay_ms, [self]() {
            // 通过 weak_ptr 检查任务是否还存活
            if (auto locked = self) {  // 等价于 self.lock() 检查
                if (locked->running_) {
                    TaskScheduler::instance().getIOPool().enqueue([self]() {
                        if (auto stillAlive = self) {
                            stillAlive->stepIO();
                        }
                    });
                }
            }
        });
    } else {
        // 立即投递到 IO 线程池
        TaskScheduler::instance().getIOPool().enqueue([self](){
            if (auto stillAlive = self) {
                stillAlive->stepIO();
            }
        });
    }
}

// ---------------------------------------------------------
// 阶段 1: IO 线程池中执行
// 负责：重连、av_read_frame (grab)
// ---------------------------------------------------------
void StreamTask::stepIO()
{
    // 1. 基础检查
    if (!running_) return;
    
    std::unique_lock<std::mutex> lock(decoder_mutex_);
    if (!decoder_) return;

    // 2. 检查连接状态 & 重连逻辑
    if (!decoder_->isOpened())
    {
        const int max_reconnect_attempts = 5;
        if (reconnect_attempts_ >= max_reconnect_attempts) {
            if (!keep_on_failure_) {
                spdlog::error("Max reconnect attempts reached. Stopping task: {}", url_);
                // 停止任务
                lock.unlock();
                stop(); 
                return;
            }
            // 如果允许失败保持，重置计数器，继续无限尝试
            reconnect_attempts_ = 0; 
        }

        reconnect_attempts_++;
        status_ = StreamStatus::CONNECTING;
        connected_ = false;

        spdlog::warn("Attempting to open/reconnect {}/{}: {}", reconnect_attempts_, max_reconnect_attempts, url_);
        
        // 尝试打开 (可能会阻塞几秒)
        if (decoder_->open(url_)) {
            spdlog::info("Connected successfully: {}", url_);
            status_ = StreamStatus::CONNECTED;
            reconnect_attempts_ = 0;
            last_encode_time_ = std::chrono::steady_clock::now();
        } else {
            // 打开失败
            lock.unlock();
            // 延迟 1000ms 后再次进入 stepIO 重试
            scheduleNext(1000); 
            return;
        }
    }

    // 3. 抓取数据 (Network IO)
    // grab() 通常内部调用 av_read_frame 或 cuvidParseVideoData
    if (!decoder_->grab())
    {
        spdlog::warn("Frame grab failed: {}", url_);
        connected_ = false;
        decoder_->release(); // 关闭连接，触发下次重连逻辑
        
        lock.unlock();
        // 立即调度，下次循环会进入 !isOpened 分支进行重连
        scheduleNext(0); 
        return;
    }

    connected_ = true;
    reconnect_attempts_ = 0;

    // 4. 帧率控制 (Decide whether to process)
    bool should_process = true;
    int64_t encode_interval_us = decode_interval_ms_ * 1000LL;

    if (encode_interval_us > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_encode_time_).count();
        if (elapsed_us < encode_interval_us) {
            should_process = false;
        }
    }

    if (should_process) {
        // 需要处理：将任务转移到【计算线程池】
        lock.unlock(); // 解锁，让 IO 线程去处理别的任务
        
        auto self = shared_from_this();
        TaskScheduler::instance().getComputePool().enqueue([self](){
            self->stepCompute();
        });
    } else {
        // 不需要处理（丢帧）：仅消耗缓存，不解码
        cv::Mat dummy;
        decoder_->retrieve(dummy, false); // false = 不需要像素数据
        
        lock.unlock();
        // 继续 IO 循环，不延迟 (为了尽快清空缓冲区)
        // 使用 yield 稍微让出 CPU
        // TaskScheduler::instance().getIOPool().enqueue([self=shared_from_this()](){ self->stepIO(); });
        // 或者直接调用 scheduleNext(0)
        scheduleNext(0);
    }
}

// ---------------------------------------------------------
// 阶段 2: 计算线程池中执行
// 负责：解码 (retrieve / copy to host)、转码 (resize / jpeg encode)
// ---------------------------------------------------------
void StreamTask::stepCompute()
{
    if (!running_) return;

    std::unique_lock<std::mutex> lock(decoder_mutex_);
    if (!decoder_ || !decoder_->isOpened()) {
        lock.unlock();
        scheduleNext(0); // 回到 IO 循环
        return;
    }

    std::string temp_encoded_buffer;
    bool encode_ok = false;

    // 1. 获取解码后的帧 (Retrieve)
    // 2. 编码 (Encode)
    
    if (decoder_->isGpuFrame() && encoder_->supportsGpuEncode())
    {
        // === 全 GPU 路径 (Zero Copy) ===
        // retrieve(true) 确保 GPU 上有数据
        cv::Mat dummy;
        if (decoder_->retrieve(dummy, true)) 
        {
            uint8_t* gpu_ptr = decoder_->getGpuFramePtr();
            if (gpu_ptr) {
                // 如果 Encoder 也是 GPU 的，直接传显存指针
                encode_ok = encoder_->encodeGpu(
                    gpu_ptr, 
                    decoder_->getWidth(), 
                    decoder_->getHeight(), 
                    temp_encoded_buffer
                );
            }
        }
    }
    else
    {
        // === CPU 路径 (Memory Copy) ===
        cv::Mat frame;
        // retrieve(true) 会发生 Device -> Host 拷贝 (如果解码器是 GPU 的)
        // 或者 YUV -> BGR 转换 (如果解码器是 CPU 的)
        if (decoder_->retrieve(frame, true) && !frame.empty())
        {
            // JPEG 编码 (CPU)
            encode_ok = encoder_->encode(frame, temp_encoded_buffer);
        }
    }

    lock.unlock(); // 耗时操作结束，释放锁

    // 3. 更新结果
    if (encode_ok)
    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex_);
        latest_encoded_frame_ = std::move(temp_encoded_buffer);
        last_encode_time_ = std::chrono::steady_clock::now();
    }

    // 4. 回到 IO 循环抓取下一帧
    scheduleNext(0);
}

void StreamTask::updateHeartbeat()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    last_access_time_.store(now);
}

bool StreamTask::getLatestEncodedFrame(std::string &out_buffer)
{
    updateHeartbeat();
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (latest_encoded_frame_.empty())
    {
        return false;
    }
    out_buffer = latest_encoded_frame_;
    return true;
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