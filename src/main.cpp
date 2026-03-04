#include <iostream>
#include <memory>
#include <filesystem>
#include <thread> // for hardware_concurrency
#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include "rtsp_service.hpp"
#include "task_scheduler.hpp"
#include "timer_scheduler.hpp"
#include <cuda_runtime.h>

// spdlog headers
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

void display_banner() {
    printf("%s\n"," ██████╗ ██████╗ ██████╗  ██████╗    ██████╗ ████████╗███████╗██████╗ ");
    printf("%s\n","██╔════╝ ██╔══██╗██╔══██╗██╔════╝    ██╔══██╗╚══██╔══╝██╔════╝██╔══██╗");
    printf("%s\n","██║  ███╗██████╔╝██████╔╝██║         ██████╔╝   ██║   ███████╗██████╔╝");
    printf("%s\n","██║   ██║██╔══██╗██╔═══╝ ██║         ██╔══██╗   ██║   ╚════██║██╔═══╝ ");
    printf("%s\n","╚██████╔╝██║  ██║██║     ╚██████╗    ██║  ██║   ██║   ███████║██║     ");
    printf("%s\n"," ╚═════╝ ╚═╝  ╚═╝╚═╝      ╚═════╝    ╚═╝  ╚═╝   ╚═╝   ╚══════╝╚═╝     ");
    printf("%s\n","                                 v1.1                                 ");
    printf("%s\n","                                                                      ");
}

// 配置日志系统
void setupLogging() {
    try {
        if (!std::filesystem::exists("logs")) {
            std::filesystem::create_directories("logs");
        }
    } catch (const std::exception &e) {
        std::cerr << "Could not create logs directory: " << e.what() << std::endl;
    }

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/server.log", true);
    
    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("rtsp_logger", sinks.begin(), sinks.end());
    
    // 设置日志级别和格式
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v"); // 添加 [%t] 显示线程ID

    spdlog::set_default_logger(logger);
}

// 初始化 CUDA 设备
void initCudaDevice()
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0)
    {
        spdlog::warn("No CUDA devices found. GPU decoding will involve CPU fallback or fail.");
        return;
    }

    // 设置同步模式为 BlockingSync，避免 CPU 100% 自旋等待
    // 这对于多流高并发场景至关重要，能显著降低 CPU 负载
    err = cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
    if (err != cudaSuccess)
    {
        // 如果设备此前已被其他库隐式初始化，尝试重置
        if (err == cudaErrorSetOnActiveProcess)
        {
            // 注意：reset 可能会影响同一进程中其他 CUDA 上下文，
            // 但在 main 开头调用通常是安全的。
            cudaDeviceReset();
            cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
        }
    }

    // 默认绑定设备 0
    cudaSetDevice(0);
    
    // 打印显卡信息
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    spdlog::info("CUDA initialized: {} (Compute Capability {}.{})", prop.name, prop.major, prop.minor);
    spdlog::info("CUDA Sync Mode: BlockingSync (Low CPU Usage)");
}

int main(int argc, char **argv)
{
    // 1. 先初始化日志，确保后续步骤可以打印日志
    setupLogging();
    display_banner();

    // 2. 解析参数
    std::string server_address("0.0.0.0:50051");
    if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help"))
    {
        std::cout << "Usage: " << argv[0] << " [address:port]\n"
                  << "Default address is 0.0.0.0:50051" << std::endl;
        return 0;
    }
    if (argc > 1)
    {
        server_address = argv[1];
    }

    // 3. 初始化 CUDA (取消了原来的注释)
    // 必须在启动任何解码器之前调用
    // initCudaDevice();

    // 4. 初始化全局线程池 (TaskScheduler)
    // 根据机器硬件动态计算线程数
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) hardware_threads = 4; // 兜底

    // IO 线程池：用于网络阻塞操作 (Demux/Download)，建议 2-4 倍核心数
    size_t io_threads = hardware_threads * 3;
    
    // 计算线程池：用于解码/转码，建议 1-1.5 倍核心数
    // 如果有 GPU，CPU 负载会低一些，但 NVJPEG 拷贝和 OpenCV 操作仍需 CPU
    size_t compute_threads = hardware_threads + 2;

    // 初始化单例
    TaskScheduler::instance().init(io_threads, compute_threads);
    // TaskScheduler::instance().init(2, 2);
    spdlog::info("Global TaskScheduler initialized. IO Threads: {}, Compute Threads: {}", io_threads, compute_threads);
    TimerScheduler::instance().start();

    // 5. 启动 gRPC 服务
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    RTSPServiceImpl service; // Service 构造函数内的 init 会因为这里已经 init 过了而被忽略

    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(20 * 1024 * 1024); // 增大一点缓冲
    builder.SetMaxSendMessageSize(20 * 1024 * 1024);
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    spdlog::info(">>> 🚀 C++ RTSP gRPC Server Listening on {} <<<", server_address);
    
    // 阻塞等待
    server->Wait();

    return 0;
}