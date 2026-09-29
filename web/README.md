# RTSP 流控制台（Web 前端）

一个轻量的 Web 控制台，用来查看/管理 `rtsp_server` 的流，并做实时预览。

```
浏览器 ──HTTP/JSON + MJPEG──▶ web/server.py ──gRPC──▶ rtsp_server
```

## 功能

| 功能 | 说明 | 对应 RPC |
|---|---|---|
| 查看流信息 | 列表自动刷新（2s），显示 ID / URL / 状态 / 解码器 / 分辨率 / 选项 | `ListStreams` / `CheckStream` |
| 获取流图片 | 实时预览（MJPEG）+ 单帧截图下载（JPEG） | `GetLatestFrame` / SHM |
| 创建任务 | 表单：URL、解码器（CPU/GPU/海康）、GPU ID、仅关键帧、SHM、心跳超时、解码间隔 | `StartStream` |
| 停止任务 | 单行停止 / 一键全部停止 | `StopStream` |
| 修改 URL | 行内“改 URL”按钮 | `UpdateStream` |

## 运行

```bash
pip install fastapi uvicorn          # 依赖

python web/server.py                 # 默认 http://127.0.0.1:8080，服务端 127.0.0.1:50052
```

环境变量：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `GRPC_SERVER` | `127.0.0.1:50052` | rtsp_server 地址（**注意端口**：容器常见映射 `-p 50052:50051`，宿主是 50052） |
| `WEB_HOST` | `0.0.0.0` | 监听地址（`127.0.0.1` 表示仅本机） |
| `WEB_PORT` | `8080` | 监听端口 |
| `WEB_FRAME_WAIT_MS` | `10000` | 预览无新帧多久后结束该 MJPEG 连接（前端会自动重试） |

示例：

```bash
# 服务端在别的机器上（此时无法预览 SHM 模式的流，只能预览 gRPC JPEG 模式的流）
GRPC_SERVER=172.16.20.193:50052 python web/server.py

# 换端口
WEB_PORT=8090 python web/server.py
```

启动时会自动探活一次 gRPC 服务端；地址写错会立刻提示，例如：

```
[web] 控制台: http://0.0.0.0:8080  (gRPC 服务端: 172.16.20.93:50052)
ERROR - 连接服务器失败: 172.16.20.93:50052 不可达 (3s 超时: FutureTimeoutError)
[web] 无法连接 gRPC 服务端，请检查环境变量 GRPC_SERVER。
```

## 取帧说明（重要）

服务端在 **SHM 模式**下不会编码 JPEG，所以预览的 JPEG 由 `web/server.py` 生成：

| 流的模式 | 取帧路径 |
|---|---|
| gRPC JPEG（`use_shared_mem=false`） | 服务端返回已编码 JPEG → 本进程解码后按需缩放/重编码 |
| SHM + BGR24（默认） | 本进程直接读共享内存原始帧 → 编码为 JPEG |
| SHM + YUV（`pixel_format=NV12/I420/YUYV422`） | 读到的原始帧是 YUV → 后端先 `cv2.cvtColor` 转 BGR → 再编码 JPEG |

> 选 YUV 的目的是让**下游客户端**（如 Ascend DVPP）省掉色彩转换；Web 预览只是旁路取图，
> 因此后端会自己转一次 BGR（多一次全图转换，预览分辨率建议配合 `max_width` 限制）。

因此**预览 SHM 模式的流时，本进程必须与服务端在同一台机器**（同一 `/dev/shm`，容器需挂载 `-v /dev/shm:/dev/shm`）。
跨机使用时请只用 gRPC JPEG 模式的流。

预览参数：`/api/streams/<id>/mjpeg?fps=10&max_width=1280&quality=80`（前端按“流畅/标准/高清”三档调用）。
每个分片都带 `X-Frame-Ts: <毫秒时间戳>` 头，客户端可据此计算**端到端延迟**（含网络/隧道积压）。

## 预览延迟（重要）

