# AGENTS.md — RTSP gRPC 服务器

> 本文档面向 AI 编程助手。读者被假设对该项目完全不了解。

---

## 项目概述

本项目是一个 **C++ 实现的 RTSP 流媒体服务器**，通过 gRPC 接口向客户端提供视频流服务。核心能力包括：

- **GPU 硬件加速**：支持 NVIDIA CUDA 硬件解码（NVCUVID）和 NVJPEG 编码
- **多解码器支持**：CPU (FFmpeg) / GPU (NVCUVID) 可选
- **流式传输**：gRPC 服务端流式推送，低延迟实时传输
- **多客户端共享**：单路解码，多客户端零拷贝共享
- **灵活帧率控制**：支持解码间隔和客户端独立帧率限制
- **流状态管理**：连接中 / 已连接 / 无法连接 / 不存在
- **共享内存传输**：同一台机器上的客户端可通过 POSIX SHM 零拷贝获取原始帧数据

---

## 技术栈

| 层级 | 技术 |
|------|------|
| **服务端语言** | C++17, CUDA C++ |
| **构建系统** | CMake 3.18+ |
| **RPC** | gRPC (C++ 服务端, Python 客户端) |
| **序列化** | Protocol Buffers |
| **视频解码** | FFmpeg (libavcodec) + NVIDIA NVCUVID (硬件解码) |
| **视频解封装** | FFmpeg (libavformat) |
| **图像编码** | OpenCV CPU (`cv::imencode`) + NVJPEG GPU |
| **GPU 计算** | CUDA Toolkit 12.x, 自定义 CUDA Kernel (`color.cu`) |
| **内存管理** | jemalloc (服务端), POSIX Shared Memory (零拷贝通道) |
| **日志** | spdlog (服务端), simple-logger (遗留解码库) |
| **线程** | 自定义 ThreadPool, 每路流独立 IO 线程, TaskScheduler |
| **容器化** | Docker + Docker Compose, NVIDIA Container Runtime |
| **客户端** | Python 3, grpcio, OpenCV, numpy, turbojpeg |
| **网关** | Envoy (gRPC-JSON 转码 / OpenAPI) |

---

## 项目结构

