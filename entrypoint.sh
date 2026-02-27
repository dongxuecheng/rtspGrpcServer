#!/bin/bash

ldconfig

# 1. 在运行时创建软链接 (此时驱动已通过 --gpus all 挂载进来了)
if [ -f /usr/lib/x86_64-linux-gnu/libnvcuvid.so.1 ]; then
    ln -sf /usr/lib/x86_64-linux-gnu/libnvcuvid.so.1 /usr/lib/x86_64-linux-gnu/libnvcuvid.so
fi

# 2. 启动你的服务
exec ./rtsp_server