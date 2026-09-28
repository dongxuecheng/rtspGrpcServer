#!/usr/bin/env python3
"""
批量启动 RTSP 流，全部使用 GPU (NVCUVID) 解码。

用法示例:
    # 默认读取当前目录下的 rtsp_list.txt，本机 50051 端口，GPU 0
    python start_all_streams.py

    # 指定服务器地址、GPU ID、并发数和列表文件
    python start_all_streams.py \
        --server 192.168.1.10:50051 \
        --gpu-id 0 \
        --workers 20 \
        --list rtsp_list.txt

    # 只启动关键帧，降低 GPU 压力
    python start_all_streams.py --only-key-frames
"""

import argparse
import logging
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Tuple

from remote_capture import RTSPClient, DECODER_GPU_NVCUVID

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="读取 rtsp_list.txt 并使用 GPU 解码批量打开所有流"
    )
    parser.add_argument(
        "--server",
        default="127.0.0.1:50052",
        help="gRPC 服务器地址 (默认: 127.0.0.1:50052)",
    )
    parser.add_argument(
        "--list",
        default="rtsp_list.txt",
        dest="list_file",
        help="RTSP URL 列表文件，每行一个 (默认: rtsp_list.txt)",
    )
    parser.add_argument(
        "--gpu-id",
        default=0,
        type=int,
        help="目标 GPU ID (默认: 0)",
    )
    parser.add_argument(
        "--workers",
        default=20,
        type=int,
        help="并发启动流的数量 (默认: 20)",
    )
    parser.add_argument(
        "--heartbeat",
        default=100000,
        type=int,
        help="心跳超时时间 (ms)，默认 100s",
    )
    parser.add_argument(
        "--decode-interval",
        default=0,
        type=int,
        help="解码间隔 (ms)，0 表示不抽帧",
    )
    parser.add_argument(
        "--only-key-frames",
        action="store_true",
        help="是否只解码关键帧",
    )
    parser.add_argument(
        "--keep-on-failure",
        action="store_true",
        help="连接失败后是否保留任务继续重连",
    )
    parser.add_argument(
        "--use-shared-mem",
        action="store_true",
        help="是否启用共享内存传输（需客户端与服务端在同一机器）",
    )
    parser.add_argument(
        "--check-status",
        action="store_true",
        help="启动后批量查询所有流的状态",
    )
    parser.add_argument(
        "--check-timeout",
        default=30,
        type=int,
        help="等待流进入 CONNECTED 状态的超时时间 (秒)，仅与 --check-status 配合生效",
    )
    return parser.parse_args()


def load_rtsp_list(path: str) -> List[str]:
    """从文件读取 RTSP URL 列表，过滤空行和注释。"""
    if not os.path.isfile(path):
        logger.error(f"列表文件不存在: {path}")
        sys.exit(1)

    urls = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            urls.append(line)

    if not urls:
        logger.error(f"列表文件为空: {path}")
        sys.exit(1)

    logger.info(f"从 {path} 读取到 {len(urls)} 个 RTSP URL")
    return urls


def start_one_stream(
    client: RTSPClient,
    rtsp_url: str,
    gpu_id: int,
    heartbeat_ms: int,
    decode_interval_ms: int,
    only_key_frames: bool,
    keep_on_failure: bool,
    use_shared_mem: bool,
) -> Tuple[str, bool, str]:
    """启动单路流，返回 (url, success, stream_id_or_message)。"""
    stream_id = client.start_stream(
        rtsp_url=rtsp_url,
        heartbeat_timeout_ms=heartbeat_ms,
        decode_interval_ms=decode_interval_ms,
        decoder_type=DECODER_GPU_NVCUVID,
        gpu_id=gpu_id,
        keep_on_failure=keep_on_failure,
        use_shared_mem=use_shared_mem,
        only_key_frames=only_key_frames,
    )
    if stream_id:
        return rtsp_url, True, stream_id
    return rtsp_url, False, "启动失败"


def check_all_status(
    client: RTSPClient,
    stream_ids: List[str],
    timeout_sec: int,
) -> Tuple[int, int, int]:
    """等待并统计流状态，返回 (connected, connecting, disconnected/not_found)。"""
    deadline = time.time() + timeout_sec
    connected = set()
    checked_any = set()

    while time.time() < deadline and len(connected) < len(stream_ids):
        for sid in stream_ids:
            if sid in connected:
                continue
            info = client.check_stream(sid)
            checked_any.add(sid)
            if info and info.get("status") == 1:  # STATUS_CONNECTED
                connected.add(sid)
        if len(connected) < len(stream_ids):
            time.sleep(1)

    not_connected = len(stream_ids) - len(connected)
    connecting_count = 0
    disconnected_count = 0
    not_found_count = 0

    for sid in stream_ids:
        if sid in connected:
            continue
        info = client.check_stream(sid)
        status = info.get("status") if info else 3  # STATUS_NOT_FOUND
        if status == 0:  # STATUS_CONNECTING
            connecting_count += 1
        elif status == 2:  # STATUS_DISCONNECTED
            disconnected_count += 1
        else:
            not_found_count += 1

    logger.info(
        f"状态检查完成 (等待 {timeout_sec}s): "
        f"已连接={len(connected)}, 连接中={connecting_count}, "
        f"无法连接={disconnected_count}, 不存在={not_found_count}"
    )
    return len(connected), connecting_count, disconnected_count + not_found_count


def main() -> int:
    args = parse_args()
    urls = load_rtsp_list(args.list_file)

    client = RTSPClient(server_address=args.server)
    if not client.connect():
        logger.error(f"无法连接到 gRPC 服务器: {args.server}")
        return 1

    logger.info(
        f"开始批量启动流: server={args.server}, gpu_id={args.gpu_id}, "
        f"workers={args.workers}, only_key_frames={args.only_key_frames}, "
        f"use_shared_mem={args.use_shared_mem}"
    )

    started_ids: List[str] = []
    failed_urls: List[str] = []
    start_time = time.time()

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        future_to_url = {
            executor.submit(
                start_one_stream,
                client,
                url,
                args.gpu_id,
                args.heartbeat,
                args.decode_interval,
                args.only_key_frames,
                args.keep_on_failure,
                args.use_shared_mem,
            ): url
            for url in urls
        }

        for future in as_completed(future_to_url):
            url = future_to_url[future]
            try:
                _, success, result = future.result()
                if success:
                    started_ids.append(result)
                    logger.info(f"[OK] {url} -> {result}")
                else:
                    failed_urls.append(url)
                    logger.error(f"[FAIL] {url}: {result}")
            except Exception as e:
                failed_urls.append(url)
                logger.error(f"[EXCEPTION] {url}: {e}")

    elapsed = time.time() - start_time
    logger.info(
        f"启动阶段完成: 成功={len(started_ids)}, 失败={len(failed_urls)}, "
        f"总耗时={elapsed:.2f}s"
    )

    if args.check_status and started_ids:
        check_all_status(client, started_ids, args.check_timeout)

    client.disconnect()

    # 有失败时返回非 0，方便脚本调用方判断
    return 0 if not failed_urls else 2


if __name__ == "__main__":
    sys.exit(main())
