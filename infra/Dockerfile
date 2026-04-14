# Stage 1: Build the engine
FROM ubuntu:22.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential cmake git libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN mkdir build && cd build && cmake .. && make -j$(nproc)

# Stage 2: Run the engine
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y libstdc++6 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/build/ndd-server /usr/local/bin/ndd-server
RUN mkdir -p /data && chmod 777 /data
EXPOSE 8080
ENTRYPOINT ["ndd-server", "--host", "0.0.0.0", "--port", "8080", "--data_dir", "/data"]