#pragma once
#include "thread_pool.hpp"
#include <memory>

class TaskScheduler {
public:
    static TaskScheduler& instance() {
        static TaskScheduler instance;
        return instance;
    }

    // IO 线程池：用于网络读取、Demux (建议数量: 核心数 * 2 或更多，因为经常阻塞)
    ThreadPool& getIOPool() { return *io_pool_; }

    // 解码线程池：用于 Decode, Convert, Encode (建议数量: 核心数 + 2)
    ThreadPool& getComputePool() { return *decode_pool_; }

    void init(size_t io_threads, size_t decode_threads) {
        io_pool_ = std::make_unique<ThreadPool>(io_threads);
        decode_pool_ = std::make_unique<ThreadPool>(decode_threads);
    }

private:
    TaskScheduler() = default;
    std::unique_ptr<ThreadPool> io_pool_;
    std::unique_ptr<ThreadPool> decode_pool_;
};