```
rtspGrpcServer/
├── .git/                  # Git 仓库
├── .qoder/                # Qoder IDE 配置
├── build/                 # CMake 构建输出（二进制、生成的 protobuf 文件）
├── client/                # Python 客户端库和示例
│   ├── remote_capture.py  # 统一客户端（RTSPClient），同时支持 gRPC JPEG 与共享内存
│   ├── example.py         # 综合示例：含轮询、流式推送、SHM、并发、性能对比、批量操作等
│   ├── stream_service.proto   # 客户端侧 proto 副本
│   └── stream_service_pb2.py / stream_service_pb2_grpc.py  # 生成代码
├── cpu_decoder_py/        # 空目录（预留）
├── envoy-gateway/         # Envoy 代理 + OpenAPI/gRPC 网关配置
│   ├── readme.md          # REST API 文档
│   ├── proto/             # 网关 proto 文件
│   ├── docker-compose.yml # 网关服务编排
│   └── envoy.yaml         # Envoy 配置文件
├── include/               # C++ 头文件
│   ├── interfaces.hpp         # 核心抽象：IVideoDecoder, IImageEncoder
│   ├── rtsp_service.hpp       # gRPC 服务实现（RTSPServiceImpl）
│   ├── stream_task.hpp        # 每路流任务管理（IO/计算分离）
│   ├── decoder_factory.hpp    # CPU/GPU/海康 解码器工厂
│   ├── cpu_decoder.hpp        # FFmpeg 软解封装
│   ├── cuda_decoder.hpp       # NVIDIA GPU 解码封装
│   ├── cuvid_decoder.hpp      # 底层 NVCUVID 接口
│   ├── hik.hpp                # 海康 SDK 封装（HikSDKManager / HikSDKCap）
│   ├── hik_decoder.hpp        # 海康 SDK 解码器（IVideoDecoder 适配）
│   ├── hik_url_parser.hpp     # 海康 URL 解析（无 SDK 依赖）
│   ├── ffmpeg_decoder.hpp     # FFmpeg AVCodec 抽象
│   ├── ffmpeg_demuxer.hpp     # FFmpeg AVFormatContext 抽象
│   ├── nvjpeg_encoder.hpp     # GPU JPEG 编码器
│   ├── opencv_encoder.hpp     # CPU JPEG 编码器
│   ├── cuda_tools.hpp         # CUDA 错误检查宏、AutoDevice RAII
│   ├── frame_memory_pool.hpp  # std::string 帧缓冲区对象池
│   ├── zero_copy_channel.hpp  # POSIX 共享内存通道（生产者/消费者）
│   ├── task_scheduler.hpp     # 单例调度器：每 GPU 懒加载 ThreadPool + CPU 池
│   ├── thread_pool.hpp        # 经典 std::thread + queue 线程池
│   ├── timer_scheduler.hpp    # 延迟任务调度器
│   ├── nalu.hpp               # H.264 NAL 单元解析
│   ├── utils.hpp              # generate_uuid() 辅助函数
│   └── simple-logger.hpp      # 遗留 C 风格日志宏
├── nvcuvid/               # NVIDIA Video Codec SDK 头文件
│   ├── cuviddec.h
│   ├── nvEncodeAPI.h
│   └── nvcuvid.h
├── src/                   # C++ 源文件 + CUDA Kernel
│   ├── main.cpp               # 入口：banner、spdlog 初始化、CUDA 初始化、gRPC 启动
│   ├── rtsp_service.cpp       # gRPC RPC 实现、流生命周期管理、清理循环
│   ├── stream_task.cpp        # 核心流循环：IO 线程(grab) → 计算池(decode/encode)
│   ├── decoder_factory.cpp    # 工厂分发
│   ├── cpu_decoder.cpp        # CPU 路径：demux → decode → sws_scale 到 BGR
│   ├── cuda_decoder.cpp       # GPU 路径：demux → CUVIDDecoder::decode → get_frame
│   ├── cuvid_decoder.cpp      # 完整 NVCUVID 实现
│   ├── hik.cpp                # 海康 SDK 登录/通道/抓图实现
│   ├── hik_decoder.cpp        # 海康 SDK 抓图 → cv::Mat 适配
│   ├── hik_url_parser.cpp     # 海康 URL 解析实现
│   ├── ffmpeg_decoder.cpp     # FFmpeg decode/send/receive + sws_scale
│   ├── ffmpeg_demuxer.cpp     # FFmpeg demux + RTSP 探测 + 关键帧过滤
│   ├── nvjpeg_encoder.cpp     # NVJPEG 编码
│   ├── opencv_encoder.cpp     # OpenCV cv::imencode
│   ├── cuda_tools.cpp         # CUDA 错误处理、AutoDevice 实现
│   ├── color.cu               # CUDA Kernel：NV12 → BGR 色彩转换
│   ├── simple-logger.cpp      # SimpleLogger 实现
│   └── timer_scheduler.cpp    # TimerScheduler 实现
├── CMakeLists.txt         # CMake 构建配置
├── Dockerfile             # GPU 运行时镜像
├── Dockerfile.cpu         # CPU-only 单阶段运行镜像（拷贝预编译二进制）
├── docker-compose.yml     # GPU 部署编排
├── docker-compose.cpu.yml # CPU-only 部署编排
├── entrypoint.sh          # 容器入口（nvcuvid 软链接、jemalloc 预加载）
├── stream_service.proto   # 主 gRPC/Protobuf 服务定义
├── web/                   # Web 控制台（FastAPI BFF + 原生 JS 页面）
│   ├── server.py          # REST + MJPEG 后端：查看流/预览截图/创建停止任务
│   ├── static/            # index.html / app.js / style.css（无构建步骤）
│   └── README.md          # 运行方式、接口与环境变量说明
├── README.md              # 详细中文文档（含 API、示例、SHM 用法）
└── 编译.md                 # 逐步构建说明
```

---

## 构建和运行

### 服务端编译（本地）

```bash
mkdir build && cd build
cmake .. && make -j
./rtsp_server [address:port]   # 默认 0.0.0.0:50051
```

### 纯 CPU 编译（禁用 CUDA）

