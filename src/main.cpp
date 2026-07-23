#include <iostream>
#include <memory>
#include <filesystem>
#include <thread> // for hardware_concurrency
#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include "rtsp_service.hpp"
#include "task_scheduler.hpp"
#include "timer_scheduler.hpp"

#ifdef RTSP_ENABLE_CUDA
#include <cuda_runtime.h>
#endif

// spdlog headers
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

extern "C"
{
#include <libavformat/avformat.h>
}

// 全局变量用于信号处理，实现优雅关闭
// 注意：信号处理函数中只设置原子标志，绝不调用任何可能持有锁的函数（如 Shutdown()）
static std::atomic<bool> g_shutdown_requested{false};

extern "C" void signalHandler(int signum)
{
    (void)signum;
    g_shutdown_requested.store(true);
}

// 服务版本号：升版本只需修改这一处，banner 会自动居中显示
constexpr char kServerVersion[] = "1.5.0";

void display_banner()
{
    // ANSI 颜色：亮青主标题、亮白副标题、灰色分隔线、亮黄版本号
    const char *CYAN = "\033[1;36m";
    const char *WHITE = "\033[1;37m";
    const char *GRAY = "\033[90m";
    const char *YELLOW = "\033[1;33m";
    const char *RESET = "\033[0m";

    printf("%s", CYAN);
    printf("%s\n", R"(  ██████╗ ██████╗ ██████╗  ██████╗     ██████╗ ████████╗███████╗██████╗)");
    printf("%s\n", R"( ██╔════╝ ██╔══██╗██╔══██╗██╔════╝     ██╔══██╗╚══██╔══╝██╔════╝██╔══██╗)");
    printf("%s\n", R"( ██║  ███╗██████╔╝██████╔╝██║          ██████╔╝   ██║   ███████╗██████╔╝)");
    printf("%s\n", R"( ██║   ██║██╔══██╗██╔═══╝ ██║          ██╔══██╗   ██║   ╚════██║██╔═══╝)");
    printf("%s\n", R"( ╚██████╔╝██║  ██║██║     ╚██████╗     ██║  ██║   ██║   ███████║██║)");
    printf("%s\n", R"(  ╚═════╝ ╚═╝  ╚═╝╚═╝      ╚═════╝     ╚═╝  ╚═╝   ╚═╝   ╚══════╝╚═╝)");
    printf("%s", RESET);
    printf("\n");
    printf("%s                         RTSP Streaming Server%s\n", WHITE, RESET);
    printf("%s                 ─────────────────────────────────────%s\n", GRAY, RESET);

    // 版本行随 kServerVersion 变化，按 72 列画布自动居中（内容限 ASCII 字符）
    // 特性行随构建类型自适应：GPU 构建含 NVDEC/NVJPEG 加速，CPU-only 构建仅 CPU
#ifdef RTSP_ENABLE_CUDA
    const char *features = "CPU + GPU (NVDEC/NVJPEG)";
#else
    const char *features = "CPU Only";
#endif
    char version_line[128];
    snprintf(version_line, sizeof(version_line), "v%s | %s", kServerVersion, features);
    int pad = (72 - static_cast<int>(strlen(version_line))) / 2;
    if (pad < 0)
        pad = 0;
    printf("%s%*s%s%s\n", YELLOW, pad, "", version_line, RESET);
    printf("\n");
}

// 配置日志系统
void setupLogging()
{
    try
    {
        if (!std::filesystem::exists("logs"))
        {
            std::filesystem::create_directories("logs");
        }
    }
    catch (const std::exception &e)
    {
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

#ifdef RTSP_ENABLE_CUDA
// 【修改】初始化所有 CUDA 设备，而不仅仅是设备 0
void initCudaDevices()
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0)
    {
        spdlog::warn("No CUDA devices found. GPU decoding will fallback to CPU or fail.");
        return;
    }

    // 遍历系统中所有的 GPU 进行初始化
    for (int i = 0; i < device_count; ++i)
    {
        cudaSetDevice(i);

        // 设置同步模式为 BlockingSync，避免 CPU 100% 自旋等待
        // 这对于多流高并发场景至关重要，能显著降低 CPU 负载
        err = cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
        if (err != cudaSuccess)
        {
            if (err == cudaErrorSetOnActiveProcess)
            {
                // 如果设备此前已被隐式初始化，尝试重置并重新设置标志
                spdlog::warn("GPU {} already initialized, resetting to apply BlockingSync...", i);
                cudaDeviceReset();
                cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
            }
        }

        // 打印显卡信息
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, i);
        spdlog::info("CUDA GPU [{}] initialized: {} (Compute Capability {}.{})", i, prop.name, prop.major, prop.minor);
    }

    spdlog::info("CUDA Sync Mode: BlockingSync (Low CPU Usage) applied to all {} GPUs.", device_count);

    // 防御性编程：回到默认设备0
    cudaSetDevice(0);
}
#endif

