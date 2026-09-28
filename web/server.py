#!/usr/bin/env python3
"""
RTSP gRPC 服务端 Web 控制台（BFF 后端）

    浏览器  ──HTTP/JSON + MJPEG──▶  本进程  ──gRPC──▶  rtsp_server

功能：
  - 查看当前所有流信息（ListStreams / CheckStream）
  - 实时预览（MJPEG）与单帧截图（JPEG）
  - 创建任务（StartStream）、停止任务（StopStream）、批量停止
  - 修改 RTSP URL（UpdateStream）

关于取帧：
  统一走 client.read()，它会自动按流的模式选择路径：
    - gRPC JPEG 模式：服务端返回已编码 JPEG，本进程解码后按需缩放/重编码
    - SHM 模式    ：服务端不产出 JPEG，本进程直接读共享内存（须与服务端同机）
  浏览器侧拿到的始终是 JPEG。

运行：
    python web/server.py                                  # http://127.0.0.1:8080
    GRPC_SERVER=172.16.20.193:50052 WEB_PORT=8080 python web/server.py
"""

from __future__ import annotations

import contextlib
import os
import sys
import threading
import time
from typing import Dict, Iterator, List, Optional

import cv2
import grpc
import uvicorn
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

# ---- 复用仓库里的 Python 客户端 -------------------------------------------------
WEB_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(WEB_DIR)
sys.path.insert(0, os.path.join(PROJECT_ROOT, "client"))

from remote_capture import (  # noqa: E402
    DECODER_CPU_FFMPEG,
    DECODER_GPU_NVCUVID,
    DECODER_HIK_SDK,
    DECODER_NAMES,
    RTSPClient,
    STATUS_NOT_FOUND,
)

SERVER_ADDR = os.getenv("GRPC_SERVER", "127.0.0.1:50052")
WEB_HOST = os.getenv("WEB_HOST", "0.0.0.0")
WEB_PORT = int(os.getenv("WEB_PORT", "8080"))

# 预览取帧的最长等待（毫秒）：超过则视为该路无帧
FRAME_WAIT_MS = int(os.getenv("WEB_FRAME_WAIT_MS", "10000"))

app = FastAPI(title="RTSP 流控制台", version="1.0.0")


# ==================== gRPC 客户端 ====================
# 控制类请求（增删查）共用一个长连接：
#   - 避免每个请求都新建 channel（连接抖动、资源浪费）
#   - 之前每请求一连接的做法会累积大量短命连接，触发服务端 ping 防洪
#     （GOAWAY ENHANCE_YOUR_CALM "too many pings"）
# 预览（MJPEG）则每个会话一个独立客户端，长时间占用互不影响。

class _ControlClient:
    """共享的 gRPC 控制客户端（带锁，线程安全）"""

    def __init__(self, address: str):
        self._address = address
        self._client: Optional[RTSPClient] = None
        self._lock = threading.Lock()

    def _ensure(self) -> RTSPClient:
        if self._client is None:
            client = RTSPClient(self._address)
            if not client.connect():
                raise HTTPException(status_code=503, detail=f"无法连接 gRPC 服务端 {self._address}")
            self._client = client
        return self._client

    def _drop(self) -> None:
        with contextlib.suppress(Exception):
            if self._client is not None:
                self._client.disconnect()
        self._client = None

    @contextlib.contextmanager
    def acquire(self) -> Iterator[RTSPClient]:
        with self._lock:
            try:
                yield self._ensure()
            except grpc.RpcError as e:
                self._drop()  # 连接已失效（服务端重启 / GOAWAY），下次请求重建
                detail = e.details() if hasattr(e, "details") else str(e)
                raise HTTPException(status_code=502, detail=f"gRPC 调用失败: {detail}") from e
            except Exception:
                self._drop()
                raise


_control = _ControlClient(SERVER_ADDR)


@contextlib.contextmanager
def grpc_client() -> Iterator[RTSPClient]:
    """控制类请求用的共享客户端"""
    with _control.acquire() as client:
        yield client


