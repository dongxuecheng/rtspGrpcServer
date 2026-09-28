#pragma once

#include "interfaces.hpp" // 包含 IVideoDecoder, IImageEncoder
#include "frame_memory_pool.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <condition_variable>
#include "zero_copy_channel.hpp"

#ifdef RTSP_ENABLE_CUDA
#include <cuda_runtime.h>
#endif

enum class StreamStatus
{
    CONNECTING,
    CONNECTED,
    DISCONNECTED
};

// 继承 enable_shared_from_this 至关重要，防止任务在线程池排队时对象被析构
class StreamTask : public std::enable_shared_from_this<StreamTask>
{
public:
    StreamTask(const std::string &url,
               const std::string &stream_id,
               int heartbeat_timeout_ms,
               int decode_interval_ms,
               int decoder_type,
               int gpu_id,
               bool keep_on_failure,
               bool use_shared_mem,
               std::unique_ptr<IVideoDecoder> decoder,
               bool use_gpu_encoder,
               int jpeg_quality);

    ~StreamTask();

    // 启动任务循环
    void start();

    // 停止任务
    void stop();

    // 获取最新编码好的帧（线程安全，零拷贝）
    bool getLatestEncodedFrame(std::shared_ptr<std::string> &out_buffer);

    // Getters / Setters
    bool isConnected();
    bool isStopped() const { return stopped_; }
    StreamStatus getStatus() const { return status_; }
    const std::string &getUrl() const { return url_; }
    int getDecoderType() const { return decoder_type_; }
    int getGpuId() const { return gpu_id_; }
    int getSavedDecoderType() const { return saved_decoder_type_; }
    int getSavedGpuId() const { return saved_gpu_id_; }
    void setSavedDecoderType(int type) { saved_decoder_type_ = type; }
    void setSavedGpuId(int gpu_id) { saved_gpu_id_ = gpu_id; }
    int getWidth() const { return decoder_ ? decoder_->getWidth() : 0; }
    int getHeight() const { return decoder_ ? decoder_->getHeight() : 0; }
    int getDecodeIntervalMs() const { return decode_interval_ms_; }
    bool shouldKeepOnFailure() const { return keep_on_failure_; }
    bool usesSharedMemory() const { return use_shared_mem_; }
    int getHeartbeatTimeMs() const { return heartbeat_timeout_ms_; }
    bool onlyKeyFrames() const { return decoder_->onlyKeyFrames(); }

    // 服务端真实出帧率（每秒发布的帧数）。超过 3 秒没有新帧则返回 0（视为停流）。
    double getPublishFps() const;

    // 落后源的时间（毫秒）：媒体时间轴相对墙钟落后的量。持续变大说明接收缓冲在堆积
    // （观感就是“越播越滞后”）。负值/接近 0 表示跟得上源。
    int64_t getMediaLagMs() const { return media_drift_ms_.load(std::memory_order_relaxed); }

    // 花屏（解码质量）统计：
    //   corrupted_frames 累计错误帧数（解码器自报），glitch_ratio 是最近 1 秒窗口
    //   内出现花屏的帧占比。花屏无法通过像素域可靠判断，这里只用解码器的硬信号：
    //     CPU  : AVFrame::decode_error_flags（缺参考帧 / 错误掩盖 / 码流非法）
    //     GPU  : cuvidGetDecodeStatus（Error / Error_Concealed）
    uint64_t getCorruptedFrames() const { return glitch_total_.load(std::memory_order_relaxed); }
    double getGlitchRatio() const { return glitch_ratio_.load(std::memory_order_relaxed); }
    void updateGlitchStats();

    // 条件变量等待下一帧（零拷贝）
    bool waitForNextFrame(std::shared_ptr<std::string> &out_buffer, uint64_t &current_seq, int timeout_ms);

    // 心跳保活
    void keepAlive();
    bool isTimeout();

    void updateUrl(const std::string &new_url);
    void switchDecoder(int decoder_type,
                       std::unique_ptr<IVideoDecoder> decoder,
                       const std::string &new_url,
                       bool use_gpu_encoder);
    int64_t getFrameSequence() const { return static_cast<int64_t>(frame_seq_.load()); }

private:
    void updateHeartbeat();

