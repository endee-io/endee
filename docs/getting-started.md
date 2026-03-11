# Getting Started with Endee

This guide covers the technical setup and runtime details for building and running Endee locally or with Docker.

## System Requirements

Before installing, ensure your system meets the following hardware and operating system requirements.

### Supported Operating Systems

- **Linux**: Ubuntu(22.04, 24.04, 25.04) Debian(12, 13), Rocky(8, 9, 10), CentOS(8, 9, 10), Fedora(40, 42, 43)
- **macOS**: Apple Silicon (M Series) only

### Required Dependencies

The following packages are required for compilation:

`clang-19`, `cmake`, `build-essential`, `libssl-dev`, `libcurl4-openssl-dev`

> **Note:** The build system requires **Clang 19** or a compatible recent Clang version with C++20 support.

## 1. Quick Installation

The easiest way to build **ndd** is using the included `install.sh` script. This script handles OS detection, dependency checks, and configuration automatically.

### Usage

First, ensure the script is executable:

```bash
chmod +x ./install.sh
```

Run the script from the repository root. You must provide arguments for the build mode and/or CPU optimization.

```bash
./install.sh [BUILD_MODE] [CPU_OPTIMIZATION]
```

### Build Arguments

You can combine one build mode and one CPU optimization flag.

#### Build Modes

| Flag | Description | CMake Equivalent |
| --- | --- | --- |
| `--release` | Default optimized release build |  |
| `--debug_all` | Enable full debugging symbols | `-DND_DEBUG=ON -DDEBUG=ON` |
| `--debug_nd` | Enable NDD-specific logging and timing | `-DND_DEBUG=ON` |

#### CPU Optimization Options

Select the flag matching your hardware to enable SIMD optimizations.

| Flag | Description | Target Hardware |
| --- | --- | --- |
| `--avx2` | Enable AVX2 (FMA, F16C) | Modern x86_64 Intel/AMD |
| `--avx512` | Enable AVX512 (F, BW, VNNI, FP16) | Server-grade x86_64 (Xeon/Epyc) |
| `--neon` | Enable NEON (FP16, DotProd) | Apple Silicon / ARMv8.2+ |
| `--sve2` | Enable SVE2 (INT8/16, FP16) | ARMv9 / SVE2-compatible systems |

> **Note:** The `--avx512` build configuration enforces runtime checks for required instruction sets. Your CPU must support `avx512`, `avx512_fp16`, `avx512_vnni`, `avx512bw`, and `avx512_vpopcntdq` or the database will exit during initialization.

### Example Commands

Build for production on Intel/AMD with AVX2:

```bash
./install.sh --release --avx2
```

Build for debugging on Apple Silicon:

```bash
./install.sh --debug_all --neon
```

### Running the Server

Use `run.sh` to simplify local startup. It automatically detects the built binary and uses `ndd_data_dir=./data` by default.

First, ensure the script is executable:

```bash
chmod +x ./run.sh
```

Then run:

```bash
./run.sh
```

#### Options

- `ndd_data_dir=DIR`: set the data directory
- `binary_file=FILE`: set the binary file to run
- `ndd_auth_token=TOKEN`: set the authentication token; leave empty to run without authentication

#### Examples

Run with a custom data directory:

```bash
./run.sh ndd_data_dir=./my_data
```

Run a specific binary:

```bash
./run.sh binary_file=./build/ndd-avx2
```

Run with authentication enabled:

```bash
./run.sh ndd_auth_token=your_token
```

Run with all options:

```bash
./run.sh ndd_data_dir=./my_data binary_file=./build/ndd-avx2 ndd_auth_token=your_token
```

Show help:

```bash
./run.sh --help
```

## 2. Manual Build

If you prefer to configure the build manually or integrate it into an existing install pipeline, use `cmake` directly.

### Step 1: Prepare the Build Directory

```bash
mkdir build && cd build
```

### Step 2: Configure

Run `cmake` with the appropriate flags. Define the compiler manually if it is not your system default.

Debug options:

- `-DDEBUG=ON` to enable debug symbols and `O0`
- `-DND_DEBUG=ON` to enable internal logging

SIMD selectors, choose one:

- `-DUSE_AVX2=ON`
- `-DUSE_AVX512=ON`
- `-DUSE_NEON=ON`
- `-DUSE_SVE2=ON`

Example x86_64 AVX512 release configuration:

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DUSE_AVX512=ON \
      ..
```

### Step 3: Compile

```bash
make -j$(nproc)
```

### Running the Built Binary

After a successful build, the binary is generated in the `build/` directory.

### Binary Naming

The output binary name depends on the SIMD flag used during compilation:

- `ndd-avx2`
- `ndd-avx512`
- `ndd-neon` or `ndd-neon-darwin` on macOS
- `ndd-sve2`

A symlink named `ndd` points to the binary compiled for the current build.

### Runtime Environment Variables

Some environment variables **ndd** reads at runtime:

- `NDD_DATA_DIR`: defines the data directory
- `NDD_AUTH_TOKEN`: optional authentication token

### Authentication

**Open mode** with no authentication is the default when `NDD_AUTH_TOKEN` is not set:

```bash
./build/ndd
curl http://{{BASE_URL}}/api/v1/index/list
```

**Token mode** is enabled when `NDD_AUTH_TOKEN` is set:

```bash
export NDD_AUTH_TOKEN=$(openssl rand -hex 32)
./build/ndd
curl -H "Authorization: $NDD_AUTH_TOKEN" http://{{BASE_URL}}/api/v1/index/list
```

### Execution Example

Run the database using the AVX2 binary and a local `data` folder:

```bash
mkdir -p ./data
export NDD_DATA_DIR=$(pwd)/data
./build/ndd
```

Alternatively:

```bash
NDD_DATA_DIR=./data ./build/ndd
```

## 3. Docker Deployment

Endee ships with a Dockerfile for containerized deployment.

### Build the Image

You must specify the target architecture using the `BUILD_ARCH` build argument. Valid targets are `avx2`, `avx512`, `neon`, and `sve2`. You can optionally enable a debug build using `DEBUG=true`.

```bash
docker build --ulimit nofile=100000:100000 --build-arg BUILD_ARCH=avx2 -t endee-oss:latest -f ./infra/Dockerfile .
```

```bash
docker build --ulimit nofile=100000:100000 --build-arg BUILD_ARCH=neon --build-arg DEBUG=true -t endee-oss:latest -f ./infra/Dockerfile .
```

### Run the Container

The container exposes port `8080` and stores data in `/data` inside the container. Persist that data with a Docker volume.

```bash
docker run \
  -p 8080:8080 \
  -v endee-data:/data \
  -e NDD_AUTH_TOKEN="your_secure_token" \
  --name endee-server \
  endee-oss:latest
```

Leave `NDD_AUTH_TOKEN` empty or remove it to run Endee without authentication.

### Docker Compose

You can also use Docker Compose:

```bash
docker-compose up
```

## 4. Run from Docker Registry

You can run Endee directly using the prebuilt image from Docker Hub without building locally.

### Using Docker Compose

Create a new directory:

```bash
mkdir endee && cd endee
```

Create a `docker-compose.yml` file with:

```yaml
services:
  endee:
    image: endeeio/endee-server:latest
    container_name: endee-server
    ports:
      - "8080:8080"
    environment:
      NDD_NUM_THREADS: 0
      NDD_AUTH_TOKEN: ""
    volumes:
      - endee-data:/data
    restart: unless-stopped

volumes:
  endee-data:
```

Then run:

```bash
docker compose up -d
```

For more details, visit [docs.endee.io](https://docs.endee.io/quick-start).