int main(int argc, char **argv)
{
    // 1. 先初始化日志，确保后续步骤可以打印日志
    setupLogging();
    display_banner();

    avformat_network_init();

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

    // 3. 初始化所有 CUDA 卡（BlockingSync 低 CPU 占用同步模式）
    // 必须在创建任何解码器/线程池之前调用，否则默认的自旋同步
    // 会让上百个 IO/计算线程在 cuStreamSynchronize 上空转烧满 CPU
#ifdef RTSP_ENABLE_CUDA
    initCudaDevices();
#endif

    // 4. 初始化全局线程池 (TaskScheduler)
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0)
        hardware_threads = 4; // 兜底

    // 环境变量辅助解析器
    auto envUInt = [](const char *name, unsigned int def) -> unsigned int
    {
        if (const char *val = std::getenv(name))
        {
            try
            {
                return static_cast<unsigned int>(std::stoul(val));
            }
            catch (...)
            {
            }
        }
        return def;
    };

    // ① GPU 专属计算线程池 (单卡)：因为硬件解码(NVDEC)和编码(nvJPEG)都是扔给硬件做的，
    // C++ 线程只是负责“提交任务+等待”，不需要跟 CPU 核心数绑定。一般 4-8 个线程就能喂饱单张显卡。
    size_t gpu_threads_per_card = envUInt("RTSP_GPU_THREADS_PER_CARD", 6);
    if (gpu_threads_per_card == 0)
        gpu_threads_per_card = 2;
    if (gpu_threads_per_card > 16)
        gpu_threads_per_card = 16;

    // ③ CPU 回退计算线程池：用于 FFmpeg CPU 解码和 OpenCV 图像处理，高度消耗 CPU。
    size_t cpu_compute_threads = envUInt("RTSP_CPU_COMPUTE_THREADS", hardware_threads);
    if (cpu_compute_threads == 0)
        cpu_compute_threads = 2;
    if (cpu_compute_threads > 64)
        cpu_compute_threads = 64;

    TaskScheduler::instance().init(gpu_threads_per_card, cpu_compute_threads);
    TimerScheduler::instance().start();

    // 5. 启动 gRPC 服务
    // grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    auto service = std::make_unique<RTSPServiceImpl>();

    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(20 * 1024 * 1024); // 增大一点缓冲
    builder.SetMaxSendMessageSize(20 * 1024 * 1024);
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(service.get());

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    spdlog::info(">>> 🚀 C++ RTSP gRPC Server Listening on {} <<<", server_address);

    // 注册信号处理函数，捕获 Ctrl+C (SIGINT) 和 kill (SIGTERM)
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // 在后台线程阻塞等待 gRPC 连接，主线程轮询检查关闭标志
    // 这样 Shutdown() 可以在主线程安全调用，避免信号处理函数中死锁
    std::thread wait_thread([&server]() {
        server->Wait();
    });

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spdlog::info("Interrupt signal received. Shutting down gracefully...");
    server->Shutdown();
    wait_thread.join();

    // 显式释放所有流资源，确保 CUDA decoder/encoder 在 CUDA runtime 卸载前被销毁
    service.reset();

    // 显式停止后台线程池，确保 thread_local 的 NvjpegEncoder 在 CUDA runtime 卸载前被销毁
    TimerScheduler::instance().stop();
    TaskScheduler::instance().shutdown();

    spdlog::info("Server shutdown complete. Cleaning up resources...");

    return 0;
}