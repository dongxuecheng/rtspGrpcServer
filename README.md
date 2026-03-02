# RTSP gRPC 服务器

本项目是一个 C++ 实现的 **RTSP 流媒体服务器**，通过 gRPC 接口向客户端提供视频流服务。
项目包含服务端应用（`src/`）和 Python 客户端（`client/`），展示了如何与服务进行交互。

---


## 🚀 特性

- **GPU 硬件加速**：支持 NVIDIA CUDA 硬件解码和 NVJPEG 编码
- **多解码器支持**：CPU (OpenCV) / GPU (CUDA) 可选
- **流式传输**：gRPC 服务端流式推送，低延迟实时传输
- **多客户端共享**：单路解码，多客户端零拷贝共享
- **灵活帧率控制**：支持解码间隔和客户端独立帧率限制
- **多 GPU 支持**：可指定 GPU ID
- **模块化设计**：工厂模式解耦解码器/编码器
- **Python SDK**：完整的客户端封装

---

## 🛠️ 依赖项

- C++17 兼容编译器（如 `g++`/`clang`）
- CMake 3.10+
- [gRPC](https://grpc.io/) 和 Protobuf
- OpenCV（推荐 4.x）
- CUDA Toolkit（GPU 解码需要）
- Python3 及 `grpcio`、`opencv-python`（客户端）

### 安装依赖（Ubuntu/Debian 示例）
```bash
sudo apt update
sudo apt install -y build-essential cmake libopencv-dev libgrpc++-dev protobuf-compiler
pip install grpcio grpcio-tools opencv-python
``` 

---

## 🏗️ 编译服务端

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

编译后会在 `build/` 目录生成可执行文件 `rtsp_server`。

### Docker 构建

使用提供的 `Dockerfile` 构建镜像：
```bash
docker build -t grpc_stream .
```

运行容器：
```bash
docker run -itd --gpus all \
  -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,video \
  --name cuda_stream \
  -p 50051:50051 \
  grpc_stream
```

---

## 📡 运行服务端

可以直接运行编译后的程序或通过 Docker 启动。默认监听端口 `50051`。

### 命令行选项
```bash
./rtsp_server
```

---

## 🧩 客户端使用

Python 客户端在 `client/` 目录下，展示了如何远程调用服务并获取视频帧。

### 方式 1：流式传输（推荐）

服务端主动推送帧，低延迟，低 CPU 占用：

```python
from remote_video_capture import RemoteVideoCapture
from stream_service_pb2 import DECODER_GPU_CUDA
import cv2

url = "rtsp://admin:password@192.168.1.100:554/stream"

with RemoteVideoCapture(url, 
                        decoder_type=DECODER_GPU_CUDA,
                        decode_interval_ms=100,
                        gpu_id=0) as cap:
    
    # 流式接收，max_fps 限制客户端帧率
    for ret, frame in cap.stream_frames(max_fps=10):
        if ret:
            cv2.imshow('Stream', frame)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
```

### 方式 2：轮询模式

客户端主动请求最新帧：

```python
with RemoteVideoCapture(url, decoder_type=DECODER_GPU_CUDA) as cap:
    while True:
        ret, frame = cap.read()  # 获取单帧
        if ret:
            cv2.imshow('Frame', frame)
            cv2.waitKey(30)
```

### 获取流信息

```python
info = cap.get_stream_info()
print(f"Resolution: {info['width']}x{info['height']}")
print(f"Decoder: {info['decoder_type']}")
print(f"Connected: {info['is_connected']}")
```

### 重新生成 Protobuf 文件

```bash
cd client
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. stream_service.proto
```

---

## 📌 Protobuf 接口定义

`stream_service.proto` 定义了 gRPC 服务接口：

### 服务接口

| 方法 | 类型 | 说明 |
|------|------|------|
| `StartStream` | Unary | 启动 RTSP 流，支持 GPU/CPU 解码器选择 |
| `StopStream` | Unary | 停止流 |
| `GetLatestFrame` | Unary | 获取单帧（轮询模式） |
| `StreamFrames` | Server Stream | 流式传输，服务端推送 |
| `CheckStream` | Unary | 查询流状态和信息 |

### 启动流参数

```protobuf
message StartRequest {
    string rtsp_url = 1;              // RTSP 地址
    int32 heartbeat_timeout_ms = 2;   // 心跳超时
    int32 decode_interval_ms = 3;     // 解码间隔（ms）
    DecoderType decoder_type = 4;     // CPU/GPU 解码器
    int32 gpu_id = 5;                 // GPU ID
}
```

### 解码器类型

```python
DECODER_CPU_OPENCV = 0    # OpenCV 软解
DECODER_GPU_CUDA = 1      # NVIDIA CUDA 硬解
DECODER_FFMPEG_NATIVE = 2 # FFmpeg 软解
```

---

## ⚡ 性能特性
**关键优势：**
- ✅ 解码一次，所有客户端共享
- ✅ 编码一次，零拷贝广播
- ✅ 每个客户端可独立设置帧率

---

## GRPC UI

使用 grpcui 工具进行接口测试：
```bash
.\grpcui.exe -plaintext 172.16.20.193:50051
```