通过 `-DENABLE_CUDA=OFF` 可在编译期完全排除 NVCUVID/NVJPEG 相关代码，不依赖 CUDA Toolkit。此时 `DECODER_GPU_NVCUVID` 会自动回退到 CPU 解码。

```bash
mkdir build-cpu && cd build-cpu
cmake -DENABLE_CUDA=OFF ..
make -j
./rtsp_server [address:port]
```

### 依赖项（开发环境）

- C++17 编译器、CMake 3.18+
- CUDA Toolkit 12.x（GPU 解码需要）
- OpenCV 4.x、Protobuf、gRPC、spdlog
- FFmpeg (libavcodec, libavformat, libavutil, libswscale)
- jemalloc
- PkgConfig

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `RTSP_GPU_THREADS_PER_CARD` | 6 | 每 GPU 计算线程数（上限 16） |
| `RTSP_CPU_COMPUTE_THREADS` | 硬件并发数 | CPU 计算线程数（上限 64） |

### Docker 启动（GPU）

```bash
# 如需项目级 SHM 隔离，先创建命名空间目录（默认 grpc_rtsp）
mkdir -p /dev/shm/${SHM_NAMESPACE:-grpc_rtsp}

docker run -itd \
  --gpus '"device=1"' \
  -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,video \
  -e SHM_NAMESPACE=${SHM_NAMESPACE:-grpc_rtsp} \
  --name grpc_rtsp_server \
  -p 50051:50051 \
  -v /dev/shm/${SHM_NAMESPACE:-grpc_rtsp}:/dev/shm \
  grpc_rtsp_server
```

> **关键参数**：挂载 `/dev/shm/<SHM_NAMESPACE>:/dev/shm` 必须开启，否则 Python 客户端无法访问 C++ 端创建的 POSIX 共享内存（`/dev/shm/rtsp_grpc_<16位十六进制>`）。由于使用 bind mount，容器内 `/dev/shm` 实际容量由宿主机 `/dev/shm` 决定；如需调整请在宿主机执行 `mount -o remount,size=32G /dev/shm`。

### Python Proto 生成

```bash
cd client
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. stream_service.proto
```

---

## 代码组织与架构

### IO / 计算分离

每路 `StreamTask` 拥有：
- **一个独立 IO 线程**（`ioLoop`/`stepIO`）：调用 `decoder_->grab()`（网络阻塞），然后调度计算任务
- **计算线程池**（`TaskScheduler::getComputePool(gpu_id)`）：执行 `stepCompute()` — `retrieve()` + `encode()` + 发布帧

### 双编码路径

| 模式 | 数据流 | 用途 |
|------|--------|------|
| **gRPC Streaming** | Raw frame → JPEG (NVJPEG/OpenCV) → `std::string` → gRPC `FrameResponse` | 远程客户端 |
| **Shared Memory** | Raw frame (`cv::Mat`) → `memcpy` to SHM slot | 同机零拷贝 |

### 内存管理

- `FrameMemoryPool`：为每路流缓存最多 3 个 JPEG 输出缓冲区
- `ZeroCopyChannel`：8 槽位 POSIX SHM 环形缓冲区，序列号锁机制（奇数=写入中，偶数=就绪）
- jemalloc 配合显式 `arena.0.purge` 防止内存膨胀

### gRPC 服务定义（`stream_service.proto`）

服务 `RTSPStreamService` 提供 7 个 RPC：
- `StartStream` / `StopStream` — 启动/停止流
- `GetLatestFrame` — 轮询最新 JPEG 帧
- `StreamFrames` — 服务端流式推送（带 FPS 限制）
- `CheckStream` / `ListStreams` — 查询流状态
- `UpdateStream` — 动态修改 RTSP URL

---

## 代码风格指南

### C++ 风格（从现有代码观察）

- **命名**：
  - 类名：`PascalCase`（如 `StreamTask`, `RTSPServiceImpl`）
  - 类成员函数：`camelCase`（如 `getLatestEncodedFrame`, `isOpened`）
  - 私有成员变量：`snake_case_` 后缀下划线（如 `heartbeat_timeout_ms_`, `running_`）
  - 局部变量：`snake_case`
  - 宏/常量：`UPPER_SNAKE_CASE` 或 `PascalCase`
