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
import logging
import os
import sys
import threading
import time
from collections import deque
from typing import Deque, Dict, Iterator, List, Optional, Tuple

import cv2
import numpy as np
import grpc
import uvicorn
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

logger = logging.getLogger("rtsp-web")

# ---- 复用仓库里的 Python 客户端 -------------------------------------------------
WEB_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(WEB_DIR)
sys.path.insert(0, os.path.join(PROJECT_ROOT, "client"))

from remote_capture import (  # noqa: E402
    DECODER_CPU_FFMPEG,
    DECODER_GPU_NVCUVID,
    DECODER_HIK_SDK,
    DECODER_NAMES,
    PIXEL_BGR,
    PIXEL_I420,
    PIXEL_NV12,
    PIXEL_YUYV422,
    PIXEL_FORMAT_NAMES,
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

def yuv_to_bgr(img: np.ndarray, pixel_format: int) -> Optional[np.ndarray]:
    """把共享内存的 YUV 原始帧转成 BGR（预览/截图需要 BGR 才能编 JPEG）。

    服务端在 SHM 模式下不产出 JPEG，若流配置为 YUV 格式（如 Ascend 常用的 NV12），
    这里后端必须自己转一次，否则预览会报错或花屏。转换失败时返回 None。
    """
    if pixel_format == PIXEL_BGR:
        return img
    try:
        if pixel_format == PIXEL_NV12:
            return cv2.cvtColor(img, cv2.COLOR_YUV2BGR_NV12)
        if pixel_format == PIXEL_I420:
            return cv2.cvtColor(img, cv2.COLOR_YUV2BGR_I420)
        if pixel_format == PIXEL_YUYV422:
            # (H, W*2) 单通道 -> (H, W, 2) 双通道，OpenCV 的 YUYV 转换接受两种布局
            return cv2.cvtColor(img.reshape(img.shape[0], img.shape[1] // 2, 2),
                                cv2.COLOR_YUV2BGR_YUYV)
    except Exception as e:  # noqa: BLE001
        logger.warning(f"YUV({PIXEL_FORMAT_NAMES.get(pixel_format, pixel_format)}) 转 BGR 失败: {e}")
        return None
    logger.warning(f"未知像素格式 {pixel_format}，无法转 BGR")
    return None


def frame_pixel_format(client: RTSPClient, stream_id: str) -> int:
    """当前帧的像素格式：以刚读到的 SHM 元数据为准（旧桩/ gRPC 模式回退到 BGR）。"""
    meta = client.get_last_frame_meta(stream_id)
    if meta is None:
        return PIXEL_BGR
    return int(meta.get("pix_fmt", PIXEL_BGR))


def grab_jpeg(client: RTSPClient, stream_id: str,
              quality: int = 80, max_width: int = 0, pixel_format: int = PIXEL_BGR) -> Optional[bytes]:
    """取一帧并编码为 JPEG（按流自身模式自动走 SHM 或 gRPC JPEG）"""
    _ts, img, _corrupted = client.read_ex(stream_id, blocking=False)
    if img is None:
        return None
    # SHM 的 YUV 流需要先转 BGR
    pix_fmt = pixel_format if pixel_format != PIXEL_BGR else frame_pixel_format(client, stream_id)
    img = yuv_to_bgr(img, pix_fmt)
    if img is None:
        return None
    if max_width and img.shape[1] > max_width:
        scale = max_width / float(img.shape[1])
        img = cv2.resize(img, (max_width, max(1, int(round(img.shape[0] * scale)))),
                         interpolation=cv2.INTER_AREA)
    ok, buf = cv2.imencode(".jpg", img, [int(cv2.IMWRITE_JPEG_QUALITY), int(quality)])
    return buf.tobytes() if ok else None


# ==================== 预览统计 ====================
# 说明：这里统计的 fps 是“本进程向浏览器实际发送”的帧率（受 ?fps= 限速影响），
# 不是服务端出帧率。服务端真实出帧率由 C++ 侧统计并通过 ListStreams/CheckStream
# 的 StreamInfo.fps 上报（见 server_fps）。

_PREVIEW_LOCK = threading.Lock()
_PREVIEWS: Dict[str, dict] = {}

# 滑动窗口长度：用窗口内的发送事件数 / 窗口时长，得到“最近的”发送帧率，
# 而不是从预览开始到现在的累计平均值（累计值会从 0 缓慢爬升、卡顿后又不降）。
_PREVIEW_WINDOW_S = 3.0


def _preview_enter(stream_id: str) -> None:
    with _PREVIEW_LOCK:
        st = _PREVIEWS.setdefault(stream_id, {"viewers": 0, "sent": 0, "last_ts": 0,
                                              "send_win": deque()})
        st["viewers"] += 1


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
        st["last_ts"] = ts
        now = time.monotonic()
        win: Deque[float] = st["send_win"]
        win.append(now)
        cutoff = now - _PREVIEW_WINDOW_S
        while win and win[0] < cutoff:
            win.popleft()


def _preview_stats(stream_id: str, server_fps: float = 0.0,
                   server_glitch_ratio: float = 0.0, server_corrupted: int = 0) -> dict:
    """预览统计。

    server_fps            : 服务端真实出帧率（来自 StreamInfo.fps，0 表示未知/停流）
    server_glitch_ratio   : 服务端最近 1 秒花屏帧占比（0 表示正常）
    server_corrupted      : 服务端累计花屏帧数
    send_fps              : 单个观众视角下，本进程实际发送的帧率（滑动窗口）
    """
    with _PREVIEW_LOCK:
        st = _PREVIEWS.get(stream_id)
        if not st:
            return {"viewers": 0, "sent": 0, "send_fps": 0.0, "server_fps": server_fps,
                    "server_glitch_ratio": server_glitch_ratio,
                    "server_corrupted_frames": server_corrupted, "last_ts": 0}
        viewers = max(1, st["viewers"])
        win: Deque[float] = st["send_win"]
        if len(win) >= 2 and win[-1] > win[0]:
            # 窗口内事件数 / 窗口跨度；多观众时按人数平摊，得到单观众帧率
            send_fps = (len(win) - 1) / (win[-1] - win[0]) / viewers
        else:
            send_fps = 0.0
        return {"viewers": st["viewers"], "sent": st["sent"],
                "send_fps": round(send_fps, 2), "server_fps": round(server_fps, 2),
                "server_glitch_ratio": round(server_glitch_ratio, 4),
                "server_corrupted_frames": server_corrupted,
                "last_ts": st["last_ts"]}


# ==================== 请求/响应模型 ====================

class StartStreamBody(BaseModel):
    rtsp_url: str = Field(..., min_length=1, description="rtsp:// 或 hik:// 地址")
    decoder_type: int = Field(DECODER_CPU_FFMPEG, description="0=CPU 1=GPU_NVCUVID 2=HIK_SDK")
    gpu_id: int = Field(0, ge=0)
    only_key_frames: bool = False
    use_shared_mem: bool = False
    # 共享内存原始帧的像素格式：0=BGR24 1=NV12 2=I420 3=YUYV422
    # （仅 use_shared_mem=true 时生效；gRPC 通道始终返回 JPEG）
    pixel_format: int = Field(PIXEL_BGR, ge=0, le=3)
    heartbeat_timeout_ms: int = Field(100000, ge=1, le=3600000)
    decode_interval_ms: int = Field(0, ge=0)
    keep_on_failure: bool = False


class UpdateUrlBody(BaseModel):
    new_rtsp_url: str = Field(..., min_length=1)


# ==================== API ====================

@app.get("/api/meta")
def api_meta():
    """前端初始化用：服务端地址、解码器/像素格式枚举、状态名"""
    return {
        "server": SERVER_ADDR,
        "decoders": [
            {"value": DECODER_CPU_FFMPEG, "label": DECODER_NAMES.get(DECODER_CPU_FFMPEG, "CPU")},
            {"value": DECODER_GPU_NVCUVID, "label": DECODER_NAMES.get(DECODER_GPU_NVCUVID, "GPU")},
            {"value": DECODER_HIK_SDK, "label": DECODER_NAMES.get(DECODER_HIK_SDK, "HIK")},
        ],
        # 仅 SHM 模式生效的原始帧格式；选 YUV 可省掉客户端侧的色彩转换
        # （例：Ascend DVPP 直接吃 NV12）
        "pixel_formats": [
            {"value": PIXEL_BGR, "label": "BGR24（默认，兼容 OpenCV）"},
            {"value": PIXEL_NV12, "label": "NV12 / YUV420SP（Ascend DVPP 常用）"},
            {"value": PIXEL_I420, "label": "I420 / YUV420P"},
            {"value": PIXEL_YUYV422, "label": "YUYV422（packed 4:2:2）"},
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
            pixel_format=body.pixel_format,
        )
        if not stream_id:
            raise HTTPException(status_code=400, detail="创建任务失败（请检查 RTSP URL 与解码器类型）")

        # 创建成功 ≠ 配置生效，必须把差异回报给前端，否则会出现
        # “明明选了 NVCUVID，列表却显示 CPU (FFmpeg)”这种难以自查的困惑。两种常见情况：
        #   1) 服务端把解码器降级：CPU-only 构建（ENABLE_CUDA=OFF）或 GPU 不可用
        #   2) 同一 URL 已有流被复用（服务端设计如此：单路解码多客户端共享），
        #      其解码器 / SHM 像素格式沿用原有流
        warnings: List[str] = []
        info = client.check_stream(stream_id) or {}
        actual_decoder = int(info.get("decoder_type_raw", body.decoder_type))
        if actual_decoder != body.decoder_type:
            warnings.append(
                f"请求的解码器「{DECODER_NAMES.get(body.decoder_type, body.decoder_type)}」未生效，"
                f"实际使用「{DECODER_NAMES.get(actual_decoder, actual_decoder)}」"
                f"（常见原因：服务端为 CPU-only 构建 ENABLE_CUDA=OFF，或 gpu_id 对应的 GPU 不可用）"
            )
        if body.use_shared_mem and not info.get("use_shared_mem"):
            warnings.append("请求的『共享内存 (SHM)』未生效，实际走的是 gRPC JPEG")
        if body.use_shared_mem:
            actual_pix = int(info.get("pixel_format", PIXEL_BGR))
            if actual_pix != body.pixel_format:
                warnings.append(
                    f"请求的原始帧格式「{PIXEL_FORMAT_NAMES.get(body.pixel_format, body.pixel_format)}」未生效，"
                    f"实际「{PIXEL_FORMAT_NAMES.get(actual_pix, actual_pix)}」"
                    f"（该 URL 已有流时会被复用，格式沿用原有流）"
                )
        for w in warnings:
            logger.warning(f"[web] 创建流 {stream_id}: {w}")
        return {"stream_id": stream_id, "warnings": warnings}


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
        # SHM + YUV 的流需要后端先转 BGR 才能编 JPEG
        pix_fmt = int(info.get("pixel_format", PIXEL_BGR)) if info.get("use_shared_mem") else PIXEL_BGR
        jpeg = grab_jpeg(client, stream_id, quality=quality, max_width=max_width, pixel_format=pix_fmt)
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
                # SHM 流可能是 YUV（如 NV12），预览需要 BGR 才能编 JPEG。
                # 以服务端上报的流配置为准（gRPC 流恒为 BGR）。
                info = client.check_stream(stream_id) or {}
                pix_fmt = int(info.get("pixel_format", PIXEL_BGR)) if info.get("use_shared_mem") else PIXEL_BGR
                if pix_fmt != PIXEL_BGR:
                    logger.info(
                        f"预览流 {stream_id} 的 SHM 像素格式为 "
                        f"{PIXEL_FORMAT_NAMES.get(pix_fmt, pix_fmt)}，后端将转 BGR 后编码 JPEG"
                    )
                idle_deadline = time.time() + FRAME_WAIT_MS / 1000.0
                while True:
                    tick = time.monotonic()
                    ts, img, frame_corrupted = client.read_ex(stream_id, blocking=False)
                    if img is None or ts == last_ts:
                        if time.time() > idle_deadline:
                            return  # 长时间无新帧：结束本次预览（前端会提示并重试）
                        time.sleep(idle_sleep)
                        idle_sleep = min(0.1, idle_sleep * 1.5)  # 空闲时逐步退避，避免空转打满 CPU
                        continue

                    idle_sleep = 0.02
                    last_ts = ts
                    idle_deadline = time.time() + FRAME_WAIT_MS / 1000.0

                    # YUV 帧（SHM）先转 BGR；也可根据元数据兜底修正（服务端版本差异）
                    if pix_fmt != PIXEL_BGR:
                        img = yuv_to_bgr(img, pix_fmt)
                        if img is None:
                            continue
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
                    # X-Frame-Glitch：这一帧被解码器判定为花屏（缺参考帧/错误掩盖/码流非法）
                    yield (b"--frame\r\n"
                           b"Content-Type: image/jpeg\r\n"
                           + b"Content-Length: " + str(len(data)).encode() + b"\r\n"
                           + b"X-Frame-Ts: " + str(int(ts)).encode() + b"\r\n"
                           + (b"X-Frame-Glitch: 1\r\n" if frame_corrupted else b"")
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
    """预览统计：send_fps（本进程发送）/ server_fps（服务端真实出帧率）/ 花屏情况"""
    server_fps = 0.0
    glitch_ratio = 0.0
    corrupted = 0
    with contextlib.suppress(HTTPException, grpc.RpcError, Exception):
        with grpc_client() as client:
            info = client.check_stream(stream_id)
            if info:
                server_fps = float(info.get("fps") or 0.0)
                glitch_ratio = float(info.get("glitch_ratio") or 0.0)
                corrupted = int(info.get("corrupted_frames") or 0)
    return _preview_stats(stream_id, server_fps, glitch_ratio, corrupted)


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
