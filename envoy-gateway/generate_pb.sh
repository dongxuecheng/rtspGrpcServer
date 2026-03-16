#!/bin/bash
set -e

# 1. 下载 Google API 依赖
# mkdir -p proto/google/api
# curl -o proto/google/api/annotations.proto https://raw.githubusercontent.com/googleapis/googleapis/master/google/api/annotations.proto
# curl -o proto/google/api/http.proto https://raw.githubusercontent.com/googleapis/googleapis/master/google/api/http.proto

# 2. 生成 descriptor set 文件给 Envoy 使用
# 请确保你的机器上安装了 protoc (如未安装：sudo apt install protobuf-compiler)
protoc -I ./proto \
       --include_imports \
       --include_source_info \
       --descriptor_set_out=./proto/stream.pb \
       ./proto/stream.proto

echo "stream.pb 生成成功！"