- **接口类**：以 `I` 为前缀（如 `IVideoDecoder`, `IImageEncoder`）
- **智能指针**：广泛使用 `std::shared_ptr` / `std::unique_ptr`
- **原子变量**：使用 `{}` 初始化（如 `std::atomic<bool> running_{false}`）
- **注释**：关键逻辑使用中文注释，代码中也存在英文注释，混合使用
- **头文件**：使用 `#pragma once`，不使用 include guard
- **CUDA**：`color.cu` 使用 `.cu` 扩展名，CUDA 代码与 C++ 代码分离编译（`CUDA_SEPARABLE_COMPILATION ON`）

### Python 风格（客户端）

- 遵循 PEP 8
- 使用类型注解（`typing` 模块）
- 类名 `PascalCase`，函数/变量 `snake_case`
- 常量定义在模块顶部
- 使用上下文管理器（`__enter__` / `__exit__`）管理连接生命周期

---

## 测试说明

### Python 客户端测试（`client/example.py`）

项目没有 C++ 单元测试框架，测试与示例主要通过 `client/example.py` 进行：

1. **串行反复开关流测试**（`example_poll_frame` / `example_stream_frames`）
2. **并发压力测试**（`example_multi_thread`）
3. **gRPC JPEG vs SHM 性能对比**（`example_benchmark`）
4. **批量操作与断线重连**（`example_batch_operations` / `example_reconnect`）

运行方式：
```bash
cd client
python example.py [编号]      # 编号 1-10，或 all 顺序运行全部
```

### 手动验证

- `client/example.py` 包含 10 个独立示例，覆盖 gRPC JPEG、SHM、多线程、性能对比、批量操作、断线重连等场景

---

## 部署流程

### GPU 部署（生产环境）

1. 使用 `Dockerfile` 构建运行时镜像（基于 `nvcr.io/nvidia/cuda:12.6.3-runtime-ubuntu22.04`）
2. `entrypoint.sh` 在运行时创建 `libnvcuvid.so` 软链接（驱动库通过 `--gpus` 挂载）
3. 预加载 jemalloc：`LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2`
4. `docker-compose.yml` 配置 GPU 设备预留、IPC host、共享内存大小

### 共享内存清理与命名

- **命名前缀**：为便于识别和清理，所有共享内存对象（及通知信号量）名称统一带 `rtsp_grpc_` 前缀，例如 `/dev/shm/rtsp_grpc_a1b2c3d4...`、`/dev/shm/sem.rtsp_grpc_a1b2c3d4..._notify`。
- **项目级 SHM 隔离**：`docker-compose.yml` 支持通过环境变量 `SHM_NAMESPACE` 将容器内 `/dev/shm` 映射到宿主机的 `/dev/shm/<SHM_NAMESPACE>/`，避免同一宿主机多个项目之间的 `stream_id` 冲突。容器内代码仍通过 `/dev/shm/rtsp_grpc_*` 创建/访问共享内存文件，因此清理逻辑无需修改。
- **启动兜底清理**：
  - `entrypoint.sh` 在启动服务前会执行 `rm -f /dev/shm/rtsp_grpc_* /dev/shm/sem.rtsp_grpc_*_notify`，清理上一次运行遗留的对象（当启用 `SHM_NAMESPACE` 时，容器内 `/dev/shm` 已对应宿主机子目录，因此只会清理该命名空间下的对象）。
  - `RTSPServiceImpl` 构造函数也会扫描 `/dev/shm` 并 `shm_unlink` 匹配的孤儿对象，作为非 Docker / 直接启动时的兜底。
- **运行时清理**：流正常停止、心跳超时、`SIGINT/SIGTERM` 退出时都会调用 `ZeroCopyChannel::cleanup()` 进行 `shm_unlink` / `sem_unlink`。
- **竞态保护**：`StreamTask::stop()` 会等待所有已投递的 `stepCompute` 任务完成后再清理 SHM；`ZeroCopyChannel` 内部通过互斥锁和 `cleaned_` 标志避免 `write_frame` 与 `cleanup` 并发。
- **心跳上限**：`heartbeat_timeout_ms` 必须大于 0 且不超过 1 小时（3600000 ms），否则会被修正，避免客户端失联后 SHM 长期不回收。

