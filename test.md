docker run --gpus all --name stream -p 50052:50051 -e NVIDIA_DRIVER_CAPABILITIES=compute,utility,video -v `pwd`:/workspace -w /workspace -it nvcr.io/nvidia/cuda:12.6.3-cudnn-devel-ubuntu22.04 /bin/bash


sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

apt-get update -y && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libopencv-dev \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    protobuf-compiler \
    libprotobuf-dev \
    libfmt-dev \
    libspdlog-dev


ln -s /usr/lib/x86_64-linux-gnu/libnvcuvid.so.1 /usr/lib/x86_64-linux-gnu/libnvcuvid.so