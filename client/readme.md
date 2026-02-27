# 生成 Python 的 gRPC 接口代码
pip install grpcio grpcio-tools
python -m grpc_tools.protoc -I. --python_out=. --grpc_python_out=. stream_service.proto