### CPU-only 部署

- 使用 `Dockerfile.cpu`（单阶段运行镜像，基于 `ubuntu:22.04`，不含 CUDA）
- 需要先在宿主机编译 CPU-only 二进制：`cmake -DENABLE_CUDA=OFF .. && make`
- 将生成的 `rtsp_server` 放到项目根目录
- 使用 `docker-compose.cpu.yml` 一键启动：
  ```bash
  mkdir -p /dev/shm/${SHM_NAMESPACE:-grpc_rtsp}
  cp build-cpu/rtsp_server ./rtsp_server
  SHM_NAMESPACE=grpc_rtsp docker compose -f docker-compose.cpu.yml up -d
  ```
- 此时 `DECODER_GPU_NVCUVID` 会自动回退为 CPU 解码

### Envoy 网关（可选）

`envoy-gateway/` 目录提供 gRPC-JSON 转码配置，可将 gRPC 接口暴露为 REST API：
- `docker-compose.swagger.yml` — 网关服务编排
- `envoy.yaml` — Envoy 配置
- `stream.swagger.json` — OpenAPI/Swagger 定义

---

## 安全注意事项

1. **gRPC 传输未加密**：当前使用 `grpc::InsecureServerCredentials()`，没有 TLS。如需公网暴露，必须增加 TLS 或 mTLS 配置。
2. **共享内存权限**：`ZeroCopyChannel` 创建 SHM 时使用 `0666` 权限（`shm_open(..., O_CREAT | O_RDWR, 0666)`），任何有 `/dev/shm` 访问权限的进程都可读写。
3. **Docker `/dev/shm` 挂载**：使用 `-v /dev/shm/${SHM_NAMESPACE:-grpc_rtsp}:/dev/shm` 替代 `--ipc=host`，只共享 POSIX 共享内存文件目录，不暴露完整的 IPC 命名空间。通过 `SHM_NAMESPACE` 可进一步实现项目级隔离，避免多项目部署时的 `stream_id` 冲突。
4. **RTSP URL 中的凭据**：`StartRequest` 和 `StreamInfo` 中包含明文 RTSP 账号密码，日志和 gRPC 消息中均为明文传输。
5. **nvcuvid 运行时链接**：`libnvcuvid.so` 在构建时可能找不到（CMake 仅警告），但在运行时必须存在，否则 GPU 解码会失败。
6. **jemalloc 预加载**：`entrypoint.sh` 通过 `LD_PRELOAD` 强制加载 jemalloc，可能与某些调试工具不兼容。

---

## 开发提示

- **出帧率与“越播越滞后”**：`StreamInfo.fps` 是服务端真实出帧率，`StreamInfo.media_lag_ms` 是**落后源**的时长（媒体时间轴相对墙钟落后的量）。`media_lag_ms` 持续变大 = RTSP 接收缓冲在堆积，观感就是“播着播着延迟越来越高”；Web 控制台在超过 1s 时会打醒目标签。
  根因通常是**取流速度低于源帧率**：`stepCompute` 曾经在持有 `decoder_mutex_` 的情况下做 sws + JPEG 编码，把编码耗时串进了 IO 线程的 `grab()` 循环（1440p HEVC 软解 ~22ms + 转 BGR ~5ms + JPEG ~25ms > 源帧间隔 50ms）。现在 `stepIO` 用 `compute_scheduled_` 保证同一路流只有一个计算任务在跑，计算未完成时**直接丢帧**（保实时，不排队等锁），编码也不再持锁。
  代价：CPU 软解 1440p + JPEG 时总工作量仍可能超过 50ms，表现为出帧率略低于源（约 15~18 FPS）。SHM 模式不做 JPEG 编码，可以稳定跑满源帧率；要同时兼顾 20 FPS 与 JPEG，需要 GPU 解码（NVCUVID + NVJPEG）或降低编码分辨率。