@contextlib.contextmanager
def dedicated_client() -> Iterator[RTSPClient]:
    """独占客户端（预览等长连接场景）"""
    client = RTSPClient(SERVER_ADDR)
    try:
        if not client.connect():
            raise HTTPException(status_code=503, detail=f"无法连接 gRPC 服务端 {SERVER_ADDR}")
        yield client
    finally:
        with contextlib.suppress(Exception):
            client.disconnect()


# ==================== 取帧 ====================

def grab_jpeg(client: RTSPClient, stream_id: str,
              quality: int = 80, max_width: int = 0) -> Optional[bytes]:
    """取一帧并编码为 JPEG（按流自身模式自动走 SHM 或 gRPC JPEG）"""
    _ts, img = client.read(stream_id, blocking=False)
    if img is None:
        return None
    if max_width and img.shape[1] > max_width:
        scale = max_width / float(img.shape[1])
        img = cv2.resize(img, (max_width, max(1, int(round(img.shape[0] * scale)))),
                         interpolation=cv2.INTER_AREA)
    ok, buf = cv2.imencode(".jpg", img, [int(cv2.IMWRITE_JPEG_QUALITY), int(quality)])
    return buf.tobytes() if ok else None


# ==================== 预览统计 ====================

_PREVIEW_LOCK = threading.Lock()
_PREVIEWS: Dict[str, dict] = {}


def _preview_enter(stream_id: str) -> None:
    with _PREVIEW_LOCK:
        st = _PREVIEWS.setdefault(stream_id, {"viewers": 0, "sent": 0, "fps": 0.0,
                                              "last_ts": 0, "started": 0.0, "_n": 0})
        st["viewers"] += 1
        # 每次新开预览都重新计时，保证 FPS 反映本次会话
        st["started"] = time.time()
        st["_n"] = 0


def _preview_exit(stream_id: str) -> None:
    with _PREVIEW_LOCK:
        st = _PREVIEWS.get(stream_id)
        if st:
            st["viewers"] = max(0, st["viewers"] - 1)


def _preview_sent(stream_id: str, ts: int) -> None:
    with _PREVIEW_LOCK:
        st = _PREVIEWS.get(stream_id)
        if not st:
            return
        st["sent"] += 1
        st["_n"] += 1
        st["last_ts"] = ts
        elapsed = time.time() - st["started"]
        if elapsed > 0:
            st["fps"] = st["_n"] / elapsed


def _preview_stats(stream_id: str) -> dict:
    with _PREVIEW_LOCK:
        st = _PREVIEWS.get(stream_id)
        if not st:
            return {"viewers": 0, "sent": 0, "fps": 0.0, "last_ts": 0}
        viewers = max(1, st["viewers"])
        return {"viewers": st["viewers"], "sent": st["sent"],
                # fps 是多个预览会话的合计，这里换算成“单个观众看到的发送帧率”
                "fps": round(st["fps"] / viewers, 2), "last_ts": st["last_ts"]}


# ==================== 请求/响应模型 ====================

class StartStreamBody(BaseModel):
    rtsp_url: str = Field(..., min_length=1, description="rtsp:// 或 hik:// 地址")
    decoder_type: int = Field(DECODER_CPU_FFMPEG, description="0=CPU 1=GPU_NVCUVID 2=HIK_SDK")
    gpu_id: int = Field(0, ge=0)
    only_key_frames: bool = False
    use_shared_mem: bool = False
    heartbeat_timeout_ms: int = Field(100000, ge=1, le=3600000)
    decode_interval_ms: int = Field(0, ge=0)
    keep_on_failure: bool = False


class UpdateUrlBody(BaseModel):
    new_rtsp_url: str = Field(..., min_length=1)


# ==================== API ====================

@app.get("/api/meta")
def api_meta():
    """前端初始化用：服务端地址、解码器枚举、状态名"""
    return {
        "server": SERVER_ADDR,
        "decoders": [
            {"value": DECODER_CPU_FFMPEG, "label": DECODER_NAMES.get(DECODER_CPU_FFMPEG, "CPU")},
            {"value": DECODER_GPU_NVCUVID, "label": DECODER_NAMES.get(DECODER_GPU_NVCUVID, "GPU")},
            {"value": DECODER_HIK_SDK, "label": DECODER_NAMES.get(DECODER_HIK_SDK, "HIK")},
        ],
    }


