import requests
import json
import base64
import time

BASE_URL = "http://localhost:8080"

def test_api_workflow():
    print("=== 开始 RTSP 视频流服务 API 测试 ===")

    # 1. 启动流
    print("\n[1/5] 正在启动流任务...")
    start_payload = {
        "rtsp_url": "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/901",
        "decoder_type": "DECODER_CPU_FFMPEG",
        "gpu_id": 0,
        "keep_on_failure": False
    }
    resp = requests.post(f"{BASE_URL}/v1/streams/start", json=start_payload)
    data = resp.json()
    print(f"响应: {data}")
    stream_id = data.get("streamId")
    
    if not stream_id:
        print("启动失败，请检查 gRPC 后端状态 (是否监听 0.0.0.0:50051?)")
        return

    # 等待一会儿确保流已初始化
    time.sleep(2)

    # 2. 查询单个流状态
    print("\n[2/5] 正在查询流状态...")
    status = requests.get(f"{BASE_URL}/v1/streams/{stream_id}/status")
    print(f"状态: {status.json()}")

    # 3. 获取最新帧 (Base64)
    print("\n[3/5] 正在拉取最新帧...")
    frame = requests.get(f"{BASE_URL}/v1/streams/{stream_id}/frame")
    frame_data = frame.json()
    print(f"响应: {frame_data.keys()}")
    if frame_data.get("success") and "imageData" in frame_data:
        img_b64 = frame_data["imageData"]
        # 解码并保存测试
        with open("test_output.jpg", "wb") as f:
            f.write(base64.b64decode(img_b64))
        print(f"成功获取图片，已保存为 test_output.jpg (大小: {len(img_b64)} bytes)")

    # 4. 获取所有流列表
    print("\n[4/5] 查询流列表...")
    all_streams = requests.get(f"{BASE_URL}/v1/streams")
    print(f"当前总数: {all_streams.json().get('total_count')}")

    # 5. 停止流
    print("\n[5/5] 正在停止流...")
    stop = requests.post(f"{BASE_URL}/v1/streams/stop", json={"stream_id": stream_id})
    print(f"响应: {stop.json()}")

def test_streaming_endpoint(stream_id):
    """测试流式传输 (StreamFrames)"""
    print(f"\n=== 开始测试流式接口: {stream_id} ===")
    url = f"{BASE_URL}/v1/streams/{stream_id}/stream"
    try:
        # stream=True 允许处理长连接
        with requests.get(url, stream=True) as r:
            print("连接已建立，正在接收帧数据...")
            for i, line in enumerate(r.iter_lines()):
                if line:
                    decoded_line = json.loads(line)
                    print(f"收到第 {i+1} 帧, 成功: {decoded_line.get('success')}")
                if i >= 5: # 只收 5 帧就停止
                    print("测试完毕，关闭连接。")
                    break
    except Exception as e:
        print(f"流式接口测试错误: {e}")

if __name__ == "__main__":
    # 简单的执行流程
    test_api_workflow()