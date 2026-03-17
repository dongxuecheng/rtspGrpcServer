import requests
import json
import base64
import time
import logging
import cv2
import numpy as np
import os

# 配置日志
logging.basicConfig(level=logging.INFO, format='%(asctime)s - [%(levelname)s] - %(message)s')
logger = logging.getLogger(__name__)

# 配置参数
BASE_URL = "http://172.16.20.193:8080"
SAVE_DIR = "output_frames"
if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)

session = requests.Session()

def base64_to_image(base64_str):
    """将Base64字符串转换为OpenCV图像"""
    try:
        img_data = base64.b64decode(base64_str)
        np_arr = np.frombuffer(img_data, np.uint8)
        img = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
        return img
    except Exception as e:
        logger.error(f"Base64解码失败: {e}")
        return None

# ---------------------------------------------------------
# 1. 核心接口测试函数
# ---------------------------------------------------------

def test_start_stream(rtsp_url):
    """POST /v1/streams/start"""
    logger.info(f"测试：启动流 {rtsp_url}")
    payload = {
        "rtspUrl": rtsp_url,
        "decodeIntervalMs": 1000,
        "decoderType": "DECODER_CPU_FFMPEG",
        "heartbeatTimeoutMs": 10000,
        "keepOnFailure": False,
        "onlyKeyFrames": False,
        "useSharedMem": False
    }
    resp = session.post(f"{BASE_URL}/v1/streams/start", json=payload)
    if resp.status_code != 200:
        logger.error(f"启动失败: {resp.text}")
        return None
    data = resp.json()
    sid = data.get("streamId")
    logger.info(f"启动成功，StreamId: {sid}")
    return sid

def test_check_stream(stream_id):
    """GET /v1/streams/{streamId}/status"""
    resp = session.get(f"{BASE_URL}/v1/streams/{stream_id}/status")
    if resp.status_code != 200: return None
    
    data = resp.json()
    # 修复逻辑：状态嵌套在 stream 字段中
    stream_info = data.get("stream", {})
    return stream_info

def test_get_latest_frame(stream_id):
    """GET /v1/streams/{streamId}/frame 并保存"""
    resp = session.get(f"{BASE_URL}/v1/streams/{stream_id}/frame")
    if resp.status_code != 200: return -1, None
    
    data = resp.json()
    b64_data = data.get("imageData")
    seq = data.get("frameSeq", -1)
    
    if not b64_data:
        return seq, None
        
    image = base64_to_image(b64_data)
    if image is not None:
        save_path = os.path.join(SAVE_DIR, f"{seq}.jpg")
        cv2.imwrite(save_path, image)
        return seq, image
    return seq, None

def test_stop_stream(stream_id):
    """POST /v1/streams/stop"""
    resp = session.post(f"{BASE_URL}/v1/streams/stop", json={"streamId": stream_id})
    logger.info(f"停止流响应: {resp.json()}")

# ---------------------------------------------------------
# 2. 自动化执行逻辑
# ---------------------------------------------------------

if __name__ == "__main__":
    # 请根据实际情况修改 RTSP 地址
    RTSP_URL = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/101"
    
    sid = test_start_stream(RTSP_URL)
    if not sid:
        logger.error("无法创建流任务")
        exit(1)

    try:
        # 等待流连接成功 (STATUS_CONNECTED)
        logger.info("等待流连接...")
        connected = False
        for _ in range(15):
            info = test_check_stream(sid)
            print(info)
            status = info.get("status") if info else "UNKNOWN"
            if status == "STATUS_CONNECTED":
                connected = True
                logger.info("流已连接！")
                break
            time.sleep(1)

        if not connected:
            logger.error("流连接超时，请检查网络或RTSP地址")
        else:
            # 循环获取最新帧并保存
            logger.info("开始轮询获取最新帧...")
            while True:
                seq, img = test_get_latest_frame(sid)
                if img is not None:
                    logger.info(f"轮询保存成功: Seq: {seq}")
                # time.sleep(2)
                if seq > 5:
                    break
                

    except Exception as e:
        logger.error(f"测试异常: {e}")
    finally:
        # 无论成功失败都尝试停止流，释放资源
        test_stop_stream(sid)
        logger.info("测试任务结束，资源已释放")