@app.get("/api/health")
def api_health():
    """服务端连通性探针"""
    try:
        with grpc_client() as client:
            streams = client.list_streams()
        return {"ok": True, "server": SERVER_ADDR, "stream_count": len(streams)}
    except HTTPException as e:
        return {"ok": False, "server": SERVER_ADDR, "error": e.detail}
    except Exception as e:  # noqa: BLE001
        return {"ok": False, "server": SERVER_ADDR, "error": str(e)}


@app.get("/api/streams")
def api_list_streams():
    with grpc_client() as client:
        return {"streams": client.list_streams()}


@app.get("/api/streams/{stream_id}")
def api_stream_info(stream_id: str):
    with grpc_client() as client:
        info = client.check_stream(stream_id)
    if not info or info.get("status") == STATUS_NOT_FOUND:
        raise HTTPException(status_code=404, detail=f"流不存在: {stream_id}")
    return info


@app.post("/api/streams", status_code=201)
def api_start_stream(body: StartStreamBody):
    with grpc_client() as client:
        stream_id = client.start_stream(
            rtsp_url=body.rtsp_url,
            heartbeat_timeout_ms=body.heartbeat_timeout_ms,
            decode_interval_ms=body.decode_interval_ms,
            decoder_type=body.decoder_type,
            gpu_id=body.gpu_id,
            keep_on_failure=body.keep_on_failure,
            use_shared_mem=body.use_shared_mem,
            only_key_frames=body.only_key_frames,
        )
    if not stream_id:
        raise HTTPException(status_code=400, detail="创建任务失败（请检查 RTSP URL 与解码器类型）")
    return {"stream_id": stream_id}


@app.post("/api/streams/{stream_id}/stop")
def api_stop_stream(stream_id: str):
    with grpc_client() as client:
        ok = client.stop_stream(stream_id)
    if not ok:
        raise HTTPException(status_code=404, detail=f"停止失败，流不存在: {stream_id}")
    return {"success": True, "stream_id": stream_id}


@app.post("/api/streams/stop-all")
def api_stop_all():
    """停止当前所有流（先列出再逐个停止）"""
    stopped: List[str] = []
    failed: List[str] = []
    with grpc_client() as client:
        for s in client.list_streams():
            sid = s["stream_id"]
            (stopped if client.stop_stream(sid) else failed).append(sid)
    return {"stopped": stopped, "failed": failed}


@app.patch("/api/streams/{stream_id}")
def api_update_stream(stream_id: str, body: UpdateUrlBody):
    with grpc_client() as client:
        ok = client.update_stream_url(stream_id, body.new_rtsp_url)
    if not ok:
        raise HTTPException(status_code=404, detail=f"更新失败，流不存在或 URL 被占用: {stream_id}")
    return {"success": True, "stream_id": stream_id, "rtsp_url": body.new_rtsp_url}


@app.get("/api/streams/{stream_id}/snapshot.jpg")
def api_snapshot(stream_id: str,
                 quality: int = Query(90, ge=1, le=100),
                 max_width: int = Query(0, ge=0, le=7680)):
    """取一帧完整 JPEG（“获取流图片”）"""
    with grpc_client() as client:
        info = client.check_stream(stream_id)
        if not info or info.get("status") == STATUS_NOT_FOUND:
            raise HTTPException(status_code=404, detail=f"流不存在: {stream_id}")
        jpeg = grab_jpeg(client, stream_id, quality=quality, max_width=max_width)
    if jpeg is None:
        raise HTTPException(status_code=409, detail="暂时没有可用帧（流未连接或首帧尚未到达）")
    headers = {"Content-Disposition": f'inline; filename="{stream_id}.jpg"',
               "Cache-Control": "no-store"}
    return Response(content=jpeg, media_type="image/jpeg", headers=headers)


