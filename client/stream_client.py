from remote_video_capture import RemoteVideoCapture
from stream_service_pb2 import DECODER_GPU_CUDA
import time
import cv2

if __name__ == '__main__':
    url = "rtsp://admin:lww123456@172.16.22.16:554/Streaming/Channels/801"
    
    with RemoteVideoCapture(url, decode_interval_ms=0, decoder_type=DECODER_GPU_CUDA, gpu_id=0) as cap:
        while not cap.is_connected():
            print("等待连接...")
            time.sleep(1)
            
        print("✅ 已连接，开始流式接收...")
        
        # 使用流式接口，max_fps=10 表示最多 10fps
        index = 0
        for ret, frame in cap.stream_frames(max_fps=25):
            if ret:
                filename = f"stream_{index}.jpg"
                cv2.imwrite(filename, frame)
                print(f"✅ 保存: {filename}")
                index += 1
                
                if index >= 1000:  # 保存 100 帧后退出
                    break
            else:
                print("⚠️  未获取到帧")
