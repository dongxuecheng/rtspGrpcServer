#pragma once

#include <iostream>
#include <atomic>
#include <string>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <semaphore.h>
#include <spdlog/spdlog.h>
#include <opencv2/opencv.hpp>

// 槽位数量（固定）
constexpr int SHM_SLOT_COUNT = 3;

// 默认单帧最大字节数：FHD BGR ≈ 1920×1080×3 ≈ 6 MB
// 可被 ZeroCopyChannel 构造参数覆盖——再也不需要改代码重新编译来适配不同分辨率了
constexpr size_t DEFAULT_SHM_FRAME_BYTES = 3 * 1920 * 1080;

struct alignas(64) ShmMeta
{
    uint64_t actual_size;    // 实际数据字节数
    uint64_t width;          // 图像宽度
    uint64_t height;         // 图像高度
    uint64_t timestamp;      // 时间戳 (ms)

    // 图像格式描述 ===
    uint32_t channels;       // 通道数: 1=GRAY, 3=BGR, 4=BGRA
    uint32_t depth;          // 位深: CV_8U=0, CV_16U=2, CV_32F=5 等
    uint32_t step;           // 行字节数 (含 padding)，用于非连续内存
    uint32_t reserved;       // 对齐填充
};

// 注意：ShmFrameSlot 只包含元数据，不包含 payload。
// payload 紧跟元数据区之后，slot 总大小 = align_up(SLOT_META_SIZE + max_frame_bytes, 64)
//
// ⚠️ ShmMeta 带 alignas(64)，因此真实布局是：
//     offsetof(ShmFrameSlot, sequence) = 0
//     offsetof(ShmFrameSlot, meta)     = 64   ← 不是 8（sequence 后有 56 字节填充）
//     sizeof(ShmMeta)                  = 64   ← 不是 48
//     payload 偏移（SLOT_META_SIZE）    = 128  ← 不是 56/64
//      sizeof(ShmFrameSlot)            = 128
// 消费端（Python 客户端、tools/*）一律不要硬编码这些偏移，请使用 GetShmLayout 的返回值。
struct alignas(64) ShmFrameSlot
{
    std::atomic<uint64_t> sequence{0};   // 8 bytes, offset 0
    ShmMeta meta;                        // offset 64（alignas(64) 插入填充）
    // sizeof(ShmFrameSlot) = 128 (8 + 56 padding + 64)
};

// 布局自检：上述数值是消费端/文档的约定，改动结构体时必须同步更新（否则编译期报错）
static_assert(offsetof(ShmFrameSlot, sequence) == 0, "sequence 应位于槽位起始处");
static_assert(offsetof(ShmFrameSlot, meta) == 64, "ShmMeta 带 alignas(64)，meta 偏移应为 64");
static_assert(sizeof(ShmMeta) == 64, "sizeof(ShmMeta) 应为 64");
static_assert(sizeof(ShmFrameSlot) == 128, "sizeof(ShmFrameSlot) 应为 128");

// 共享内存布局（运行时计算）：
//   [slot 0 metadata (sizeof(ShmFrameSlot)=128B)] [slot 0 payload (max_frame_bytes)] [padding to 64]
//   [slot 1 metadata (128B)] [slot 1 payload (max_frame_bytes)] [padding to 64]
//   [slot 2 metadata (128B)] [slot 2 payload (max_frame_bytes)] [padding to 64]
//   [head_idx (8B)]
//
// 每个 slot 的 payload 偏移量 = SLOT_META_SIZE = 128
// 每个 slot 的总大小        = align_up(128 + max_frame_bytes, 64)

// C++ 端共享内存布局描述（用于填充 protobuf 返回给 Python 客户端）
struct ShmLayoutInfo
{
    uint64_t slot_count = 0;
    uint64_t max_frame_bytes = 0;
    uint64_t alignment = 0;
    uint64_t slot_size = 0;
    uint64_t seq_offset = 0;
    uint64_t meta_offset = 0;
    uint64_t payload_offset = 0;
    uint64_t meta_data_size = 0;
    uint64_t head_idx_offset = 0;
    uint64_t total_size = 0;
};

// 对齐工具函数
static inline uint64_t align_up_64(uint64_t val, uint64_t alignment)
{
    return (val + alignment - 1) & ~(alignment - 1);
}

