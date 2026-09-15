# 第一阶段：安装开发依赖，编译 C++ 程序。
FROM ubuntu:22.04 AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       g++ \
       libcurl4-openssl-dev \
       nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY examples/tcp_llm_stream/server.cpp \
     examples/tcp_llm_stream/llm_http.hpp \
     examples/tcp_llm_stream/llm_stream.hpp \
     ./

RUN g++ -std=c++17 -O2 -Wall -Wextra -pthread \
    server.cpp \
    -o /src/tcp-stream-server \
    -lcurl


# 第二阶段：只保留运行依赖和编译产物。
FROM ubuntu:22.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       libcurl4 \
       libstdc++6 \
       ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/tcp-stream-server /app/tcp-stream-server

# 使用非 root 用户运行。
USER 10001:10001

ENTRYPOINT ["/app/tcp-stream-server"]