    // 重连辅助函数
    int calculateReconnectDelayMs() const;
    void markConnectionFailure();
    bool shouldGiveUpReconnection();

    // 首帧宽限期：open 成功后的一段时间内，grab/demux 临时失败不视为连接中断，
    // 避免摄像头刚 PLAY 后第一帧（尤其关键帧）尚未到达就触发重连。
    static constexpr int FIRST_FRAME_GRACE_PERIOD_MS = 30000; // 30 秒
    bool inFirstFrameGracePeriod() const;
    void resetFirstFrameState();

    // --- 异步调度逻辑 ---

    // 调度下一步操作
    void scheduleNext(int force_delay_ms = 0);
    
    // IO 操作，每个流一个线程，负责 grab 和调度计算任务
    void stepIO();
    void ioLoop();

    // 共享内存通道：首次按实际帧大小创建；帧变大（分辨率切换）时自动扩容重建
    void ensureShmChannel(const cv::Mat &frame);

    // 阶段2：计算操作 (Decode / Convert / Encode) -> 运行在 计算线程池
    void stepCompute();

    // 返回 false 表示休眠被 stop() 中断；返回 true 表示休眠正常结束
    bool interruptibleSleep(int ms);

    // --- 成员变量 ---
    std::string url_;
    std::string pending_url_;
    std::atomic<bool> url_changed_{false};
    std::atomic<bool> force_reopen_{false}; // URL 变化但不需要 release 时，强制重新 open

    // 切换解码器相关（用于 UpdateStream 从 RTSP 切到 HIK_SDK 或反向）
    std::unique_ptr<IVideoDecoder> pending_decoder_;
    std::atomic<bool> decoder_changed_{false};
    int pending_decoder_type_ = 0;
    bool pending_use_gpu_encoder_ = false;
    std::string stream_id_;
    int heartbeat_timeout_ms_;
    int decode_interval_ms_;
    int decoder_type_;
    bool keep_on_failure_;
    bool use_shared_mem_;
    int gpu_id_ = -1;

    // 保存启动时指定的解码器类型和 GPU ID，便于 HIK_SDK 切回 RTSP 时恢复
    int saved_decoder_type_ = 0;
    int saved_gpu_id_ = -1;

#ifdef RTSP_ENABLE_CUDA
    cudaStream_t cuda_stream_ = nullptr; // 每路流独立的 CUDA Stream
#endif

    std::unique_ptr<IVideoDecoder> decoder_;
    bool use_gpu_encoder_ = false;
    int jpeg_quality_ = 85;

    std::unique_ptr<ZeroCopyChannel> shm_channel_;

