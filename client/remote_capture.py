"""
RTSP gRPC / SharedMemory 统一客户端
"""

import os
import sys
import time
import mmap
import struct
import ctypes
import ctypes.util
import logging
import atexit
import weakref
import contextlib
from typing import Optional, Tuple, List, Dict, Generator

import cv2
import grpc
import numpy as np
import urllib.parse

# 可选依赖：turbojpeg 仅在 gRPC JPEG 模式下使用
try:
    import turbojpeg
    _HAS_TURBOJPEG = True
except Exception:
    turbojpeg = None
    _HAS_TURBOJPEG = False

import stream_service_pb2
import stream_service_pb2_grpc

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

__all__ = [
    "RTSPClient",
    "DECODER_CPU_FFMPEG",
    "DECODER_GPU_NVCUVID",
    "DECODER_HIK_SDK",
    "DECODER_NAMES",
    "PIXEL_BGR",
    "PIXEL_NV12",
    "PIXEL_I420",
    "PIXEL_YUYV422",
    "PIXEL_FORMAT_NAMES",
    "split_yuv_planes",
    "STATUS_CONNECTING",
    "STATUS_CONNECTED",
    "STATUS_DISCONNECTED",
    "STATUS_NOT_FOUND",
    "STATUS_NAMES",
]

# ==================== 常量（与 proto 保持一致）====================

DECODER_CPU_FFMPEG = stream_service_pb2.DECODER_CPU_FFMPEG
DECODER_GPU_NVCUVID = stream_service_pb2.DECODER_GPU_NVCUVID
DECODER_HIK_SDK = stream_service_pb2.DECODER_HIK_SDK

DECODER_NAMES = {
    DECODER_CPU_FFMPEG: "CPU (FFmpeg)",
    DECODER_GPU_NVCUVID: "GPU (NVCUVID)",
    DECODER_HIK_SDK: "HIK SDK"
}

# 共享内存原始帧的像素格式（仅 use_shared_mem=True 时生效；gRPC 通道恒为 JPEG）。
# 使用方式：client.start_stream(..., use_shared_mem=True, pixel_format=PIXEL_NV12)
PIXEL_BGR = stream_service_pb2.PIXEL_BGR            # 默认，BGR24（OpenCV 原生）
PIXEL_NV12 = stream_service_pb2.PIXEL_NV12          # YUV420SP，Ascend DVPP 常用
PIXEL_I420 = stream_service_pb2.PIXEL_I420          # YUV420P
PIXEL_YUYV422 = stream_service_pb2.PIXEL_YUYV422    # packed 4:2:2

PIXEL_FORMAT_NAMES = {
    PIXEL_BGR: "BGR24",
    PIXEL_NV12: "NV12(YUV420SP)",
    PIXEL_I420: "I420(YUV420P)",
    PIXEL_YUYV422: "YUYV422",
}

STATUS_CONNECTING = stream_service_pb2.STATUS_CONNECTING
STATUS_CONNECTED = stream_service_pb2.STATUS_CONNECTED
STATUS_DISCONNECTED = stream_service_pb2.STATUS_DISCONNECTED
STATUS_NOT_FOUND = stream_service_pb2.STATUS_NOT_FOUND

STATUS_NAMES = {
    STATUS_CONNECTING: "连接中",
    STATUS_CONNECTED: "已连接",
    STATUS_DISCONNECTED: "无法连接",
    STATUS_NOT_FOUND: "不存在"
}

_DEFAULT_CHANNEL_OPTIONS = [
    ('grpc.max_receive_message_length', 10 * 1024 * 1024),
    # keepalive 必须与服务端的 ping 防洪策略兼容：gRPC C++ 服务端默认
    # min_recv_ping_interval_without_data=300s、max_ping_strikes=2，
    # 空闲连接上每 5s 一次的 keepalive ping 会被判为 "too many pings" 并
    # GOAWAY(ENHANCE_YOUR_CALM) 断开（表现为长时间空闲后 RPC 突然报 Too many pings）。
    # 因此不在“无调用”时发 ping；仅在调用进行中由 keepalive 保活。
    ('grpc.keepalive_time_ms', 30000),
    ('grpc.keepalive_timeout_ms', 10000),
    ('grpc.keepalive_permit_without_calls', False),
]


# ==================== 共享内存辅助 ====================

def align_up(value: int, alignment: int) -> int:
    """模拟 C++ alignas 内存对齐"""
    return (value + alignment - 1) & ~(alignment - 1)


def _cleanup_client_on_exit(client_ref):
    """atexit 钩子：进程退出时尝试关闭所有 SHM reader，释放 mmap 引用"""
    client = client_ref()
    if client is None:
        return
    try:
        logger.debug("[atexit] Cleaning up RTSPClient SHM readers")
        for reader in list(client._shm_readers.values()):
            try:
                reader.close()
            except Exception:
                pass
        client._shm_readers.clear()
    except Exception:
        pass


CV_DEPTH_TO_NUMPY = {
    0: np.uint8,
    1: np.int8,
    2: np.uint16,
    3: np.int16,
    4: np.int32,
    5: np.float32,
    6: np.float64,
}


_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)


class _timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


_libc.sem_open.restype = ctypes.c_void_p
_libc.sem_close.argtypes = [ctypes.c_void_p]
_libc.sem_close.restype = ctypes.c_int
_libc.sem_wait.argtypes = [ctypes.c_void_p]
_libc.sem_wait.restype = ctypes.c_int
_libc.sem_trywait.argtypes = [ctypes.c_void_p]
_libc.sem_trywait.restype = ctypes.c_int
_libc.sem_timedwait.argtypes = [ctypes.c_void_p, ctypes.POINTER(_timespec)]
_libc.sem_timedwait.restype = ctypes.c_int
_libc.sem_post.argtypes = [ctypes.c_void_p]
_libc.sem_post.restype = ctypes.c_int
_libc.clock_gettime.argtypes = [ctypes.c_int, ctypes.POINTER(_timespec)]
_libc.clock_gettime.restype = ctypes.c_int

CLOCK_REALTIME = 0
SEM_FAILED = ctypes.c_void_p(-1).value