// 由 C++ 编译器自动计算实际偏移/大小，避免 Python 端硬编码出错
// 参数 max_frame_bytes：单帧最大字节数，默认 FHD 级别
static inline ShmLayoutInfo getShmLayoutInfo(size_t max_frame_bytes = DEFAULT_SHM_FRAME_BYTES)
{
    ShmLayoutInfo info;
    info.slot_count = SHM_SLOT_COUNT;
    info.max_frame_bytes = max_frame_bytes;
    info.alignment = alignof(ShmFrameSlot);
    info.seq_offset = offsetof(ShmFrameSlot, sequence);
    info.meta_offset = offsetof(ShmFrameSlot, meta);
    // payload 从元数据末尾开始。注意 ShmMeta 带 alignas(64)：
    // offsetof(ShmFrameSlot, meta) = 64、sizeof(ShmMeta) = 64 → SLOT_META_SIZE = 128
    constexpr size_t SLOT_META_SIZE = offsetof(ShmFrameSlot, meta) + sizeof(ShmMeta);
    info.payload_offset = SLOT_META_SIZE;
    info.meta_data_size = sizeof(ShmMeta);

    // 每个 slot 的总大小（元数据 + 最大帧数据，对齐到 64）
    info.slot_size = align_up_64(SLOT_META_SIZE + max_frame_bytes, info.alignment);
    // head_idx 在所有 slot 之后
    info.head_idx_offset = SHM_SLOT_COUNT * info.slot_size;
    // 总大小 = 所有 slot + head_idx（对齐到 8 字节即可）
    info.total_size = align_up_64(info.head_idx_offset + sizeof(uint64_t), sizeof(uint64_t));

    return info;
}

class ZeroCopyChannel
{
public:
    // role: 0=生产者（服务端）, 1=消费者（客户端）
    // max_frame_bytes: 单帧最大字节数，不再写死在编译期
    ZeroCopyChannel(const std::string &stream_id, int role, size_t max_frame_bytes = DEFAULT_SHM_FRAME_BYTES)
        : stream_id_(stream_id), role_(role), max_frame_bytes_(max_frame_bytes),
          shm_path_("/" + stream_id), sem_name_("/" + stream_id + "_notify")
    {
        // 计算运行时布局
        auto info = getShmLayoutInfo(max_frame_bytes_);
        total_size_ = info.total_size;
        slot_size_ = info.slot_size;
        payload_offset_ = info.payload_offset;
        head_idx_offset_ = info.head_idx_offset;

        // 1. 先创建/打开通知信号量，再创建/映射 SHM。
        //    shm_open(O_CREAT) 会让 SHM 文件立刻可见，而客户端一看到该文件就会
        //    sem_open；如果信号量晚于 SHM 出现，客户端会因 ENOENT 退化成轮询模式。
        if (role_ == 0)
        {
            // 临时清除 umask，确保信号量文件权限真正为 0666（跨用户/容器访问）
            auto old_umask = umask(0);
            notify_sem_ = sem_open(sem_name_.c_str(), O_CREAT | O_RDWR, 0666, 0);
            umask(old_umask);
        }
        else
        {
            notify_sem_ = sem_open(sem_name_.c_str(), 0);
        }
        if (notify_sem_ == SEM_FAILED)
        {
            notify_sem_ = nullptr;
            spdlog::warn("[ZeroCopyChannel] Notify semaphore unavailable for {} (errno={}), SHM will work without cross-process notify", stream_id_, errno);
        }
        else
        {
            spdlog::info("[ZeroCopyChannel] Notify semaphore ready: {} (role={})", sem_name_, role_ == 0 ? "producer" : "consumer");
        }

        // 2. 创建/映射共享内存
        if (role_ == 0)
        {
            // 临时清除 umask，确保共享内存文件权限真正为 0666（跨用户/容器访问）
            auto old_umask = umask(0);
            shm_fd_ = shm_open(shm_path_.c_str(), O_CREAT | O_RDWR, 0666);
            umask(old_umask);
            if (shm_fd_ < 0)
            {
                releaseSemaphoreOnFailure();
                throw std::runtime_error("shm_open failed for " + shm_path_ + ": " + std::to_string(errno));
            }
            if (ftruncate(shm_fd_, total_size_) < 0)
            {
                close(shm_fd_);
                shm_fd_ = -1;
                shm_unlink(shm_path_.c_str());
                releaseSemaphoreOnFailure();
                throw std::runtime_error("ftruncate failed for " + shm_path_ + ": " + std::to_string(errno));
            }
            base_ = (uint8_t *)mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        }
        else
        {
            shm_fd_ = shm_open(shm_path_.c_str(), O_RDONLY, 0666);
            if (shm_fd_ < 0)
            {
                releaseSemaphoreOnFailure();
                throw std::runtime_error("shm_open (consumer) failed for " + shm_path_ + ": " + std::to_string(errno));
            }
            base_ = (uint8_t *)mmap(nullptr, total_size_, PROT_READ, MAP_SHARED, shm_fd_, 0);
        }

        if (base_ == MAP_FAILED || base_ == nullptr)
        {
            if (shm_fd_ >= 0)
            {
                close(shm_fd_);
                shm_fd_ = -1;
            }
            if (role_ == 0)
            {
                shm_unlink(shm_path_.c_str());
            }
            releaseSemaphoreOnFailure();
            throw std::runtime_error("mmap failed for " + shm_path_ + ": " + std::to_string(errno));
        }

        // 初始化共享内存（生产者负责清零，避免消费者读到脏数据）
        if (role_ == 0)
        {
            std::memset(base_, 0, total_size_);
        }
    }