- **性能剖析**：`RTSP_PROFILE=1` 启动服务端，会每秒打印各阶段平均耗时（等锁 / 解码 / demux 等待 / 转 BGR / JPEG 编码 / 落后源 / 丢帧数），用于定位瓶颈；另有 `RTSP_DEC_THREADS`（默认 4）控制软解线程数，**不要设 0/AUTO**（72 核机器上按核数建线程反而更慢）。
- **花屏判定**：不猜像素，只用**解码器自报的硬信号**（零误报、几乎零开销），通过 `IVideoDecoder::getDecodeHealth()` 暴露：
  - CPU（FFmpeg）：`AVFrame::decode_error_flags` 里的 `MISSING_REFERENCE`（缺参考帧，必然花屏）/ `CONCEALMENT_ACTIVE`（已做错误掩盖，画面就是花屏）/ `INVALID_BITSTREAM`。注意先清零（AVFrame 复用会残留标志）。
  - GPU（NVDEC）：`cuvidGetDecodeStatus` 的 `cuvidDecodeStatus_Error` / `Error_Concealed`；**历史实现是静默 `return 0`**，既丢帧又丢掉花屏信息，且 `pfnDisplayPicture` 返回 0 可能中止解析——现在是计数 + `return 1`（只丢这一帧）。
  - `StreamTask::updateGlitchStats()` 每秒结算窗口占比 → `StreamInfo.corrupted_frames`（累计）与 `StreamInfo.glitch_ratio`（最近 1 秒占比），Web 控制台据此显示「花屏累计 N」/「花屏 x%」标签。
  - 持续花屏时可选自动重连：`RTSP_GLITCH_RECONNECT_S=<秒>`（默认 0=关闭）。FFmpeg 封装没有暴露 RTCP FIR/PLI，无法直接请求 IDR，重连是唯一可靠的自愈手段。
  - 自测开关：`RTSP_FAKE_GLITCH=1` 每 100 个发布帧伪造一次花屏（同时置位逐帧标记），用于验证「日志 → proto → Web/客户端」链路（正常流实测 30 秒 0 误报）。
  - **逐帧标记**：除了整流的统计，还会随帧下发“这一帧本身是否花屏”——gRPC 走
    `FrameResponse.corrupted`，SHM 走 `ShmMeta.frame_flags` 的 `SHM_FRAME_FLAG_CORRUPTED`
    位（复用原 `reserved` 字段，布局与 `sizeof(ShmMeta)` 都没变）。客户端用
    `read_ex()` / `stream_frames_ex()` 取第三个返回值即可。注意 NVDEC 路径是丢弃花屏帧、
    不下发，因此该标记恒为 false（丢帧次数看 `corrupted_frames`）。
  - 像素域检测（灰块率、块效应、帧间差）只在解码器不报错但画面确有伪影时才需要，且夜间红外切黑白/低照度/纯色场景会大量误报，慎用。