class _NotifySemaphore:
    """跨进程 POSIX 有名信号量包装器"""

    def __init__(self, name: str):
        self._sem = _libc.sem_open(name.encode(), 0)
        if self._sem is None or self._sem == SEM_FAILED:
            errno = ctypes.get_errno()
            raise OSError(errno, f"sem_open failed for {name}: {os.strerror(errno)}")

    def wait(self, timeout_ms: Optional[float] = None) -> bool:
        if timeout_ms is None or timeout_ms < 0:
            return _libc.sem_wait(self._sem) == 0
        abs_ts = _timespec()
        if _libc.clock_gettime(CLOCK_REALTIME, ctypes.byref(abs_ts)) != 0:
            return False
        sec = int(timeout_ms // 1000)
        nsec = int((timeout_ms % 1000) * 1_000_000)
        abs_ts.tv_sec += sec
        abs_ts.tv_nsec += nsec
        if abs_ts.tv_nsec >= 1_000_000_000:
            abs_ts.tv_sec += 1
            abs_ts.tv_nsec -= 1_000_000_000
        return _libc.sem_timedwait(self._sem, ctypes.byref(abs_ts)) == 0

    def close(self):
        if self._sem:
            _libc.sem_close(self._sem)
            self._sem = None

# ==== 共享内存布局常量（必须与 C++ include/zero_copy_channel.hpp 保持一致）====
# ⚠️ 这里只是**兜底值**：真正权威的偏移来自服务端 GetShmLayout（C++ 用 offsetof/sizeof 计算）。
# 对应 C++ 的 ShmFrameSlot：
#   sequence (8B)  @ 0
#   对齐填充        [8, 64)   ← 因为 ShmMeta 带 alignas(64)
#   ShmMeta        @ 64，sizeof(ShmMeta) = 64（52B 数据 + 12B 尾部填充）
#   payload        @ 128
# 注意：不要按“注释/直觉”改成 8/48/56，务必以 GetShmLayout 返回值为准。
SHM_SLOT_COUNT = 3                                             # constexpr int SHM_SLOT_COUNT
SHM_ALIGNMENT = 64                                             # alignas(64)
SHM_SEQ_OFFSET = 0                                             # offsetof(ShmFrameSlot, sequence)
SHM_META_OFFSET = 64                                           # offsetof(ShmFrameSlot, meta)（被 alignas(64) 抬高）
SHM_META_DATA_SIZE = 64                                        # sizeof(ShmMeta)
SHM_PAYLOAD_OFFSET = SHM_META_OFFSET + SHM_META_DATA_SIZE      # = 128
SHM_HEAD_IDX_SIZE = 8                                          # sizeof(uint64_t)
# 兜底默认值：C++ 端 DEFAULT_SHM_FRAME_BYTES = 3 * 1920 * 1080
SHM_FALLBACK_MAX_FRAME_BYTES = 3 * 1920 * 1080

# ShmMeta.frame_flags 的位定义（与 C++ 端 zero_copy_channel.hpp 保持一致）
SHM_FLAG_CORRUPTED = 1 << 0   # 该帧解码出错/花屏（缺参考帧、错误掩盖、码流非法）

# ShmMeta 内部字段（struct 格式 "QQQQIIIII"，共 52 字节）在 meta 内的偏移：
#   actual_size@0 width@8 height@16 timestamp@24
#   channels@32 depth@36 step@40 frame_flags@44 pixel_format@48

# 服务端“运行中扩容”的探测参数：
# 帧变大（分辨率切换）时服务端会 unlink 旧 SHM 并创建新对象（新 inode），
# 客户端手里的旧映射会永远冻结在最后一帧，所以必须在“一段时间没有新帧”后
# 校验文件身份（st_dev/st_ino/大小）并重新连接。
SHM_RECONNECT_PROBE_INTERVAL_S = 0.5   # 探测间隔（秒），也是“多久无新帧才开始探测”
SHM_WAIT_SLICE_MS = 200                # 阻塞读时信号量单次等待上限（毫秒）
SHM_SEM_ATTACH_RETRY_S = 0.5           # 信号量打开失败后的重试间隔（秒）


# 计算共享内存布局（与 C++ getShmLayoutInfo 保持一致）
def _compute_shm_layout(max_frame_bytes: int) -> dict:
    """根据单帧最大字节数计算完整布局（兜底用途，动态部分应以 SHM 文件大小为准）"""
    # 每个 slot 的总大小 = 元数据区 + 最大帧数据，对齐到 64
    SLOT_SIZE = align_up(SHM_PAYLOAD_OFFSET + max_frame_bytes, SHM_ALIGNMENT)

    # head_idx 在所有 slot 之后
    HEAD_IDX_OFFSET = SHM_SLOT_COUNT * SLOT_SIZE
    # 总大小 = 所有 slot + head_idx（对齐到 8 字节）
    TOTAL_SIZE = align_up(HEAD_IDX_OFFSET + SHM_HEAD_IDX_SIZE, SHM_HEAD_IDX_SIZE)

    return {
        "slot_count": SHM_SLOT_COUNT,
        "max_frame_bytes": max_frame_bytes,
        "alignment": SHM_ALIGNMENT,
        "slot_size": SLOT_SIZE,
        "seq_offset": SHM_SEQ_OFFSET,
        "meta_offset": SHM_META_OFFSET,
        "payload_offset": SHM_PAYLOAD_OFFSET,
        "meta_data_size": SHM_META_DATA_SIZE,
        "head_idx_offset": HEAD_IDX_OFFSET,
        "total_size": TOTAL_SIZE,
    }


def derive_shm_layout_from_size(file_size: int,
                               payload_offset: int = SHM_PAYLOAD_OFFSET) -> Optional[dict]:
    """由共享内存对象的实际字节数反推布局（动态部分）。

    服务端会根据每路流的实际分辨率动态决定 max_frame_bytes（见 stream_task.cpp 中的
    ensureShmChannel），客户端无法凭空算出 slot_size。但 C++ 端的布局公式保证：

        total_size = SHM_SLOT_COUNT * slot_size + 8
        slot_size  = align_up(payload_offset + max_frame_bytes, 64)

    因此文件大小可以唯一确定 slot_size（64 的整数倍），从而得到全部偏移量。
    payload_offset 取自服务端 GetShmLayout（兜底为常量），因为它是 C++ 结构体布局决定的。
    派生出的 max_frame_bytes 是 payload 容量上界（>= 服务端实际值），仅用于校验。
    """
    if not file_size or file_size <= SHM_HEAD_IDX_SIZE:
        return None
    if (file_size - SHM_HEAD_IDX_SIZE) % SHM_SLOT_COUNT != 0:
        return None
    slot_size = (file_size - SHM_HEAD_IDX_SIZE) // SHM_SLOT_COUNT
    if slot_size % SHM_ALIGNMENT != 0 or slot_size <= payload_offset:
        return None
    return {
        "slot_count": SHM_SLOT_COUNT,
        "max_frame_bytes": slot_size - payload_offset,
        "alignment": SHM_ALIGNMENT,
        "slot_size": slot_size,
        "seq_offset": SHM_SEQ_OFFSET,
        "meta_offset": SHM_META_OFFSET,
        "payload_offset": payload_offset,
        "meta_data_size": SHM_META_DATA_SIZE,
        "head_idx_offset": SHM_SLOT_COUNT * slot_size,
        "total_size": file_size,
    }


def split_yuv_planes(frame: np.ndarray, pixel_format: int) -> dict:
    """把 YUV 原始帧按平面拆开（纯切片视图，不拷贝数据）。

    服务端写入共享内存的 YUV 缓冲是**紧密排列**的（无行末 padding），因此可以直接
    按扁平缓冲切出各平面（返回的都是视图，不拷贝数据）：

        NV12    y 平面 W*H 字节，其后是 W*ceil(H/2) 字节的交错 UV
        I420    y 平面 W*H 字节，其后是 (W/2)*(H/2) 的 U，再是 (W/2)*(H/2) 的 V
        YUYV422 每 4 字节为 [Y0,U,Y1,V]

    注意：I420 的色度平面宽度是 W/2，无法按“整行”切分，所以这里统一在扁平缓冲上切片。

    :param frame: read()/read_ex() 返回的 YUV 帧（2-D uint8 数组）
    :param pixel_format: PIXEL_NV12 / PIXEL_I420 / PIXEL_YUYV422
    :return: 字典形式的平面；传入 BGR 或形状不匹配时抛出 ValueError
    """
    if frame is None or frame.ndim != 2:
        raise ValueError("frame 必须是二维 YUV 缓冲")

    rows, cols = frame.shape
    if pixel_format in (PIXEL_NV12, PIXEL_I420):
        if rows % 3 != 0:
            raise ValueError(f"YUV420 缓冲行数 {rows} 不是 3 的倍数，无法推导平面")
        h = rows * 2 // 3
        w = cols
        flat = frame.reshape(-1)
        y = flat[:w * h].reshape(h, w)
        if pixel_format == PIXEL_NV12:
            chroma_h = (h + 1) // 2
            return {"y": y, "uv": flat[w * h:w * h + w * chroma_h].reshape(chroma_h, w)}
        cw, ch = (w + 1) // 2, (h + 1) // 2
        base = w * h
        u = flat[base:base + cw * ch].reshape(ch, cw)
        v = flat[base + cw * ch:base + 2 * cw * ch].reshape(ch, cw)
        return {"y": y, "u": u, "v": v}

    if pixel_format == PIXEL_YUYV422:
        if cols % 2 != 0:
            raise ValueError(f"YUYV422 行字节数 {cols} 不是偶数")
        return {"yuyv": frame}

    raise ValueError(f"不支持的 YUV 像素格式: {pixel_format}")


class _ShmReader:
    """共享内存帧读取器（内部使用）"""

    def __init__(self, stream_id: str, layout_info: Optional[dict] = None):
        self.stream_id = stream_id
        self.shm_paths = [f"/dev/shm/{stream_id}", f"/{stream_id.lstrip('/')}"]

        self.UINT64_SIZE = 8
        self.UINT32_SIZE = 4

        # 结构体偏移：优先取服务端 GetShmLayout 的返回值（C++ 用 offsetof/sizeof 计算，权威），
        # 兜底用本模块常量（与 ShmFrameSlot 一致：seq@0 / meta@64 / payload@128）。
        # 注意 ShmMeta 带 alignas(64)，所以 meta 偏移与 sizeof 都是 64。
        if layout_info:
            self.ALIGNMENT = int(layout_info.get("alignment", SHM_ALIGNMENT))
            self.SLOT_COUNT = int(layout_info.get("slot_count", SHM_SLOT_COUNT))
            self.SEQ_OFFSET = int(layout_info.get("seq_offset", SHM_SEQ_OFFSET))
            self.META_OFFSET = int(layout_info.get("meta_offset", SHM_META_OFFSET))
            self.META_DATA_SIZE = int(layout_info.get("meta_data_size", SHM_META_DATA_SIZE))
            self.PAYLOAD_OFFSET = int(layout_info.get("payload_offset", SHM_PAYLOAD_OFFSET))
        else:
            self.ALIGNMENT = SHM_ALIGNMENT
            self.SLOT_COUNT = SHM_SLOT_COUNT
            self.SEQ_OFFSET = SHM_SEQ_OFFSET
            self.META_OFFSET = SHM_META_OFFSET
            self.META_DATA_SIZE = SHM_META_DATA_SIZE
            self.PAYLOAD_OFFSET = SHM_PAYLOAD_OFFSET
        # meta 在内存中实际占用的对齐后大小
        self.META_STRUCT_SIZE = self.PAYLOAD_OFFSET - self.META_OFFSET

        # 动态字段（slot_size / head_idx_offset / total_size / max_frame_bytes）先用
        # 传入布局或默认布局占位，connect() 时会被 SHM 对象的实际大小覆盖
        self._apply_layout(layout_info if layout_info else
                           _compute_shm_layout(SHM_FALLBACK_MAX_FRAME_BYTES))

        self._mmap_obj: Optional[mmap.mmap] = None
        self._shm_view: Optional[memoryview] = None
        self._last_idx = -1
        self._connected = False
        self._notify_sem: Optional[object] = None
        self._blocking_mode_logged = False
        # 服务端重建/扩容探测状态
        self._identity: Optional[Tuple[int, int]] = None   # (st_dev, st_ino)
        self._last_progress = time.monotonic()              # 最近一次成功取到新帧的时刻
        self._last_probe = 0.0                              # 最近一次身份校验的时刻
        # 通知信号量惰性重试状态
        self._next_sem_attach = 0.0
        self._polling_mode_logged = False
        # 最近读到的这一帧是否花屏（由 meta.frame_flags 解析，逐帧更新）
        self.last_frame_corrupted = False
        # 最近读到的这一帧的原始元数据（含 pix_fmt/w/h/step/ch/depth/flags）
        self.last_frame_meta: Optional[dict] = None

    def _apply_layout(self, layout: dict) -> None:
        """应用布局中的动态部分"""
        self.SLOT_SIZE = int(layout["slot_size"])
        self.HEAD_IDX_OFFSET = int(layout["head_idx_offset"])
        self.TOTAL_SIZE = int(layout["total_size"])
        self.MAX_FRAME_BYTES = int(layout["max_frame_bytes"])

    def _refresh_layout_from_shm_size(self, fd: int) -> bool:
        """以 SHM 对象的实际大小为准修正布局。

        服务端按每路流的分辨率动态分配 SHM 大小（total_size = 3 * slot_size + 8），
        因此绝不能用客户端自己估算的 max_frame_bytes 去计算偏移，否则 slot_size /
        head_idx_offset 会错位（旧实现正是如此，永远读不到帧）。
        """
        try:
            st = os.fstat(fd)
        except OSError as e:
            logger.warning(f"[_ShmReader] fstat 共享内存失败，沿用已有布局: {e}")
            return False

        file_size = st.st_size
        # 记录对象身份，供 _maybe_reconnect 判断服务端是否已重建
        self._identity = (st.st_dev, st.st_ino)

        derived = derive_shm_layout_from_size(file_size, self.PAYLOAD_OFFSET)
        if derived is None:
            logger.error(
                f"[_ShmReader] 共享内存大小 {file_size} 与当前布局公式不匹配"
                f"（期望 total = {self.SLOT_COUNT} * slot_size + {SHM_HEAD_IDX_SIZE}），"
                f"请检查客户端与服务端版本是否一致"
            )
            return False

        if derived["slot_size"] != self.SLOT_SIZE:
            logger.info(
                f"[_ShmReader] 按 SHM 实际大小校正布局: slot_size {self.SLOT_SIZE} -> "
                f"{derived['slot_size']} (max_frame_bytes={derived['max_frame_bytes']}, "
                f"total_size={derived['total_size']})"
            )
        self._apply_layout(derived)
        return True

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def exists(self) -> bool:
        """检查本地是否存在该共享内存文件"""
        return any(os.path.exists(p) for p in self.shm_paths)

    def connect(self) -> bool:
        if self._connected:
            return True
        for path in self.shm_paths:
            if os.path.exists(path):
                try:
                    fd = os.open(path, os.O_RDONLY)
                    # 布局必须来自 SHM 对象本身（服务端按分辨率动态分配大小），
                    # 不能沿用调用方估算的 max_frame_bytes，否则偏移会错位
                    self._refresh_layout_from_shm_size(fd)
                    total_size = self.TOTAL_SIZE
                    self._mmap_obj = mmap.mmap(fd, total_size, prot=mmap.PROT_READ)
                    self._shm_view = memoryview(self._mmap_obj)
                    os.close(fd)
                    # 通知信号量可能比 SHM 文件晚一点出现（服务端创建顺序 / 扩容重建），
                    # 失败时不要永久退化成轮询，read() 里还会按间隔重试
                    self._next_sem_attach = 0.0
                    self._try_attach_notify_sem()
                    self._connected = True
                    # 新对象：head_idx 从 0 重新开始，清空进度/序号状态
                    self._last_idx = -1
                    self._last_progress = time.monotonic()
                    self._last_probe = 0.0
                    logger.debug(f"✓ SHM connected: {path}")
                    return True
                except Exception as e:
                    logger.error(f"✗ SHM connect failed {path}: {e}")
        return False

    def _read_u64(self, offset: int) -> Optional[int]:
        end = offset + self.UINT64_SIZE
        if end > len(self._shm_view):
            return None
        return struct.unpack("Q", self._shm_view[offset:end])[0]

    def _read_meta(self, slot_offset: int) -> Optional[dict]:
        """读取 ShmMeta。

        ShmMeta 布局（与 C++ 端 zero_copy_channel.hpp 一致）：
            QQQQIIIII = 4×u64(actual_size/width/height/timestamp)
                        + 5×u32(channels/depth/step/frame_flags/pixel_format) = 52 字节
        第 9 个 u32（pixel_format）落在原结构体尾部填充区内，
        旧服务端从不写它（内存初始为 0），因此读到的就是 PIXEL_BGR，天然兼容。
        """
        start = slot_offset + self.META_OFFSET

        # 【关键】不管 C++ 填充（Padding）了多少，
        # 我们只读取 struct 解包需要的、没有填充的 52 字节数据
        unpack_format = "QQQQIIIII"
        unpack_size = struct.calcsize(unpack_format)  # 52 字节

        end = start + unpack_size
        if end > len(self._shm_view):
            return None
        raw = self._shm_view[start:end]
        f = struct.unpack(unpack_format, raw)
        return {
            'size': f[0], 'w': f[1], 'h': f[2], 'ts': f[3],
            'ch': f[4], 'depth': f[5], 'step': f[6],
            # 第 8 个 uint32 是帧标志位（帧花屏等），由服务端 ShmMeta.frame_flags 写入
            'flags': f[7], '_rsv': f[7],
            # 第 9 个 uint32 是像素格式（PixelFormat），旧服务端恒为 0 = BGR24
            'pix_fmt': f[8],
        }

    def _rebuild_yuv_frame(self, raw_data, meta: dict) -> Optional[np.ndarray]:
        """按 YUV 格式重建原始帧。

        返回二维 uint8 数组（行 = 一行字节）：
            NV12/I420: (H*3/2, step)，YUYV422: (H, step)
        meta['width']/['height'] 始终是**图像**宽高，与行数不一定相等。
        用 split_yuv_planes() 可以直接拆出各平面。
        """
        w, h = int(meta['w']), int(meta['h'])
        step = int(meta['step'])
        pix_fmt = int(meta.get('pix_fmt', PIXEL_BGR))

        if pix_fmt in (PIXEL_NV12, PIXEL_I420):
            rows = h + (h + 1) // 2
            stride = step if step > 0 else w
        elif pix_fmt == PIXEL_YUYV422:
            rows = h
            stride = step if step > 0 else w * 2
        else:
            logger.error(f"[_ShmReader] 未知像素格式 {pix_fmt}，无法重建 YUV 帧")
            return None

        arr = np.frombuffer(raw_data, dtype=np.uint8)
        need = rows * stride
        if arr.size < need:
            logger.error(
                f"[_ShmReader] YUV 帧数据不足：需要 {need} 字节，实际 {arr.size} "
                f"({PIXEL_FORMAT_NAMES.get(pix_fmt, pix_fmt)} {w}x{h} step={stride})"
            )
            return None
        # 拷贝出来，避免与后续写入的帧共享同一块 mmap（与 BGR 路径行为一致）
        return arr[:need].reshape((rows, stride)).copy()

    def _rebuild_frame(self, raw_data, meta: dict) -> Optional[np.ndarray]:
        pix_fmt = int(meta.get('pix_fmt', PIXEL_BGR))
        if pix_fmt != PIXEL_BGR:
            return self._rebuild_yuv_frame(raw_data, meta)

        w, h, c = meta['w'], meta['h'], meta['ch']
        depth, step = meta['depth'], meta['step']
        dtype = CV_DEPTH_TO_NUMPY.get(depth, np.uint8)
        elem_sz = np.dtype(dtype).itemsize

        arr = np.frombuffer(raw_data, dtype=dtype)
        expected_step = w * c * elem_sz

        if step > 0 and step != expected_step:
            row_elems = step // elem_sz
            if arr.size < h * row_elems:
                return None
            img = arr[:h * row_elems].reshape((h, row_elems))
            img = img[:, :w * c]
        else:
            if arr.size < h * w * c:
                return None
            img = arr[:h * w * c]

        if c == 1:
            img = img.reshape((h, w))
        else:
            img = img.reshape((h, w, c))

        if not img.flags.writeable:
            img = img.copy()
        return img

    def grab(self) -> bool:
        if not self._shm_view:
            return False
        try:
            # 【优化】直接使用 self.HEAD_IDX_OFFSET
            head_off = self.HEAD_IDX_OFFSET
            latest = self._read_u64(head_off)
            if latest is None or latest == self._last_idx:
                return False
            slot_off = (latest % self.SLOT_COUNT) * self.SLOT_SIZE
            v1 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 is None or (v1 & 1):
                return False
            meta = self._read_meta(slot_off)
            if not meta or meta['size'] == 0:
                return False
            v2 = self._read_u64(slot_off + self.SEQ_OFFSET)
            return v1 == v2
        except Exception as e:
            logger.debug(f"grab error: {e}")
            return False

    def retrieve(self) -> Tuple[Optional[np.ndarray], int, int]:
        if not self._shm_view:
            return None, 0, 0
        try:
            # 【优化】直接使用 self.HEAD_IDX_OFFSET
            head_off = self.HEAD_IDX_OFFSET
            latest = self._read_u64(head_off)
            if latest is None or latest == self._last_idx:
                return None, 0, 0
            slot_off = (latest % self.SLOT_COUNT) * self.SLOT_SIZE
            v1 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 is None or (v1 & 1):
                return None, 0, 0
            meta = self._read_meta(slot_off)
            if not meta or meta['size'] == 0 or meta['size'] > self.MAX_FRAME_BYTES:
                return None, 0, 0
            v2 = self._read_u64(slot_off + self.SEQ_OFFSET)
            if v1 != v2:
                return None, 0, 0
            pay_start = slot_off + self.PAYLOAD_OFFSET
            pay_end = pay_start + meta['size']
            if pay_end > len(self._shm_view):
                return None, 0, 0
            raw = self._shm_view[pay_start:pay_end]
            img = self._rebuild_frame(raw, meta)
            if img is None:
                return None, 0, 0
            self._last_idx = latest
            self.last_frame_meta = meta
            return img, meta['ts'], int(meta.get('flags', 0))
        except Exception as e:
            logger.debug(f"retrieve error: {e}")
            return None, 0, 0

    def _try_attach_notify_sem(self) -> bool:
        """惰性打开跨进程通知信号量（失败后按间隔重试）。

        服务端创建 SHM 对象与创建信号量之间存在极短的时间窗（毫秒级）：shm_open
        一执行 SHM 文件就对客户端可见，而客户端会立即 sem_open。若此时信号量还未
        创建就会 ENOENT；信号量在服务端扩容重建时也会被重建。因此不能“一次失败就
        永久退化为轮询”，必须按间隔重试。
        """
        if self._notify_sem is not None:
            return True
        now = time.monotonic()
        if now < self._next_sem_attach:
            return False
        self._next_sem_attach = now + SHM_SEM_ATTACH_RETRY_S
        try:
            self._notify_sem = _NotifySemaphore(f"/{self.stream_id}_notify")
        except Exception as e:
            if not self._polling_mode_logged:
                logger.warning(f"[_ShmReader] 通知信号量暂不可用，先使用轮询模式并定期重试: {e}")
                self._polling_mode_logged = True
            return False
        if not self._blocking_mode_logged:
            logger.info(f"[_ShmReader] Using semaphore blocking mode for {self.stream_id}")
            self._blocking_mode_logged = True
        return True

    def read(self, blocking: bool = False, timeout_ms: Optional[float] = None) -> Tuple[bool, Optional[np.ndarray], int]:
        if not blocking:
            return self._try_read()

        deadline = None if timeout_ms is None else time.monotonic() + timeout_ms / 1000.0
        sleep_ms = 1.0
        while True:
            # 信号量可能尚未创建（服务端 SHM/信号量创建的时间窗）或已被重建，惰性重试
            self._try_attach_notify_sem()

            ok, img, ts = self._try_read()
            if ok:
                return ok, img, ts
            now = time.monotonic()
            if deadline is not None and now >= deadline:
                return False, None, 0
            remaining_ms = None if deadline is None else (deadline - now) * 1000.0

            if self._notify_sem:
                # 分段等待：既尊重调用方超时，也保证能周期性发现服务端重建/扩容
                # （否则旧信号量永远等不到 post，会阻塞死）
                wait_ms = SHM_WAIT_SLICE_MS if remaining_ms is None else min(remaining_ms, SHM_WAIT_SLICE_MS)
                self._notify_sem.wait(wait_ms)  # type: ignore
                continue

            # 无信号量时退化为自适应轮询（同时继续尝试获取信号量）
            if remaining_ms is not None and sleep_ms > remaining_ms:
                sleep_ms = remaining_ms
            if sleep_ms > 0:
                time.sleep(sleep_ms / 1000.0)
            sleep_ms = min(sleep_ms * 1.5, 50.0)

    def _maybe_reconnect(self) -> bool:
        """检测服务端是否已重建/扩容 SHM，并重新连接。

        服务端在帧变大时会 unlink 旧对象并创建新对象（新 inode），旧映射的内容会
        永久冻结在最后一次写入，表现为“一直读不到新帧”。因此在连续一段时间没有
        新帧后校验文件身份，一旦变化就重连（重算布局、重新 mmap、重新打开信号量）。
        """
        now = time.monotonic()
        if now - self._last_progress < SHM_RECONNECT_PROBE_INTERVAL_S:
            return False  # 还在正常收帧，无需探测
        if now - self._last_probe < SHM_RECONNECT_PROBE_INTERVAL_S:
            return False
        self._last_probe = now

        changed = False
        for path in self.shm_paths:
            try:
                st = os.stat(path)
            except OSError:
                continue  # 暂时不存在：服务端可能正处在 unlink -> create 之间
            if self._identity is None or (st.st_dev, st.st_ino) != self._identity:
                logger.warning(f"[_ShmReader] SHM 已被服务端重建，重新连接: {path}")
                changed = True
            elif st.st_size != self.TOTAL_SIZE:
                logger.warning(
                    f"[_ShmReader] SHM 大小已变化 ({self.TOTAL_SIZE} -> {st.st_size})，重新连接: {path}"
                )
                changed = True
            break

        if not changed:
            return False
        self.close()
        return self.connect()

    def _try_read(self) -> Tuple[bool, Optional[np.ndarray], int]:
        # 服务端扩容重建后旧映射会永久冻结，先探测并重连
        self._maybe_reconnect()
        if not self.grab():
            return False, None, 0
        img, ts, flags = self.retrieve()
        if img is not None:
            self._last_progress = time.monotonic()
            self.last_frame_corrupted = bool(flags & SHM_FLAG_CORRUPTED)
        return (img is not None), img, ts

    def close(self):
        self._shm_view = None
        if self._mmap_obj:
            try:
                self._mmap_obj.close()
            except BufferError:
                pass
            self._mmap_obj = None
        if self._notify_sem:
            try:
                self._notify_sem.close() # type: ignore
            except Exception:
                pass
            self._notify_sem = None
        self._connected = False


# ==================== gRPC 重试装饰器 ====================

def _grpc_retry(default_return=None, max_retries: int = 5, backoff_sec: float = 1.0):
    """
    gRPC 调用重试装饰器

    当捕获到 UNAVAILABLE（服务端重启、网络抖动等）时，自动关闭并重建连接后重试。
    重试间隔按指数退避：backoff_sec * (2 ** attempt)。
    注意：服务端重启后原有 stream_id 会失效，业务层需要根据返回状态重新 start_stream。
    """
    def decorator(func):
        def wrapper(self, *args, **kwargs):
            for attempt in range(max_retries + 1):
                if not self._ensure_stub():
                    return default_return
                try:
                    return func(self, *args, **kwargs)
                except grpc.RpcError as e:
                    code = e.code()
                    if code == grpc.StatusCode.UNAVAILABLE and attempt < max_retries:
                        sleep_time = backoff_sec * (2 ** attempt)
                        logger.warning(
                            f"[RTSPClient] gRPC UNAVAILABLE ({func.__name__}), "
                            f"{sleep_time:.1f}s 后尝试重连 ({attempt + 1}/{max_retries})"
                        )
                        time.sleep(sleep_time)
                        if self.reconnect(max_retries=3, backoff_sec=1.0):
                            continue
                        else:
                            logger.error(f"[RTSPClient] {func.__name__} 重连失败")
                            return default_return
                    logger.error(f"[RTSPClient] gRPC error in {func.__name__}: {e.details()}")
                    return default_return
                except Exception as e:
                    logger.error(f"[RTSPClient] unexpected error in {func.__name__}: {e}")
                    return default_return
            return default_return
        return wrapper
    return decorator


def _parse_hik_url(rtsp_url: str) -> Optional[Dict]:
    """
    解析 hik://user:password@ip:port/channel/101 格式的 URL。
    返回 dict 或 None。
    """
    if not rtsp_url.startswith("hik://"):
        return None
    try:
        parsed = urllib.parse.urlparse(rtsp_url)
        path = parsed.path.strip("/")
        if not path.startswith("channel/"):
            return None
        channel = int(path.split("/")[1])
        return {
            "ip": parsed.hostname or "",
            "port": parsed.port or 8000,
            "user": parsed.username or "",
            "password": parsed.password or "",
            "channel": channel,
        }
    except Exception:
        return None


# ==================== gRPC 基类 ====================

class _BaseRTSPClient:
    """gRPC 连接与流生命周期管理（内部基类）"""

    def __init__(self, server_address: str = '127.0.0.1:50052'):
        self.server_address = server_address
        self._channel: Optional[grpc.Channel] = None
        self._stub: Optional[stream_service_pb2_grpc.RTSPStreamServiceStub] = None

    def connect(self, options: Optional[List[tuple]] = None,
                ready_timeout_sec: float = 3.0) -> bool:
        """
        建立 gRPC 连接。

        :param options: 自定义 channel 参数
        :param ready_timeout_sec: 等待 channel ready 的最长时间（秒）；<=0 表示不等待。
            默认 3 秒：否则即使地址不可达也会“连接成功”，直到后续 RPC 才报 UNAVAILABLE，
            容易让人误以为地址是对的。
        """
        try:
            opts = options if options is not None else _DEFAULT_CHANNEL_OPTIONS
            self._channel = grpc.insecure_channel(self.server_address, options=opts)
            self._stub = stream_service_pb2_grpc.RTSPStreamServiceStub(self._channel)
        except Exception as e:
            logger.error(f"连接服务器失败: {e}")
            return False

        if ready_timeout_sec and ready_timeout_sec > 0:
            try:
                grpc.channel_ready_future(self._channel).result(timeout=ready_timeout_sec)
            except Exception as e:  # 包括 FutureTimeoutError
                logger.error(
                    f"连接服务器失败: {self.server_address} 不可达 ({ready_timeout_sec:.0f}s 超时: {type(e).__name__})。\n"
                    f"        请检查地址/端口是否正确、服务端是否在运行；\n"
                    f"        容器部署常见映射为 -p 50052:50051（宿主 50052 → 容器 50051），同机可试 127.0.0.1:50052"
                )
                with contextlib.suppress(Exception):
                    self._channel.close()
                self._channel = None
                self._stub = None
                return False

        logger.info(f"已连接到服务器: {self.server_address}")
        return True

    def disconnect(self):
        if self._channel:
            self._channel.close()
            self._channel = None
            self._stub = None
            logger.info("已断开服务器连接")

    def is_connected(self) -> bool:
        return self._stub is not None

    def reconnect(self, max_retries: int = 3, backoff_sec: float = 1.0,
                  ready_timeout_sec: float = 5.0) -> bool:
        """
        关闭当前连接并重新建立，用于服务端重启后恢复通信

        :param max_retries: 最大重试次数
        :param backoff_sec: 首次重试等待秒数，后续按指数退避
        :param ready_timeout_sec: 等待 gRPC channel ready 的最大时间
        """
        logger.info("正在尝试重新连接服务器...")
        for attempt in range(max_retries + 1):
            try:
                self.disconnect()
                # 这里不需要 connect() 再做 ready 等待，下面会统一等待
                if not self.connect(ready_timeout_sec=0):
                    raise RuntimeError("connect() returned False")

                # 等待 channel 真正可用，避免服务端刚启动时立即调用失败
                try:
                    grpc.channel_ready_future(self._channel).result(timeout=ready_timeout_sec)
                    logger.info("重新连接服务器成功")
                    return True
                except grpc.FutureTimeoutError:
                    logger.warning(f"等待 gRPC channel ready 超时 ({ready_timeout_sec}s)")
            except Exception as e:
                logger.error(f"重新连接失败: {e}")

            if attempt < max_retries:
                sleep_time = backoff_sec * (2 ** attempt)
                logger.warning(f"{sleep_time:.1f}s 后再次尝试重连 ({attempt + 1}/{max_retries})...")
                time.sleep(sleep_time)

        logger.error(f"重试 {max_retries} 次后仍未连接成功")
        return False

    def _ensure_stub(self) -> bool:
        if self._stub is not None:
            return True
        return self.connect()

    @_grpc_retry(default_return=None)
    def get_shm_layout(self) -> Optional[dict]:
        """从服务端获取共享内存布局信息（含 C++ offsetof/sizeof 算出的结构体偏移）；旧版服务端未实现则返回 None。

        注意：服务端按每路流的实际分辨率动态分配 SHM 大小，因此该接口返回的
        slot_size / head_idx_offset / total_size 是**默认布局**，不可直接用于某一路流；
        真正读取某一路流时：
          - 结构体偏移（seq/meta/payload）用这里的返回值；
          - 动态尺寸（slot_size/head_idx_offset/total_size）由 SHM 对象实际大小反推。
        """
        if not self._ensure_stub():
            return None
        try:
            req = stream_service_pb2.ShmLayoutRequest()
            resp = self._stub.GetShmLayout(req, timeout=5)
            if resp.success:
                layout = resp.layout
                return {
                    "slot_count": layout.slot_count,
                    "max_frame_bytes": layout.max_frame_bytes,
                    "alignment": layout.alignment,
                    "slot_size": layout.slot_size,
                    "seq_offset": layout.seq_offset,
                    "meta_offset": layout.meta_offset,
                    "payload_offset": layout.payload_offset,
                    "meta_data_size": layout.meta_data_size,
                    "head_idx_offset": layout.head_idx_offset,
                    "total_size": layout.total_size,
                }
            logger.warning(f"服务端返回 GetShmLayout 失败: {resp.message}")
        except grpc.RpcError as e:
            if e.code() == grpc.StatusCode.UNIMPLEMENTED:
                logger.debug("服务端未实现 GetShmLayout，SHM 使用客户端兜底布局")
            else:
                logger.error(f"获取 SHM 布局失败: {e.details()}")
        except Exception as e:
            # 例如生成的 pb2 桩过旧（没有 ShmLayoutRequest）→ 回退到客户端兜底常量
            logger.warning(f"获取 SHM 布局不可用，改用客户端兜底布局: {e}")
        return None

    def start_stream(self,
                     rtsp_url: str,
                     heartbeat_timeout_ms: int = 100000,
                     decode_interval_ms: int = 0,
                     decoder_type: int = DECODER_CPU_FFMPEG,
                     gpu_id: int = 0,
                     keep_on_failure: bool = False,
                     use_shared_mem: bool = False,
                     only_key_frames: bool = False,
                     pixel_format: int = PIXEL_BGR) -> Optional[str]:
        """启动流。

        :param pixel_format: 共享内存原始帧的像素格式（PIXEL_BGR/PIXEL_NV12/PIXEL_I420/
            PIXEL_YUYV422）。仅在 use_shared_mem=True 时生效；未开启 SHM 时服务端忽略该参数
            （gRPC 通道始终返回 JPEG）。服务端会在首帧解码后就按该格式出帧，
            客户端用 split_yuv_planes() 可把 YUV 帧拆成各平面。
        """
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return None

        try:
            # 海康模式：URL 是 hik://... 或显式指定 HIK_SDK 时触发
            is_hik_url = rtsp_url.startswith("hik://")
            is_hik_mode = is_hik_url or (decoder_type == DECODER_HIK_SDK)

            extra_info = ""
            if is_hik_mode:
                parsed = _parse_hik_url(rtsp_url)
                if parsed:
                    extra_info = f", HIK: {parsed['user']}@{parsed['ip']}:{parsed['port']}/ch{parsed['channel']}"
            logger.info(
                f"正在启动流: {rtsp_url} "
                f"(解码器: {DECODER_NAMES.get(decoder_type, 'Unknown')}, "
                f"GPU ID: {gpu_id}, SHM: {use_shared_mem}, "
                f"Only Key Frames: {'Yes' if only_key_frames else 'No'}{extra_info})"
            )
            # 只有非 GPU 解码且非海康模式时才把 gpu_id 重置为 -1；
            # 海康模式下保留 gpu_id，便于服务端保存供后续切回 RTSP 使用
            if decoder_type != DECODER_GPU_NVCUVID and not is_hik_mode:
                gpu_id = -1

            req = stream_service_pb2.StartRequest(
                rtsp_url=rtsp_url,
                heartbeat_timeout_ms=heartbeat_timeout_ms,
                decode_interval_ms=decode_interval_ms,
                decoder_type=decoder_type,
                gpu_id=gpu_id,
                keep_on_failure=keep_on_failure,
                use_shared_mem=use_shared_mem,
                only_key_frames=only_key_frames,
                pixel_format=pixel_format
            )
            resp = self._stub.StartStream(req, timeout=10)

            if resp.success:
                logger.info(f"流启动成功: {resp.stream_id} -> {rtsp_url}")
                # 服务端会在“解码器被降级（CPU-only 构建 / GPU 不可用）”或
                # “复用了同 URL 的已有流（配置与本次请求不同）”时给出说明，
                # 直接显示出来，避免出现“选了 NVCUVID 却在用 CPU”这类难自查的问题。
                server_msg = getattr(resp, "message", "")
                if server_msg and server_msg != "Stream started successfully":
                    logger.warning(f"[RTSPClient] 服务端提示: {server_msg}")
                return resp.stream_id
            else:
                logger.error(f"流启动失败: {resp.message}")
                return None
        except grpc.RpcError as e:
            logger.error(f"启动流异常: {e.details()}")
            return None

    @_grpc_retry(default_return=False)
    def stop_stream(self, stream_id: str) -> bool:
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return False
        req = stream_service_pb2.StopRequest(stream_id=stream_id)
        resp = self._stub.StopStream(req, timeout=5)
        if resp.success:
            logger.info(f"流已停止: {stream_id}")
        else:
            logger.warning(f"停止流失败: {resp.message}")
        return resp.success

    @_grpc_retry(default_return=False)
    def update_stream_url(self, stream_id: str, new_rtsp_url: str) -> bool:
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return False
        req = stream_service_pb2.UpdateStreamRequest(
            stream_id=stream_id,
            new_rtsp_url=new_rtsp_url,
        )
        resp = self._stub.UpdateStream(req, timeout=5)
        if resp.success:
            logger.info(f"流 URL 已更新: {stream_id} -> {new_rtsp_url}")
        else:
            logger.warning(f"更新流 URL 失败: {resp.message}")
        return resp.success

    def update_stream_url_isolated(self, stream_id: str, new_rtsp_url: str) -> bool:
        with grpc.insecure_channel(self.server_address) as channel:
            stub = stream_service_pb2_grpc.RTSPStreamServiceStub(channel)
            try:
                req = stream_service_pb2.UpdateStreamRequest(
                    stream_id=stream_id,
                    new_rtsp_url=new_rtsp_url,
                )
                resp = stub.UpdateStream(req, timeout=5)
                if resp.success:
                    logger.info(f"流 URL 已更新: {stream_id} -> {new_rtsp_url}")
                else:
                    logger.warning(f"更新流 URL 失败: {resp.message}")
                return resp.success
            except grpc.RpcError as e:
                logger.error(f"更新流 URL 异常: {e.details()}")
                return False

    @_grpc_retry(default_return=[])
    def list_streams(self) -> List[Dict]:
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return []
        req = stream_service_pb2.ListStreamsRequest()
        resp = self._stub.ListStreams(req, timeout=5)
        streams = []
        for s in resp.streams:
            streams.append({
                "stream_id": s.stream_id,
                "rtsp_url": s.rtsp_url,
                "status": s.status,
                "status_name": STATUS_NAMES.get(s.status, "未知"),
                "decoder_type": DECODER_NAMES.get(s.decoder_type, "Unknown"),
                "decoder_type_raw": s.decoder_type,
                "width": s.width,
                "height": s.height,
                "decode_interval_ms": s.decode_interval_ms,
                "heartbeat_timeout_ms": s.heartbeat_timeout_ms,
                "keep_on_failure": s.keep_on_failure,
                "only_key_frames": s.only_key_frames,
                "use_shared_mem": s.use_shared_mem,
                "pixel_format": getattr(s, "pixel_format", PIXEL_BGR),
                "pixel_format_name": PIXEL_FORMAT_NAMES.get(getattr(s, "pixel_format", PIXEL_BGR), "BGR24"),
                "fps": getattr(s, "fps", 0.0),
                "media_lag_ms": getattr(s, "media_lag_ms", 0),
                "corrupted_frames": getattr(s, "corrupted_frames", 0),
                "glitch_ratio": getattr(s, "glitch_ratio", 0.0)
            })
        return streams

    @_grpc_retry(default_return=-1)
    def get_stream_count(self) -> int:
        if not self._ensure_stub():
            return -1
        req = stream_service_pb2.ListStreamsRequest()
        resp = self._stub.ListStreams(req, timeout=5)
        return resp.total_count

    @_grpc_retry(default_return=None)
    def check_stream(self, stream_id: str) -> Optional[Dict]:
        if not self._ensure_stub():
            return None
        req = stream_service_pb2.CheckRequest(stream_id=stream_id)
        resp = self._stub.CheckStream(req, timeout=5)
        s = resp.stream
        return {
            "stream_id": s.stream_id or stream_id,
            "rtsp_url": s.rtsp_url,
            "status": s.status,
            "status_name": STATUS_NAMES.get(s.status, "未知"),
            "decoder_type": DECODER_NAMES.get(s.decoder_type, "Unknown"),
            "decoder_type_raw": s.decoder_type,
            "width": s.width,
            "height": s.height,
            "decode_interval_ms": s.decode_interval_ms,
            "heartbeat_timeout_ms": s.heartbeat_timeout_ms,
            "keep_on_failure": s.keep_on_failure,
            "only_key_frames": s.only_key_frames,
            "use_shared_mem": s.use_shared_mem,
            "pixel_format": getattr(s, "pixel_format", PIXEL_BGR),
            "pixel_format_name": PIXEL_FORMAT_NAMES.get(getattr(s, "pixel_format", PIXEL_BGR), "BGR24"),
            "fps": getattr(s, "fps", 0.0),
            "media_lag_ms": getattr(s, "media_lag_ms", 0),
            "corrupted_frames": getattr(s, "corrupted_frames", 0),
            "glitch_ratio": getattr(s, "glitch_ratio", 0.0),
            "server_message": resp.message
        }

    def check_stream_exists(self, stream_id: str) -> bool:
        info = self.check_stream(stream_id)
        if info is None:
            return False
        return info.get("status", STATUS_NOT_FOUND) != STATUS_NOT_FOUND

    def is_stream_connected(self, stream_id: str) -> bool:
        info = self.check_stream(stream_id)
        return info is not None and info.get("status") == STATUS_CONNECTED

    def get_stream_status(self, stream_id: str) -> int:
        info = self.check_stream(stream_id)
        return info.get("status", STATUS_NOT_FOUND) if info else STATUS_NOT_FOUND

    def get_stream_status_name(self, stream_id: str) -> str:
        return STATUS_NAMES.get(self.get_stream_status(stream_id), "未知")

    def __enter__(self):
        if not self.connect():
            raise RuntimeError(f"无法连接到服务器: {self.server_address}")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()


# ==================== 统一客户端 ====================

class RTSPClient(_BaseRTSPClient):
    """
    统一 RTSP 客户端：支持 gRPC JPEG 与共享内存两种帧获取方式

    通过 start_stream(..., use_shared_mem=...) 选择路径：
      - False -> read(stream_id) 走 gRPC JPEG，返回 (timestamp, frame)
      - True  -> read(stream_id) 走本地共享内存，返回 (timestamp, frame)

    注：服务端 gRPC 字段名为 frame_seq，但实际下发的是时间戳（毫秒），
    因此两种模式统一理解为“帧时间戳”。

    自动重连：gRPC 调用在捕获 UNAVAILABLE 时会自动关闭并重建连接后重试一次。
    自动重启流：服务端重启导致 stream_id 失效时，会用 start_stream 时的相同参数
    自动重新启动流，并继续读取。对业务层透明。

    若 use_shared_mem=True 但本地不存在 /dev/shm/<stream_id>，
    read() 会报错并返回 (-1, None)（说明客户端与服务端不在同一机器或 SHM 创建失败）。
    """

    def __init__(self, server_address: str = '127.0.0.1:50051'):
        super().__init__(server_address)
        self._shm_readers: Dict[str, _ShmReader] = {}
        self._stream_modes: Dict[str, bool] = {}      # stream_id -> use_shared_mem 缓存
        self._stream_params: Dict[str, dict] = {}      # original_stream_id -> 启动参数
        self._stream_id_map: Dict[str, str] = {}       # original_stream_id -> current_stream_id
        self._shm_layout: Optional[dict] = None        # 服务端 GetShmLayout 缓存（结构体偏移）
        self._shm_missing_log_at: Dict[str, float] = {}   # 抑制“共享内存不可见”告警刷屏
        # 注册进程退出兜底清理：避免客户端异常退出后 mmap 长期占用 tmpfs 空间
        atexit.register(_cleanup_client_on_exit, weakref.ref(self))

    def disconnect(self):
        # 清理所有 SHM 读取器
        for reader in self._shm_readers.values():
            try:
                reader.close()
            except Exception:
                pass
        self._shm_readers.clear()
        self._stream_modes.clear()
        # 注意：_stream_params 和 _stream_id_map 在 reconnect 后仍需保留，
        # 以便服务端重启时能够自动重新启动流。只有 stop_stream / _cleanup_stream_cache
        # 才应该显式清理这些缓存。
        super().disconnect()

    def _cleanup_stream_cache(self, stream_id: str):
        """清理某一路流的内部缓存"""
        self._stream_params.pop(stream_id, None)
        self._stream_id_map.pop(stream_id, None)
        self._stream_modes.pop(stream_id, None)
        reader = self._shm_readers.pop(stream_id, None)
        if reader:
            try:
                reader.close()
            except Exception:
                pass

    def start_stream(self,
                     rtsp_url: str,
                     heartbeat_timeout_ms: int = 100000,
                     decode_interval_ms: int = 0,
                     decoder_type: int = DECODER_CPU_FFMPEG,
                     gpu_id: int = 0,
                     keep_on_failure: bool = False,
                     use_shared_mem: bool = False,
                     only_key_frames: bool = False,
                     pixel_format: int = PIXEL_BGR) -> Optional[str]:
        """启动流并缓存启动参数，用于服务端重启后的自动恢复"""
        stream_id = super().start_stream(
            rtsp_url=rtsp_url,
            heartbeat_timeout_ms=heartbeat_timeout_ms,
            decode_interval_ms=decode_interval_ms,
            decoder_type=decoder_type,
            gpu_id=gpu_id,
            keep_on_failure=keep_on_failure,
            use_shared_mem=use_shared_mem,
            only_key_frames=only_key_frames,
            pixel_format=pixel_format
        )
        if stream_id:
            self._stream_params[stream_id] = {
                "rtsp_url": rtsp_url,
                "heartbeat_timeout_ms": heartbeat_timeout_ms,
                "decode_interval_ms": decode_interval_ms,
                "decoder_type": decoder_type,
                "gpu_id": gpu_id,
                "keep_on_failure": keep_on_failure,
                "use_shared_mem": use_shared_mem,
                "only_key_frames": only_key_frames,
                "pixel_format": pixel_format,
            }
            self._stream_id_map[stream_id] = stream_id
        return stream_id

    def stop_stream(self, stream_id: str) -> bool:
        """停止流并清理内部缓存"""
        current_id = self._stream_id_map.get(stream_id, stream_id)
        result = super().stop_stream(current_id)
        self._cleanup_stream_cache(stream_id)
        return result

    def _restart_stream_if_needed(self, stream_id: str) -> Optional[str]:
        """
        如果 stream_id 已失效，用缓存的相同参数重新启动。
        返回当前有效的 stream_id；无法重启则返回 None。
        """
        params = self._stream_params.get(stream_id)
        if not params:
            logger.debug(f"[RTSPClient] 无缓存参数，无法自动重启: {stream_id}")
            return stream_id  # 没有缓存参数，无法自动重启，返回原 ID

        current_id = self._stream_id_map.get(stream_id, stream_id)
        logger.debug(
            f"[RTSPClient] _restart_stream_if_needed: original={stream_id}, "
            f"current_id={current_id}"
        )
        status_info = self.check_stream(current_id)
        fresh_id = self._stream_id_map.get(stream_id, stream_id)

        # check_stream 可能触发重连，_stream_id_map 在此过程中被其他路径更新，
        # 需要以最新映射为准重新查询状态。
        if fresh_id != current_id:
            logger.info(
                f"[RTSPClient] check_stream 期间流映射发生变化: "
                f"{stream_id}: {current_id} -> {fresh_id}，重新查询"
            )
            current_id = fresh_id
            status_info = self.check_stream(current_id)

        # 流仍有效，直接返回当前 ID
        if status_info is not None and status_info.get("status") != STATUS_NOT_FOUND:
            logger.debug(
                f"[RTSPClient] 流仍有效: original={stream_id}, current_id={current_id}, "
                f"status={status_info.get('status_name')}"
            )
            return current_id

        logger.warning(
            f"[RTSPClient] 流 {stream_id} (current_id={current_id}) 已失效，"
            f"尝试用相同参数重新启动..."
        )
        new_id = super().start_stream(**params)
        if not new_id:
            logger.error(f"[RTSPClient] 流 {stream_id} 重启失败")
            return None

        # 防止重连/并发路径已经重启了流，导致同一原始 ID 对应多个服务端流
        existing_id = self._stream_id_map.get(stream_id)
        if existing_id and existing_id != stream_id and existing_id != new_id:
            logger.warning(
                f"[RTSPClient] 流 {stream_id} 在启动过程中已被其他路径重启为 {existing_id}，"
                f"将停止冗余流 {new_id}"
            )
            try:
                super().stop_stream(new_id)
            except Exception:
                pass
            chosen_id = existing_id
        else:
            chosen_id = new_id
            self._stream_id_map[stream_id] = new_id

        logger.info(f"[RTSPClient] 流已重启: {stream_id} -> {chosen_id}")
        # 清理旧 SHM reader（如果存在）
        for old_id in (stream_id, current_id):
            old_reader = self._shm_readers.pop(old_id, None)
            if old_reader:
                try:
                    old_reader.close()
                except Exception:
                    pass
        # 清除模式缓存，下次 read 会重新查询
        self._stream_modes.pop(stream_id, None)
        self._stream_modes.pop(current_id, None)
        return chosen_id

    def _get_current_stream_id(self, stream_id: str) -> Optional[str]:
        """获取当前有效的 stream_id，必要时自动重启"""
        result = self._restart_stream_if_needed(stream_id)
        logger.debug(
            f"[RTSPClient] _get_current_stream_id: original={stream_id}, result={result}"
        )
        return result

    def get_active_stream_id(self, stream_id: str) -> Optional[str]:
        """
        获取指定原始 stream_id 当前对应的有效 stream_id。
        如果服务端已重启并自动重启了流，返回的是新的 stream_id；否则返回原 ID。
        """
        return self._stream_id_map.get(stream_id, stream_id)

    def check_stream(self, stream_id: str) -> Optional[Dict]:
        """查询单个流状态时自动映射到当前有效的 stream_id"""
        current_id = self._stream_id_map.get(stream_id, stream_id)
        return super().check_stream(current_id)

    def is_stream_connected(self, stream_id: str) -> bool:
        info = self.check_stream(stream_id)
        return info is not None and info.get("status") == STATUS_CONNECTED

    def check_stream_exists(self, stream_id: str) -> bool:
        info = self.check_stream(stream_id)
        if info is None:
            return False
        return info.get("status", STATUS_NOT_FOUND) != STATUS_NOT_FOUND

    def get_stream_status(self, stream_id: str) -> int:
        info = self.check_stream(stream_id)
        return info.get("status", STATUS_NOT_FOUND) if info else STATUS_NOT_FOUND

    def get_stream_status_name(self, stream_id: str) -> str:
        return STATUS_NAMES.get(self.get_stream_status(stream_id), "未知")

    def update_stream_url(self, stream_id: str, new_rtsp_url: str) -> bool:
        """更新当前有效流的 RTSP URL"""
        current_id = self._stream_id_map.get(stream_id, stream_id)
        result = super().update_stream_url(current_id, new_rtsp_url)
        # URL 改变后清空参数缓存，防止后续自动重启仍使用旧 URL
        if result:
            self._stream_params.pop(stream_id, None)
        return result

    @staticmethod
    def _decode_jpeg(jpeg_data: bytes) -> Optional[np.ndarray]:
        if not jpeg_data:
            return None
        if not _HAS_TURBOJPEG:
            # 降级到 OpenCV
            try:
                img_array = np.frombuffer(jpeg_data, dtype=np.uint8)
                return cv2.imdecode(img_array, cv2.IMREAD_COLOR)
            except Exception as e:
                logger.error(f"OpenCV JPEG 解码失败: {e}")
                return None
        try:
            img = turbojpeg.decompress(jpeg_data, pixelformat=turbojpeg.BGR)
            return np.array(img)
        except Exception as e:
            logger.error(f"TurboJPEG 解码失败: {e}")
            return None

    def _get_shm_reader(self, stream_id: str) -> Optional[_ShmReader]:
        """获取或创建指定流的 SHM 读取器；若本地无 SHM 则返回 None 并记录错误"""
        reader = self._shm_readers.get(stream_id)
        if reader is not None:
            if not reader.exists():
                # 服务端扩容时会 unlink 旧对象再创建新对象，存在极短的窗口期；
                # 也可能是流已停止/服务端重启，下一次调用会自动重试。
                # 预览会高频轮询，这里按流节流，避免刷屏。
                now = time.monotonic()
                if now - self._shm_missing_log_at.get(stream_id, 0.0) >= 5.0:
                    self._shm_missing_log_at[stream_id] = now
                    logger.warning(
                        f"[RTSPClient] 共享内存当前不可见（可能正在扩容重建、流已停止或未启动 SHM）: "
                        f"/dev/shm/{stream_id}，请确认客户端与服务端在同一主机且 Docker 挂载了 -v /dev/shm:/dev/shm"
                    )
                return None
            return reader

        # 不再由客户端推算布局：服务端会按每路流的实际分辨率动态分配 SHM 大小，
        # 客户端过去用 width*height*3*1.25 猜测 max_frame_bytes，与服务端实际布局不一致，
        # 导致 slot_size / head_idx_offset 错位、mmap 长度超过文件大小而永远读不到帧。
        # 真实布局在 _ShmReader.connect() 中由 SHM 对象的实际大小反推得到。
        #
        # 结构体偏移（seq/meta/payload）则以服务端 GetShmLayout 为准：ShmMeta 带
        # alignas(64)，客户端“按直觉”硬编码会在 meta 偏移上出错（读到全 0 → 丢弃所有帧）。
        if self._shm_layout is None:
            self._shm_layout = self.get_shm_layout()  # 失败返回 None，此时用客户端兜底常量
        reader = _ShmReader(stream_id, layout_info=self._shm_layout)
        if not reader.exists():
            logger.error(
                f"[RTSPClient] 未找到共享内存: /dev/shm/{stream_id}。"
                f"可能原因：1) 客户端与服务端不在同一机器；2) 服务端 SHM 创建失败；"
                f"3) 该流未启用共享内存；4) Docker 未挂载 -v /dev/shm:/dev/shm。"
            )
            return None

        if not reader.connect():
            logger.error(f"[RTSPClient] 共享内存连接失败: /dev/shm/{stream_id}")
            return None

        self._shm_readers[stream_id] = reader
        return reader

    def _stream_uses_shm(self, stream_id: str) -> bool:
        """查询服务端确认该流是否启用了共享内存（结果缓存，避免每次 read 都查）"""
        if stream_id in self._stream_modes:
            return self._stream_modes[stream_id]
        info = self.check_stream(stream_id)
        uses = info is not None and info.get("use_shared_mem", False)
        self._stream_modes[stream_id] = uses
        return uses

    def get_last_frame_meta(self, stream_id: str) -> Optional[dict]:
        """返回**最近一次 read() 读到的 SHM 帧**的原始元数据。

        键：size/w/h/ts/ch/depth/step/flags/pix_fmt。
        gRPC JPEG 模式或尚未读到过 SHM 帧时返回 None。
        用途：判断当前帧的实际像素格式（pix_fmt），据此调用 split_yuv_planes()
        拆平面或转换为 BGR（见 web/server.py）。
        """
        current_id = self._stream_id_map.get(stream_id, stream_id)
        for sid in (current_id, stream_id):
            reader = self._shm_readers.get(sid)
            if reader is not None and reader.last_frame_meta is not None:
                return reader.last_frame_meta
        return None

    def read(self, stream_id: str, blocking: bool = False, timeout_ms: Optional[float] = None) -> Tuple[int, Optional[np.ndarray]]:
        """
        获取指定流的最新帧，自动根据 use_shared_mem 选择路径

        特性：
          - gRPC JPEG 路径在服务端 UNAVAILABLE 时会自动重连并重试（指数退避）
          - 服务端重启导致 stream_id 失效时，会用缓存参数自动重启流并继续读取

        :param stream_id: 流 ID（start_stream 返回的原始 ID）
        :param blocking: 仅对 SHM 模式有效，是否阻塞等待新帧
        :param timeout_ms: 仅对 SHM 模式有效，阻塞超时（毫秒）
        :return: (帧时间戳, 图像帧)。失败返回 (-1, None)

        需要知道“这一帧是否花屏”时请用 read_ex()。
        """
        ts, img, _corrupted = self.read_ex(stream_id, blocking=blocking, timeout_ms=timeout_ms)
        return ts, img

    def read_ex(self, stream_id: str, blocking: bool = False,
                timeout_ms: Optional[float] = None) -> Tuple[int, Optional[np.ndarray], bool]:
        """
        同 read()，但额外返回**该帧本身是否花屏**（解码器自报）。

        :return: (帧时间戳, 图像帧, 是否花屏)；失败返回 (-1, None, False)

        说明：
          - 花屏判定完全基于解码器的硬信号（缺参考帧 / 错误掩盖 / 码流非法），
            不依赖像素猜测，因此不会因为夜间红外、低照度、纯色场景而误判；
          - SHM 模式从帧元数据的 flags 位读取，gRPC JPEG 模式从 FrameResponse.corrupted 读取；
          - 该标记是“逐帧”的：只说明你拿到的这一帧，而不是整条流的健康状况
            （整条流的统计见 StreamInfo.corrupted_frames / glitch_ratio）。
        """
        if not self._ensure_stub():
            return -1, None, False

        # 确保流有效（服务端重启时会自动用相同参数重新启动）
        current_id = self._get_current_stream_id(stream_id)
        logger.debug(
            f"[RTSPClient] read: original={stream_id}, initial_current_id={current_id}"
        )
        if not current_id:
            return -1, None, False

        # 优先按服务端配置决定路径
        if self._stream_uses_shm(current_id):
            # _stream_uses_shm 内部可能触发重连/重启，刷新当前有效 id
            current_id = self._get_current_stream_id(stream_id)
            if not current_id:
                return -1, None, False
            reader = self._get_shm_reader(current_id)
            if reader is None:
                return -1, None, False
            ok, img, ts = reader.read(blocking=blocking, timeout_ms=timeout_ms)
            if ok and img is not None:
                return int(ts), img, bool(getattr(reader, "last_frame_corrupted", False))
            return -1, None, False

        # gRPC JPEG 路径（带 UNAVAILABLE 自动重连，指数退避）
        max_retries = 3
        for attempt in range(max_retries + 1):
            # 每次尝试前刷新当前有效的 stream_id，防止期间发生二次重启
            current_id = self._get_current_stream_id(stream_id)
            if not current_id:
                return -1, None, False

            try:
                req = stream_service_pb2.FrameRequest(stream_id=current_id)
                resp = self._stub.GetLatestFrame(req, timeout=5)
                frame_seq = getattr(resp, "frame_seq", -1)
                corrupted = bool(getattr(resp, "corrupted", False))
                if resp.success and resp.image_data:
                    img = self._decode_jpeg(resp.image_data)
                    return frame_seq, img, corrupted
                # 记录无帧原因，便于诊断（区分“流不存在/已过期”与“已连接但暂无帧”）
                logger.info(
                    f"[RTSPClient] GetLatestFrame 无帧: "
                    f"stream_id={current_id}, success={resp.success}, "
                    f"has_data={bool(resp.image_data)}, frame_seq={frame_seq}, "
                    f"message={resp.message!r}"
                )
                return frame_seq, None, False
            except grpc.RpcError as e:
                if e.code() == grpc.StatusCode.UNAVAILABLE and attempt < max_retries:
                    sleep_time = 1.0 * (2 ** attempt)
                    logger.warning(f"[RTSPClient] gRPC 读取遇到 UNAVAILABLE，{sleep_time:.1f}s 后尝试重连")
                    time.sleep(sleep_time)
                    if not self.reconnect():
                        break
                    continue
                logger.error(f"[RTSPClient] gRPC 读取失败: {e.details()}")
                return -1, None, False
            except Exception as e:
                logger.error(f"[RTSPClient] gRPC 读取失败: {e}")
                return -1, None, False
        return -1, None, False

    def stream_frames(self, stream_id: str, max_fps: int = 0) -> Generator[Tuple[int, Optional[np.ndarray]], None, None]:
        """
        流式获取视频帧（仅 gRPC JPEG 模式支持生成器；SHM 模式会提示并降级为空）

        服务端重启导致流失效时，生成器会结束，业务层需要重新调用 start_stream + stream_frames。

        需要逐帧花屏标记时请用 stream_frames_ex()。
        """
        for ts, img, _corrupted in self.stream_frames_ex(stream_id, max_fps=max_fps):
            yield (ts, img)

    def stream_frames_ex(self, stream_id: str,
                         max_fps: int = 0) -> Generator[Tuple[int, Optional[np.ndarray], bool], None, None]:
        """
        同 stream_frames()，但额外 yield **该帧本身是否花屏**（解码器自报）。

        :return: 生成器，逐帧产出 (帧时间戳, 图像帧, 是否花屏)
        """
        if not self._ensure_stub():
            logger.error("未连接到服务器")
            return

        # 确保流有效
        current_id = self._get_current_stream_id(stream_id)
        if not current_id:
            logger.error("[RTSPClient] 流无效且无法自动重启")
            return

        if self._stream_uses_shm(current_id):
            logger.error(
                f"[RTSPClient] stream_frames 不支持共享内存模式，"
                f"请使用 read_ex(stream_id, blocking=True)。"
            )
            return

        try:
            req = stream_service_pb2.StreamRequest(stream_id=current_id, max_fps=max_fps)
            for resp in self._stub.StreamFrames(req):
                if resp.success and resp.image_data and resp.frame_seq != -1:
                    img = self._decode_jpeg(resp.image_data)
                    yield (resp.frame_seq, img, bool(getattr(resp, "corrupted", False)))
                else:
                    yield (-1, None, False)
        except grpc.RpcError as e:
            logger.error(f"流式读取异常: {e.details()}")
        except Exception as e:
            logger.error(f"流式读取异常: {e}")


# ==================== 使用示例 ====================
if __name__ == "__main__":
    server = '127.0.0.1:50051'
    rtsp_url = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/101"

    def wait_connected(client, stream_id, timeout_sec=10):
        for _ in range(timeout_sec * 5):
            status = client.get_stream_status(stream_id)
            if status == STATUS_CONNECTED:
                return True
            if status in (STATUS_DISCONNECTED, STATUS_NOT_FOUND):
                return False
            time.sleep(0.2)
        return False

    # 示例1: gRPC JPEG 模式
    with RTSPClient(server) as client:
        sid = client.start_stream(rtsp_url, use_shared_mem=False)
        if sid and wait_connected(client, sid):
            ts, frame = client.read(sid)
            print(f"JPEG 模式读取: ts={ts}, shape={frame.shape if frame is not None else None}")
            client.stop_stream(sid)
        else:
            print("JPEG 模式：流未成功连接")

    # 示例2: 共享内存模式
    with RTSPClient(server) as client:
        sid = client.start_stream(rtsp_url, use_shared_mem=True)
        if sid and wait_connected(client, sid):
            ts, frame = client.read(sid, blocking=True, timeout_ms=1000)
            print(f"SHM 模式读取: ts={ts}, shape={frame.shape if frame is not None else None}")
            client.stop_stream(sid)
        else:
            print("SHM 模式：流未成功连接")