    ~ZeroCopyChannel()
    {
        cleanup();
    }

    // 单帧最大字节数（slot payload 容量），供写入方判断是否需要扩容
    size_t maxFrameBytes() const noexcept { return max_frame_bytes_; }

    // 禁止拷贝和移动，防止 double-close / double-munmap
    ZeroCopyChannel(const ZeroCopyChannel&) = delete;
    ZeroCopyChannel& operator=(const ZeroCopyChannel&) = delete;
    ZeroCopyChannel(ZeroCopyChannel&&) = delete;
    ZeroCopyChannel& operator=(ZeroCopyChannel&&) = delete;

    bool write_frame_mat(const cv::Mat& frame, uint64_t timestamp)
    {
        std::unique_lock<std::mutex> lock(cleanup_mutex_);
        if (cleaned_.load() || !base_ || frame.empty())
        {
            return false;
        }

        // 1. 确保数据连续
        cv::Mat continuous_frame = frame;
        if (!frame.isContinuous())
        {
            continuous_frame = frame.clone();
        }

        // 2. 计算数据大小
        const size_t data_size = continuous_frame.total() * continuous_frame.elemSize();
        if (data_size > max_frame_bytes_)
        {
            spdlog::warn("[ZeroCopyChannel] Frame size {} exceeds max_frame_bytes {} for stream {}",
                         data_size, max_frame_bytes_, stream_id_);
            return false;
        }

        // 3. 获取槽位指针
        uint64_t count = write_count_++;
        size_t idx = count % SHM_SLOT_COUNT;
        uint8_t *slot_base = base_ + idx * slot_size_;
        ShmFrameSlot *slot = reinterpret_cast<ShmFrameSlot *>(slot_base);

        // 4. 标记开始写入 (sequence 奇数 = 写入中)
        slot->sequence.fetch_add(1, std::memory_order_release);

        // 5. 写入元数据
        slot->meta.actual_size = data_size;
        slot->meta.width = frame.cols;
        slot->meta.height = frame.rows;
        slot->meta.timestamp = timestamp;
        slot->meta.channels = frame.channels();
        slot->meta.depth = frame.depth();
        slot->meta.step = static_cast<uint32_t>(continuous_frame.step[0]);

        // 6. 拷贝帧数据到 payload 区域（slot 元数据之后）
        uint8_t *payload_ptr = slot_base + payload_offset_;
        std::memcpy(payload_ptr, continuous_frame.data, data_size);

        // 7. 标记写入完成 (sequence 偶数 = 就绪)
        slot->sequence.fetch_add(1, std::memory_order_release);

        // 8. 更新全局 head_idx
        std::atomic<uint64_t> *head = reinterpret_cast<std::atomic<uint64_t> *>(base_ + head_idx_offset_);
        head->store(count, std::memory_order_release);

        // 9. 通知等待的客户端
        if (notify_sem_)
        {
            if (sem_post(notify_sem_) != 0)
            {
                spdlog::debug("[ZeroCopyChannel] sem_post failed for {} (errno={})", stream_id_, errno);
            }
        }

        return true;
    }