- **proto 文件同步**：`stream_service.proto` 在根目录和 `client/` 下各有一份。修改后需要同时更新，并重新生成 C++ 和 Python 的 protobuf/gRPC 代码。
- **共享内存布局一致性**：C++ 端 `include/zero_copy_channel.hpp` 中的 `ShmMeta` **带 `alignas(64)`**，因此真实布局是：`sequence@0`、`meta@64`（不是 8）、`sizeof(ShmMeta)=64`（不是 48）、**payload@128**（不是 56）、`sizeof(ShmFrameSlot)=128`；`static_assert` 已把这些值固化。**消费端一律不要硬编码这些偏移**，应使用 `GetShmLayout` 返回的偏移（C++ 用 `offsetof/sizeof` 算）：Python 客户端 `_ShmReader` 优先取服务端偏移（兜底常量仅为兼容旧桩），C++ 工具见 `tools/save_frames.cpp`。历史上就因为头文件里一行过时注释（`// 48 bytes, offset 8`）导致 Python 硬编码错偏移，所有帧被当成 `size==0` 丢弃（表现为“能连上但 0 帧”）。
- **SHM 动态大小与运行中扩容**：`StreamTask::ensureShmChannel()` 在首帧解码后按实际帧大小创建 SHM（+25% 余量），帧变大（分辨率切换、`UpdateStream`）时会 `unlink` 旧对象并创建新对象（新的 inode）。因此**布局不能用 `GetShmLayout`（它只返回默认布局）**，消费端必须由 SHM 对象的实际文件大小反推（`total_size = 3 * slot_size + 8`，slot_size 为 64 的倍数）：Python 见 `derive_shm_layout_from_size()`，C++ 工具见 `tools/save_frames.cpp::deriveLayoutFromFileSize()`。由于旧映射在重建后会永久冻结在最后一帧，消费端还需在“一段时间没有新帧”后校验文件身份（`st_dev`/`st_ino`/大小）并重连：`_ShmReader._maybe_reconnect()` / `ShmReader::refreshIfStale()`；阻塞读必须分段等待信号量（`SHM_WAIT_SLICE_MS`），否则旧信号量等不到 `sem_post` 会卡死。
- **客户端 keepalive 参数**：`client/remote_capture.py::_DEFAULT_CHANNEL_OPTIONS` **不能**开启 `keepalive_permit_without_calls`（或把 `keepalive_time_ms` 设得很小）。服务端用 gRPC 默认 ping 防洪策略（`min_recv_ping_interval_without_data=300s`、`max_ping_strikes=2`），空闲连接上频繁的 keepalive ping 会被 GOAWAY(`ENHANCE_YOUR_CALM "too many pings"`) 断开，表现为“空闲一会儿后 RPC 突然报 Too many pings”。另外 `connect()` 默认会等待 channel ready（3s），地址不可达时直接返回 False 并打印可操作提示，不再“假连接成功”。
- **Web 控制台**：`web/server.py` 是 FastAPI BFF（REST + MJPEG），复用 `client/remote_capture.py`。服务端在 SHM 模式下不产出 JPEG，所以 SHM 流的预览由后端读共享内存再编码 JPEG，**要求后端与服务端同机**；跨机时只能用 gRPC JPEG 模式的流。控制类请求共用一个长连接（避免连接抖动），每个 MJPEG 预览会话用独立客户端。Web 端**不做鉴权**，默认监听 `0.0.0.0:8080`（`WEB_HOST`/`WEB_PORT` 可改）。
- **信号量必须先于 SHM 创建**：`ZeroCopyChannel` 构造函数中 `sem_open` 在 `shm_open` **之前**执行。因为 `shm_open(O_CREAT)` 会让 SHM 文件立刻对客户端可见，客户端一看到文件就会 `sem_open`；若信号量晚于 SHM 出现，客户端会因 ENOENT 退化到轮询模式（客户端另有 `_ShmReader._try_attach_notify_sem()` 做惰性重试与自动升级作为兜底）。
- **海康 SDK 放置**：`CMakeLists.txt` 默认在 `${CMAKE_SOURCE_DIR}/sdk/hikvision` 下查找 SDK 头文件（`hik_header/HCNetSDK.h`）和库文件（`hik_libs/libhcnetsdk.so` 等）。可通过 `-DHIKVISION_SDK_ROOT=/path/to/sdk` 指定其他路径；若未找到，CMake 会警告，`src/hik.cpp` / `src/hik_decoder.cpp` 不会被编译，`DECODER_HIK_SDK` 将降级为 CPU 解码器并运行时报错。
- **Docker 中的海康 SDK**：`Dockerfile` / `Dockerfile.cpu` 会把 `sdk/hikvision` 复制到镜像 `/opt/hikvision`，并通过 `LD_LIBRARY_PATH` 和 `ldconfig` 使其可被 `rtsp_server` 加载。`entrypoint.sh` 也做了兜底导出。
- **CUDA 架构**：`CMakeLists.txt` 中硬编码了 `75 80 86 89` 四个架构，如需支持新 GPU 需要修改此处。
- **线程池初始化**：`TaskScheduler` 在 `init()` 中为每个检测到的 GPU 直接创建线程池，避免懒加载带来的数据竞争问题。
- **心跳机制**：每个流有独立的心跳超时（默认 100 秒，上限 1 小时），客户端必须定期调用 `GetLatestFrame` / `StreamFrames` / `CheckStream` 保活，否则服务端会自动清理。`heartbeat_timeout_ms <= 0` 会被修正为默认值，禁止无限保活。
