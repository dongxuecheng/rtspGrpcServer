# RTSP Stream Service API 文档 (HTTP/RESTful)

本服务提供 HTTP/JSON 接口，底层通过 Envoy 网关自动转换为 gRPC 调用。
**基础 URL**: `http://localhost:8080`

---

## 1. 启动流 (StartStream)
启动 RTSP 解码任务。

*   **URL**: `/v1/streams/start`
*   **Method**: `POST`
*   **Content-Type**: `application/json`
*   **Request Body**:
```json
{
  "rtsp_url": "string",
  "heartbeat_timeout_ms": 0,
  "decode_interval_ms": 0,
  "decoder_type": "DECODER_CPU_FFMPEG | DECODER_GPU_NVCUVID",
  "gpu_id": 0,
  "keep_on_failure": false,
  "use_shared_mem": false,
  "only_key_frames": false
}
```
*   **Response**: `{"success": true, "stream_id": "...", "message": "..."}`

---

## 2. 停止流 (StopStream)
删除并停止指定的解码任务。

*   **URL**: `/v1/streams/stop`
*   **Method**: `POST`
*   **Request Body**: `{"stream_id": "string"}`
*   **Response**: `{"success": true, "message": "..."}`

---

## 3. 获取最新帧 (GetLatestFrame)
获取指定流的当前最新视频帧。

*   **URL**: `/v1/streams/{stream_id}/frame`
*   **Method**: `GET`
*   **Response**:
```json
{
  "success": true,
  "image_data": "base64_string",
  "message": "string"
}
```

---

## 4. 流式传输 (StreamFrames)
持续获取视频帧流（HTTP Chunked）。

*   **URL**: `/v1/streams/{stream_id}/stream`
*   **Method**: `GET`
*   **Query Parameters**: `max_fps` (可选)
*   **Response**: 持续的 `FrameResponse` 数据流。

---

## 5. 检查流状态 (CheckStream)
查询指定流的详细信息（分辨率、状态等）。

*   **URL**: `/v1/streams/{stream_id}/status`
*   **Method**: `GET`
*   **Response**:
```json
{
  "status": "STATUS_CONNECTED | STATUS_DISCONNECTED | ...",
  "message": "string",
  "rtsp_url": "string",
  "decoder_type": "...",
  "width": 0,
  "height": 0,
  "decode_interval_ms": 0
}
```

---

## 6. 查询所有流 (ListStreams)
获取服务器上当前所有管理中的流任务。

*   **URL**: `/v1/streams`
*   **Method**: `GET`
*   **Response**:
```json
{
  "total_count": 0,
  "streams": [
    {
      "stream_id": "string",
      "rtsp_url": "string",
      "status": "string",
      "decoder_type": "string",
      "width": 0,
      "height": 0,
      "decode_interval_ms": 0
    }
  ]
}
```

---

## 7. 更新流信息 (UpdateStream)
仅用于更新流的 RTSP 连接地址。

*   **URL**: `/v1/streams/{stream_id}`
*   **Method**: `PUT`
*   **Request Body**: `{"new_rtsp_url": "string"}`
*   **Response**: `{"success": true, "message": "..."}`

---

### 重要注意事项：
1.  **关于 Bytes 数据**: 所有的 `image_data` (bytes 类型) 在通过 Envoy 时会自动被转码为 **Base64 编码的字符串**。如果你在前端开发中使用，请将其赋值给 `<img>` 标签的 `src` 属性：`src="data:image/jpeg;base64," + imageData`。
2.  **关于字段命名**: 虽然你在 `proto` 中定义的是下划线命名（如 `rtsp_url`），但在 JSON 请求中可以使用驼峰命名（如 `rtspUrl`）。Envoy 自动支持这两种格式的转换。
3.  **超时处理**: 所有的 GET/POST 请求默认在 `envoy.yaml` 中配置了 `60s` 超时，如果你的解码任务需要更长时间初始化，请在 `envoy.yaml` 的 `route` 配置中调整 `timeout` 值。