    // 状态控制
    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false}; // 显式停止标志
    std::atomic<StreamStatus> status_{StreamStatus::DISCONNECTED};
    std::atomic<bool> connected_{false};

    // 保护 decoder_ 的互斥锁（防止多线程重入或与 stop 冲突）
    std::mutex decoder_mutex_;

    // 保护 SHM 清理与计算任务之间的竞态：stop() 必须等待所有 stepCompute 完成
    std::atomic<int> compute_in_flight_{0};
    std::mutex compute_done_mutex_;
    std::condition_variable compute_done_cv_;

    // 每路流独立 IO 线程
    std::thread io_thread_;
    std::mutex io_mutex_;
    int next_io_delay_ms_ = 0;
    std::atomic<bool> io_wakeup_{false};

    // 保护最新帧数据的读写锁
    std::shared_mutex frame_mutex_;
    std::condition_variable_any frame_cv_;
    std::shared_ptr<std::string> latest_encoded_frame_;

    // 帧内存池
    std::shared_ptr<FrameMemoryPool> frame_pool_;

    // 是否已有一个计算任务在途（在跑或已投递待跑）。
    // 用于保证同一路流同一时刻只有一个计算任务：既保护 reusable_frame_，
    // 又保证计算跟不上时 IO 线程直接丢帧而不是排队等锁（避免延迟持续堆积）。
    std::atomic<bool> compute_scheduled_{false};

    // ---- 流水线耗时剖析（每帧统计，RTSP_PROFILE=1 时每秒打印一次）----
    // 用于定位“取流落后于源帧率导致延迟堆积”时到底是哪一段慢。
    void profileAdd(std::atomic<uint64_t> &counter, std::chrono::steady_clock::time_point from);
    void profileLogIfDue();
    std::atomic<uint64_t> prof_frames_{0};
    std::atomic<uint64_t> prof_lock_wait_us_{0}; // IO 线程等 decoder_mutex_
    std::atomic<uint64_t> prof_grab_us_{0};      // demux + 解码
    std::atomic<uint64_t> prof_sws_us_{0};       // retrieve（YUV→BGR）
    std::atomic<uint64_t> prof_enc_us_{0};       // JPEG 编码 / 写 SHM
    std::atomic<uint64_t> prof_drop_busy_{0};    // 因计算未完成而丢帧
    std::atomic<uint64_t> prof_drop_interval_{0};// 因抽帧间隔而丢帧
    std::chrono::steady_clock::time_point prof_last_log_ = std::chrono::steady_clock::now();

    // 心跳时间戳
    std::atomic<int64_t> last_access_time_;

    // 内部逻辑变量
    int consecutive_failures_ = 0;
    std::chrono::steady_clock::time_point first_failure_time_;
    std::chrono::steady_clock::time_point last_encode_time_;

    // 抽帧（decode_interval_ms_）的下一次放行时刻。
    // 用“按固定间隔累加”的截止时间而不是“距上次发布是否够久”，
    // 保证与源帧率长期锁相，不会出现“每次都差几毫秒 → 白丢一整帧”。
    std::chrono::steady_clock::time_point next_process_deadline_;

    // 首帧容忍相关
    std::chrono::steady_clock::time_point first_grab_attempt_time_;
    std::atomic<bool> first_frame_seen_{false};

    // 优化：休眠控制相关
    std::mutex sleep_mutex_;
    std::condition_variable sleep_cv_;

    std::atomic<uint64_t> frame_seq_{0};
    uint64_t last_grab_timestamp_ms_ = 0;

    // 媒体时间轴 vs 墙钟的漂移：
    //   漂移 = (墙钟推进量) - (媒体时间轴推进量)，持续变大说明服务端取流落后于源
    //   （接收缓冲在堆积），观感就是“越播越滞后”。
    int64_t media_pts_base_ms_ = 0;
    int64_t media_wall_base_ms_ = 0;
    std::atomic<int64_t> media_drift_ms_{0};

    // ---- 花屏统计（IO 线程写入，gRPC 线程读原子量）----
    std::atomic<uint64_t> glitch_total_{0};  // 累计错误帧数（含自测伪造）
    std::atomic<double> glitch_ratio_{0.0};  // 最近 1 秒窗口内花屏帧占比 0~1
    uint64_t glitch_last_seen_total_ = 0;    // 上次采样时解码器报的计数
    uint64_t glitch_win_errors_ = 0;         // 本窗口新增错误帧数
    uint64_t glitch_win_frames_ = 0;         // 本窗口采样帧数
    int64_t glitch_win_start_ms_ = 0;        // 窗口起点（墙钟 ms）
    bool glitch_logged_ = false;             // 本次花屏是否已告警（避免刷屏）
    int64_t glitch_high_since_ms_ = 0;       // 持续花屏起始时间（用于可选重连）
    int glitch_reconnect_s_ = 0;             // >0：持续花屏该秒数后重连（RTSP_GLITCH_RECONNECT_S）
    uint64_t fake_glitch_counter_ = 0;       // 自测：伪造花屏计数（RTSP_FAKE_GLITCH=1）
    uint64_t fake_glitch_errors_ = 0;

    // 出帧率统计（1 秒滚动窗口，无锁；发布点调用 recordPublishedFrame）
    void recordPublishedFrame();
    std::atomic<uint64_t> publish_count_{0};
    std::atomic<int64_t> publish_window_start_ns_{0};
    std::atomic<uint64_t> publish_window_count_{0};
    std::atomic<double> publish_fps_{0.0};
    std::atomic<int64_t> last_publish_ns_{0};

    // 用于 CPU 路径的图像缓存，避免反复分配 cv::Mat
    cv::Mat reusable_frame_;

    // 性能监控：记录 grab 结束时间，用于计算调度延迟
    std::chrono::steady_clock::time_point last_grab_end_;
};