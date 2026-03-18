# STAGE 1: BUILDER
ARG BASE_IMAGE=debian:13-slim
FROM ${BASE_IMAGE} AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    git \
    cmake \
    clang \
    build-essential \
    libssl-dev \
    libcurl4-openssl-dev \
    unzip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build_space

# Copy source code
COPY ./src /build_space/src
COPY ./third_party /build_space/third_party
COPY ./serverless /build_space/serverless
COPY ./CMakeLists.txt /build_space/CMakeLists.txt
COPY ./install.sh /build_space/install.sh

# Run build script
ARG BUILD_ARCH
ENV BUILD_ARCH=${BUILD_ARCH}
ARG DEBUG=false
ENV DEBUG=${DEBUG}
ARG NDD_SERVERLESS
ENV NDD_SERVERLESS=${NDD_SERVERLESS}
RUN chmod +x ./install.sh && \
    if [ -z "$BUILD_ARCH" ]; then echo "ERROR: BUILD_ARCH is required (avx2|avx512|neon|sve2)" && exit 1; fi && \
    if [ -z "$NDD_SERVERLESS" ]; then echo "ERROR: NDD_SERVERLESS=ON is required" && exit 1; fi && \
    SERVERLESS_FLAG="" && \
    if [ "$NDD_SERVERLESS" = "ON" ]; then SERVERLESS_FLAG="--serverless"; fi && \
    if [ "$DEBUG" = "true" ]; then \
        ./install.sh --${BUILD_ARCH} --debug_all --skip-deps ${SERVERLESS_FLAG}; \
    else \
        ./install.sh --${BUILD_ARCH} --release --skip-deps ${SERVERLESS_FLAG}; \
    fi && \
    cp ./build/ndd-${BUILD_ARCH} ./ndd

# STAGE 2: RUNTIME
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PATH=/usr/local/bin:$PATH

# Install runtime dependencies (no dev tools)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libcurl4 \
    liblmdb0 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN groupadd -r -g 1000 endee && \
    useradd -r -u 1000 -g endee -s /bin/false -d /home/endee endee && \
    mkdir -p /data /home/endee && \
    chown -R endee:endee /data /home/endee && \
    chmod 755 /data

# Copy binary from builder
COPY --from=builder /build_space/ndd /usr/local/bin/ndd
RUN chmod +x /usr/local/bin/ndd

# Copy entrypoint script
COPY ./infra/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Switch to non-root user
USER endee
WORKDIR /home/endee

# App env
ENV NDD_DATA_DIR=/data \
    NDD_SERVER_PORT=8080 \
    NDD_LOG_LEVEL=info \
    NDD_NUM_THREADS=0

EXPOSE 8080

# Health check
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD curl -f http://localhost:8080/api/v1/health || exit 1

LABEL org.opencontainers.image.title="endee-serverless"
LABEL org.opencontainers.image.description="Endee Serverless"
LABEL org.opencontainers.image.url="https://endee.io/"
LABEL org.opencontainers.image.documentation="https://docs.endee.io/"
LABEL org.opencontainers.image.vendor="Endee Labs"
#To be updated
LABEL org.opencontainers.image.source=""


# Set the entrypoint
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]

CMD []