    // 写入原始数据（无 OpenCV Mat）
    void write_frame(const uint8_t *src_data, uint64_t size, uint64_t w, uint64_t h, uint64_t ts)
    {
        std::unique_lock<std::mutex> lock(cleanup_mutex_);
        if (cleaned_.load() || !base_ || size > max_frame_bytes_)
            return;

        uint64_t count = write_count_++;
        size_t idx = count % SHM_SLOT_COUNT;
        uint8_t *slot_base = base_ + idx * slot_size_;
        ShmFrameSlot *slot = reinterpret_cast<ShmFrameSlot *>(slot_base);

        // 1. 标记开始写入
        slot->sequence.fetch_add(1, std::memory_order_release);

        // 2. 写入元数据
        slot->meta = {};
        slot->meta.actual_size = size;
        slot->meta.width = w;
        slot->meta.height = h;
        slot->meta.timestamp = ts;

        // 3. 拷贝实际数据
        uint8_t *payload_ptr = slot_base + payload_offset_;
        std::memcpy(payload_ptr, src_data, size);

        // 4. 标记写入完成
        slot->sequence.fetch_add(1, std::memory_order_release);

        // 5. 更新索引
        std::atomic<uint64_t> *head = reinterpret_cast<std::atomic<uint64_t> *>(base_ + head_idx_offset_);
        head->store(count, std::memory_order_release);

        // 6. 通知等待的客户端
        if (notify_sem_)
        {
            if (sem_post(notify_sem_) != 0)
            {
                spdlog::debug("[ZeroCopyChannel] sem_post failed for {} (errno={})", stream_id_, errno);
            }
        }
    }

    void cleanup()
    {
        std::lock_guard<std::mutex> lock(cleanup_mutex_);
        if (cleaned_.exchange(true))
        {
            return;
        }

        spdlog::info("[ZeroCopyChannel] Cleaning up SHM: /{} (role={})", stream_id_, role_);

        if (base_)
        {
            if (munmap(base_, total_size_) != 0)
            {
                spdlog::warn("[ZeroCopyChannel] munmap failed for /{}: {} ({})", stream_id_, errno, strerror(errno));
            }
            base_ = nullptr;
        }
        if (shm_fd_ >= 0)
        {
            close(shm_fd_);
            shm_fd_ = -1;
        }
        // 只有生产者才有权限/义务从内核中删除共享内存对象
        if (role_ == 0)
        {
            if (shm_unlink(shm_path_.c_str()) != 0)
            {
                if (errno != ENOENT)
                {
                    spdlog::warn("[ZeroCopyChannel] shm_unlink failed for /{}: {} ({})", stream_id_, errno, strerror(errno));
                }
            }
            else
            {
                spdlog::info("[ZeroCopyChannel] shm_unlink succeeded: /{}", stream_id_);
            }
        }
        // 删除通知信号量
        if (notify_sem_)
        {
            sem_close(notify_sem_);
            if (role_ == 0)
            {
                if (sem_unlink(sem_name_.c_str()) != 0)
                {
                    if (errno != ENOENT)
                    {
                        spdlog::warn("[ZeroCopyChannel] sem_unlink failed for /{}_notify: {} ({})", stream_id_, errno, strerror(errno));
                    }
                }
                else
                {
                    spdlog::info("[ZeroCopyChannel] sem_unlink succeeded: /{}_notify", stream_id_);
                }
            }
            notify_sem_ = nullptr;
        }
    }

private:
    // 构造失败时释放已创建的通知信号量，避免留下孤儿 sem 文件
    void releaseSemaphoreOnFailure()
    {
        if (!notify_sem_)
        {
            return;
        }
        sem_close(notify_sem_);
        notify_sem_ = nullptr;
        if (role_ == 0)
        {
            sem_unlink(sem_name_.c_str());
        }
    }

    std::string stream_id_;
    std::string shm_path_;             // "/<stream_id>"
    std::string sem_name_;             // "/<stream_id>_notify"
    int role_;
    int shm_fd_ = -1;
    uint8_t *base_ = nullptr;          // mmap 基地址（byte 指针便于指针运算）
    uint64_t total_size_ = 0;          // SHM 总字节数
    uint64_t slot_size_ = 0;           // 每个 slot 的总字节数
    uint64_t payload_offset_ = 0;      // payload 在 slot 内的偏移量
    uint64_t head_idx_offset_ = 0;     // head_idx 在 SHM 内的偏移量
    size_t max_frame_bytes_ = DEFAULT_SHM_FRAME_BYTES; // 单帧最大字节数
    uint64_t write_count_ = 0;
    sem_t *notify_sem_ = nullptr;
    std::mutex cleanup_mutex_;
    std::atomic<bool> cleaned_{false};
};