MJPEG 是“只进不出”的流式传输：**只要接收速度低于发送速度，积压就会越来越久**，表现为画面延迟持续增大且不会自己恢复
（常见于通过 VS Code 端口转发 / 远程网络 / 弱网查看时）。实测（先正常读、再停读 10s 后继续读）：

| 场景 | 端到端延迟 |
|---|---|
| 正常读取 | 142 ~ 456 ms |
| 停读 10s 后再读（模拟慢消费者积压） | 10057ms → 8969ms（把积压的旧帧慢慢吐出来） |
| 重新连接（丢弃缓冲） | 立即回到 125 ~ 182 ms |

因此前端预览没有用 `<img src=mjpeg>`，而是 **fetch + 解析 multipart + 渲染到 canvas**，从而可以：

1. 用分片头的 `X-Frame-Ts` **测出真实端到端延迟**（会话内最小值为基准，自动抵消两端时钟偏差）；
2. 一次收到多帧时**只渲染最新一帧**（丢中间积压帧）；
3. 延迟 > 1.5s 持续 2s → **自动重新同步**（断开重连以丢弃隧道/浏览器侧积压）；
4. 30 秒内反复积压 ≥3 次 → **自动降一档画质**（高清 → 标准 → 流畅）。

手动手段：预览面板的“画质”下拉 + “重新同步”按钮；统计栏会并排显示 `客户端 FPS · 服务端 FPS · 端到端延迟`，
两者 FPS 差距大即说明瓶颈在网络/隧道，选“流畅”档或改用 gRPC 模式。

## REST 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 服务端连通性 + 流数量 |
| GET | `/api/meta` | 解码器枚举、服务端地址 |
| GET | `/api/streams` | 列出所有流 |
| GET | `/api/streams/{id}` | 单个流信息 |
| POST | `/api/streams` | 创建任务（JSON 见下） |
| POST | `/api/streams/{id}/stop` | 停止单个流 |
| POST | `/api/streams/stop-all` | 停止所有流 |
| PATCH | `/api/streams/{id}` | 修改 RTSP URL `{"new_rtsp_url": "..."}` |
| GET | `/api/streams/{id}/snapshot.jpg` | 单帧截图（`quality`、`max_width`） |
| GET | `/api/streams/{id}/mjpeg` | MJPEG 预览（`fps`、`quality`、`max_width`） |
| GET | `/api/streams/{id}/stats` | 预览统计（viewers / 已发帧数 / FPS / 最后帧时间戳） |

创建任务请求体示例：

```json
{
  "rtsp_url": "rtsp://admin:pass@172.16.22.16:554/Streaming/Channels/101",
  "decoder_type": 0,
  "gpu_id": 0,
  "only_key_frames": false,
  "use_shared_mem": true,
  "pixel_format": 1,
  "heartbeat_timeout_ms": 100000,
  "decode_interval_ms": 0,
  "keep_on_failure": false
}
```

> `pixel_format`（0=BGR24 1=NV12 2=I420 3=YUYV422）仅 `use_shared_mem=true` 时生效；
> 页面上对应「原始帧格式 (SHM)」下拉框，留默认 BGR24 即为原有行为。

交互式接口文档（FastAPI 自带）：<http://127.0.0.1:8080/docs>

## 目录

```
web/
├── server.py            # FastAPI 后端（REST + MJPEG + 静态页面）
├── static/
│   ├── index.html
│   ├── style.css
│   └── app.js           # 原生 JS，无构建步骤
└── README.md
```

## 说明与限制

- **无鉴权**：与 `rtsp_server` 一致，默认只监听本机；若用 `WEB_HOST=0.0.0.0` 暴露到局域网，请自行加反向代理/鉴权。
- 心跳：服务端 100s 内没有客户端调用会自动清理流；控制台每 2s 刷新流列表，预览时持续取帧，都会续期心跳。
- 预览有帧率上限（默认 10 FPS，`?fps=` 可调），并做了空闲退避，避免无帧时打满 CPU。
- 控制类请求共用一个长连接；每个 MJPEG 预览会话使用独立客户端，互不阻塞。
