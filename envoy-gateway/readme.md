# RTSPStreamService API 详细文档

## 1. API 路径与操作列表

| 路径 | 方法 | 操作 ID | 描述 |
| :--- | :--- | :--- | :--- |
| `/v1/streams` | `GET` | `RTSPStreamService_ListStreams` | 列出所有流 |
| `/v1/streams/start` | `POST` | `RTSPStreamService_StartStream` | 启动流服务 |
| `/v1/streams/stop` | `POST` | `RTSPStreamService_StopStream` | 停止流服务 |
| `/v1/streams/{streamId}` | `PUT` | `RTSPStreamService_UpdateStream` | 更新流地址 |
| `/v1/streams/{streamId}/frame` | `GET` | `RTSPStreamService_GetLatestFrame` | 获取最新帧 |
| `/v1/streams/{streamId}/status` | `GET` | `RTSPStreamService_CheckStream` | 检查流状态 |
| `/v1/streams/{streamId}/stream` | `GET` | `RTSPStreamService_StreamFrames` | 流式传输帧数据 |

---

## 2. 详细请求参数与响应体模型

### 2.1 请求模型 (Request Models)
| 模型名称 | 字段名 | 类型 | 必填 | 描述 |
| :--- | :--- | :--- | :--- | :--- |
| **StartRequest** | `rtspUrl` | string | 是 | RTSP 流媒体源地址 |
| | `heartbeatTimeoutMs` | integer | 否 | 心跳超时时间 (ms) |
| | `decodeIntervalMs` | integer | 否 | 解码间隔 (ms) |
| | `decoderType` | enum | 否 | 解码器类型 (见 4.2) |
| | `gpuId` | integer | 否 | GPU 设备 ID |
| | `keepOnFailure` | boolean | 否 | 出错后是否维持任务不自动删除 |
| | `useSharedMem` | boolean | 否 | 是否启用共享内存写入 |
| | `onlyKeyFrames` | boolean | 否 | 是否仅抓取/解码关键帧 |
| **StopRequest** | `streamId` | string | 是 | 需要停止的流 ID |
| **UpdateBody** | `newRtspUrl` | string | 是 | 更新后的目标 RTSP URL |

### 2.2 响应模型 (Response Models)
| 模型名称 | 字段名 | 类型 | 描述 |
| :--- | :--- | :--- | :--- |
| **ListStreamsResponse** | `totalCount` | integer | 流总数 |
| | `streams` | array | `StreamInfo` 数组 |
| **StreamInfo** | `streamId` | string | 流 ID |
| | `rtspUrl` | string | 当前流的 RTSP 地址 |
| | `status` | enum | 状态 (见 4.1) |
| | `decoderType` | enum | 使用的解码器类型 |
| | `width` | integer | 视频宽度 |
| | `height` | integer | 视频高度 |
| | `decodeIntervalMs` | integer | 当前配置的解码间隔 |
| | `keepOnFailure` | boolean | 失败保持配置 |
| | `onlyKeyFrames` | boolean | 关键帧配置 |
| | `useSharedMem` | boolean | 共享内存配置 |
| **FrameResponse**| `success` | boolean | 是否成功获取帧 |
| | `imageData` | string | **Base64 编码**的图像二进制数据 |
| | `message` | string | 状态或错误描述 |
| | `frameSeq` | integer | 图像帧序列号 |
| **CheckResponse** | `stream` | object | **`StreamInfo` 对象** (包含流的详细配置与状态) |
| | `message` | string | 额外的状态描述文字 |

---

## 3. 标准错误对象 (rpcStatus)
当 HTTP 状态码非 200 或返回 `default` 响应时使用：

| 属性名 | 类型 | 描述 |
| :--- | :--- | :--- |
| `code` | integer | gRPC 标准错误代码 |
| `message` | string | 错误信息简述 |
| `details` | array | 详细错误集合 (`protobufAny` 列表) |

---

## 4. 枚举值定义 (Enum)

### 4.1 流状态 (`StreamStatus`)
| 枚举值 | 说明 |
| :--- | :--- |
| `STATUS_CONNECTING` | 正在尝试连接源地址 |
| `STATUS_CONNECTED` | 已成功连接并正常解码 |
| `STATUS_DISCONNECTED` | 连接已中断或源地址无效 |
| `STATUS_NOT_FOUND` | 系统中不存在该流 ID |

### 4.2 解码器类型 (`DecoderType`)
| 枚举值 | 说明 |
| :--- | :--- |
| `DECODER_CPU_FFMPEG` | CPU 软件解码 (默认，基于 OpenCV/FFmpeg) |
| `DECODER_GPU_NVCUVID` | NVIDIA 硬件加速解码 (基于 NVCUVID) |

---

## 5. 特殊接口说明：StreamFrames
**路径**: `/v1/streams/{streamId}/stream`

该接口通过 HTTP Chunked Transfer Encoding 实现流式数据推送。客户端需要长连接并解析持续到达的 JSON 块。

**数据块结构**:
```json
{
  "result": {
    "success": true,
    "imageData": "/9j/4AAQSkZJRg...", // Base64
    "frameSeq": 1024
  },
  "error": null
}
```

* **控制参数**: 通过 Query 参数 `maxFps` 可动态限制服务器推送的最高频率。

---

### 修改建议说明：
1.  **修正 `CheckResponse`**: 原文档中把 `width`, `height` 等直接放在了响应根目录。根据 Swagger 定义，这些字段是在 `stream` 对象下的。
2.  **增加缺失字段**: 在 `StreamInfo` 中补充了 `keepOnFailure`, `onlyKeyFrames`, `useSharedMem` 等字段。
3.  **清晰化映射**: 明确了 `imageData` 是 Base64 字符串。

您是否需要我为您生成一段调用 `StreamFrames` (Chunked response) 的 Python 或 JavaScript 示例代码？