@app.get("/api/streams/{stream_id}/mjpeg")
def api_mjpeg(stream_id: str,
              fps: int = Query(10, ge=1, le=60),
              quality: int = Query(80, ge=1, le=100),
              max_width: int = Query(960, ge=0, le=7680)):
    """MJPEG 实时预览（可直接作为 <img src> 使用）"""

    def gen() -> Iterator[bytes]:
        _preview_enter(stream_id)
        period = 1.0 / fps
        idle_sleep = 0.02
        try:
            with dedicated_client() as client:
                last_ts = -1
                idle_deadline = time.time() + FRAME_WAIT_MS / 1000.0
                while True:
                    tick = time.monotonic()
                    ts, img = client.read(stream_id, blocking=False)
                    if img is None or ts == last_ts:
                        if time.time() > idle_deadline:
                            return  # 长时间无新帧：结束本次预览（前端会提示并重试）
                        time.sleep(idle_sleep)
                        idle_sleep = min(0.1, idle_sleep * 1.5)  # 空闲时逐步退避，避免空转打满 CPU
                        continue

                    idle_sleep = 0.02
                    last_ts = ts
                    idle_deadline = time.time() + FRAME_WAIT_MS / 1000.0
                    if max_width and img.shape[1] > max_width:
                        scale = max_width / float(img.shape[1])
                        img = cv2.resize(img, (max_width, max(1, int(round(img.shape[0] * scale)))),
                                         interpolation=cv2.INTER_AREA)
                    ok, buf = cv2.imencode(".jpg", img, [int(cv2.IMWRITE_JPEG_QUALITY), quality])
                    if not ok:
                        continue
                    data = buf.tobytes()
                    _preview_sent(stream_id, ts)
                    # 每个分片带上帧时间戳：浏览器据此计算“端到端延迟”（含隧道/网络积压），
                    # 并在积压过大时重新连接以丢弃缓冲。额外的分片头对 <img> 无害。
                    yield (b"--frame\r\n"
                           b"Content-Type: image/jpeg\r\n"
                           + b"Content-Length: " + str(len(data)).encode() + b"\r\n"
                           + b"X-Frame-Ts: " + str(int(ts)).encode() + b"\r\n"
                           + b"\r\n" + data + b"\r\n")

                    # 限速到目标帧率（只补足剩余的间隔时间）
                    time.sleep(max(0.0, period - (time.monotonic() - tick)))
        except GeneratorExit:  # 浏览器断开
            raise
        finally:
            _preview_exit(stream_id)
    return StreamingResponse(gen(), media_type="multipart/x-mixed-replace; boundary=frame",
                             headers={"Cache-Control": "no-store"})


@app.get("/api/streams/{stream_id}/stats")
def api_stream_stats(stream_id: str):
    return _preview_stats(stream_id)


# ==================== 静态页面 ====================

@app.get("/")
def index():
    return FileResponse(os.path.join(WEB_DIR, "static", "index.html"))


app.mount("/static", StaticFiles(directory=os.path.join(WEB_DIR, "static")), name="static")


def main() -> None:
    print(f"[web] 控制台: http://{WEB_HOST}:{WEB_PORT}  (gRPC 服务端: {SERVER_ADDR})", flush=True)

    # 启动时探活一次，避免地址写错时只在页面上看到含糊的“服务端不可达”
    probe = RTSPClient(SERVER_ADDR)
    if probe.connect(ready_timeout_sec=3.0):
        try:
            print(f"[web] gRPC 连接正常，当前 {len(probe.list_streams())} 路流", flush=True)
        except Exception as e:  # noqa: BLE001
            print(f"[web] gRPC 已连接但调用失败: {e}", flush=True)
        finally:
            with contextlib.suppress(Exception):
                probe.disconnect()
    else:
        print("[web] 无法连接 gRPC 服务端，请检查环境变量 GRPC_SERVER。", flush=True)
        print("[web] 容器部署通常为 -p 50052:50051，同机可用 127.0.0.1:50052", flush=True)

    print("[web] 提示: 预览 SHM 模式的流需要本进程与服务端在同一台机器（同一 /dev/shm）", flush=True)
    uvicorn.run(app, host=WEB_HOST, port=WEB_PORT, log_level="warning")


if __name__ == "__main__":
    main()
