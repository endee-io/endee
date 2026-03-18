<p align="center">
  <picture>
      <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-dark.svg">
      <source media="(prefers-color-scheme: light)" srcset="docs/assets/logo-light.svg">
      <img height="100" alt="Endee" src="docs/assets/logo-dark.svg">
  </picture>
</p>

<p align="center">
    <b>High-performance open-source vector database for AI search, RAG, semantic search, and hybrid retrieval.</b>
</p>

<p align="center">
    <a href="./docs/getting-started.md"><img src="https://img.shields.io/badge/Quick_Start-Local_Setup-success?style=flat-square" alt="Quick Start"></a>
    <a href="https://docs.endee.io/quick-start"><img src="https://img.shields.io/badge/Docs-Quick_Start-success?style=flat-square" alt="Docs"></a>
    <a href="https://github.com/endee-io/endee/blob/master/LICENSE"><img src="https://img.shields.io/github/license/endee-io/endee?style=flat-square" alt="License"></a>
    <a href="https://discord.gg/5HFGqDZQE3"><img src="https://img.shields.io/badge/Discord-Join_Chat-5865F2?logo=discord&style=flat-square" alt="Discord"></a>
    <a href="https://endee.io/"><img src="https://img.shields.io/badge/Website-Endee-111111?style=flat-square" alt="Website"></a>
    <!-- <a href="https://endee.io/benchmarks"><img src="https://img.shields.io/badge/Benchmarks-Coming_Soon-1F8B4C?style=flat-square" alt="Benchmarks"></a> -->
    <!-- <a href="https://endee.io/cloud"><img src="https://img.shields.io/badge/Cloud-Coming_Soon-2496ED?style=flat-square" alt="Cloud"></a> -->
</p>

<p align="center">
<strong><a href="./docs/getting-started.md">Quick Start</a> • <a href="#why-endee">Why Endee</a> • <a href="#use-cases">Use Cases</a> • <a href="#features">Features</a> • <a href="#api-and-clients">API and Clients</a> • <a href="#docs-and-links">Docs</a> • <a href="#community-and-contact">Contact</a></strong>
</p>

# Endee: Open-Source Vector Database for AI Search

**Endee** is a high-performance open-source vector database built for AI search and retrieval workloads. It is designed for teams building **RAG pipelines**, **semantic search**, **hybrid search**, recommendation systems, and filtered vector retrieval APIs that need production-oriented performance and control.

Endee combines vector search with filtering, sparse retrieval support, backup workflows, and deployment flexibility across local builds and Docker-based environments. The project is implemented in C++ and optimized for modern CPU targets, including AVX2, AVX512, NEON, and SVE2.

If you want the fastest path to evaluate Endee locally, start with the [Getting Started guide](./docs/getting-started.md) or the hosted docs at [docs.endee.io](https://docs.endee.io/quick-start).

## Why Endee

- Built as a dedicated vector database for AI applications, search systems, and retrieval-heavy workloads.
- Supports dense vector retrieval plus sparse search capabilities for hybrid search use cases.
- Includes payload filtering for metadata-aware retrieval and application-specific query logic.
- Ships with operational features already documented in this repo, including backup flows and runtime observability.
- Offers flexible deployment paths: local scripts, manual builds, Docker images, and prebuilt registry images.

## Getting Started

The full installation, build, Docker, runtime, and authentication instructions are in [docs/getting-started.md](./docs/getting-started.md).

Fastest local path:

```bash
chmod +x ./install.sh ./run.sh
./install.sh --release --avx2
./run.sh
```

The server listens on port `8080`. For detailed setup paths, supported operating systems, CPU optimization flags, Docker usage, and authentication examples, use:

- [Getting Started](./docs/getting-started.md)
- [Hosted Quick Start Docs](https://docs.endee.io/quick-start)

## Use Cases

### RAG and AI Retrieval

Use Endee as the retrieval layer for question answering, chat assistants, copilots, and other RAG applications that need fast vector search with metadata-aware filtering.

### Agentic AI and AI Agent Memory

Use Endee as the long-term memory and context retrieval layer for AI agents built with frameworks like LangChain, CrewAI, AutoGen, and LlamaIndex. Store and retrieve past observations, tool outputs, conversation history, and domain knowledge mid-execution with low-latency filtered vector search, so your autonomous agents get the right context without stalling their reasoning loop.

### Semantic Search

Build semantic search experiences for documents, products, support content, and knowledge bases using vector similarity search instead of exact keyword-only matching.

### Hybrid Search

Combine dense retrieval, sparse vectors, and filtering to improve relevance for search workflows where both semantic understanding and term-level precision matter.

### Recommendations and Matching

Support recommendation, similarity matching, and nearest-neighbor retrieval workflows across text, embeddings, and other high-dimensional representations.

## Features

- **Vector search** for AI retrieval and semantic similarity workloads.
- **Hybrid retrieval support** with sparse vector capabilities documented in [docs/sparse.md](./docs/sparse.md).
- **Payload filtering** for structured retrieval logic documented in [docs/filter.md](./docs/filter.md).
- **Backup APIs and flows** documented in [docs/backup-system.md](./docs/backup-system.md).
- **Operational logging and instrumentation** documented in [docs/logs.md](./docs/logs.md) and [docs/mdbx-instrumentation.md](./docs/mdbx-instrumentation.md).
- **CPU-targeted builds** for AVX2, AVX512, NEON, and SVE2 deployments.
- **Docker deployment options** for local and server environments.

## API and Clients

Endee exposes an HTTP API for managing indexes and serving retrieval workloads. The current repo documentation and examples focus on running the server directly and calling its API endpoints.

Current developer entry points:

- [Getting Started](./docs/getting-started.md) for local build and run flows
- [Hosted Docs](https://docs.endee.io/quick-start) for product documentation
- [Release Notes 1.0.0](https://github.com/endee-io/endee/releases/tag/1.0.0) for recent platform changes

## Docs and Links

- [Getting Started](./docs/getting-started.md)
- [Hosted Documentation](https://docs.endee.io/quick-start)
- [Release Notes](https://github.com/endee-io/endee/releases/tag/1.0.0)
- [Sparse Search](./docs/sparse.md)
- [Filtering](./docs/filter.md)
- [Backups](./docs/backup-system.md)

## Community and Contact

- Join the community on [Discord](https://discord.gg/5HFGqDZQE3)
- Visit the website at [endee.io](https://endee.io/)
- For trademark or branding permissions, contact [enterprise@endee.io](mailto:enterprise@endee.io)

## Contributing

We welcome contributions from the community to help make vector search faster and more accessible for everyone.

- Submit pull requests for fixes, features, and improvements
- Report bugs or performance issues through GitHub issues
- Propose enhancements for search quality, performance, and deployment workflows

* **SIMD Selectors (Choose One):**
* `-DUSE_AVX2=ON`
* `-DUSE_AVX512=ON`
* `-DUSE_NEON=ON`
* `-DUSE_SVE2=ON`


**Example (x86_64 AVX512 Release):**

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

After a successful build, the binary will be generated in the `build/` directory.

### Binary Naming

The output binary name depends on the SIMD flag used during compilation:

* `ndd-avx2`
* `ndd-avx512`
* `ndd-neon` (or `ndd-neon-darwin` for mac)
* `ndd-sve2`

A symlink called `ndd` links to the binary compiled for the current build.

### Runtime Environment Variables

Some environment variables **ndd** reads at runtime:

* `NDD_DATA_DIR`: Defines the data directory
* `NDD_AUTH_TOKEN`: Authentication token (mandatory)

### Authentication

`NDD_AUTH_TOKEN` is **mandatory**. The server will not start without it. All API requests require the token in the `Authorization` header.

```bash
# Generate a secure token
export NDD_AUTH_TOKEN=$(openssl rand -hex 32)
./build/ndd

# All APIs require the token in Authorization header
curl -H "Authorization: $NDD_AUTH_TOKEN" http://{{BASE_URL}}/api/v1/index/list
```

### Execution Example

To run the database using the AVX2 binary and a local `data` folder:

```bash
# 1. Create the data directory
mkdir -p ./data

# 2. Export the environment variable and run
export NDD_DATA_DIR=$(pwd)/data
./build/ndd
```

Alternatively, as a single line:

```bash
NDD_DATA_DIR=./data ./build/ndd
```

---



## 3. Docker Deployment

We provide a Dockerfile for easy containerization. This ensures a consistent runtime environment and simplifies the deployment process across various platforms.

### Build the Image

You **must** specify the target architecture (`avx2`, `avx512`, `neon`, `sve2`) using the `BUILD_ARCH` build argument and **must** specify `NDD_SERVERLESS=ON` to enable serverless features. You can optionally enable a debug build using the `DEBUG` argument.

```bash
# Production Build (AVX2) (for x86_64 systems)
docker build --ulimit nofile=100000:100000 --build-arg BUILD_ARCH=avx2 --build-arg NDD_SERVERLESS=ON -t endee-serverless:latest -f ./infra/Dockerfile .

# Debug Build (Neon) (for arm64, mac apple silicon)
docker build --ulimit nofile=100000:100000 --build-arg BUILD_ARCH=neon --build-arg DEBUG=true --build-arg NDD_SERVERLESS=ON -t endee-serverless:latest -f ./infra/Dockerfile .
```

### Run the Container

The container exposes port `8080` and stores data in `/data` inside container. You should persist this data using a docker volume.

```bash
docker run \
  -p 8080:8080 \
  -v nd-data:/data \
  -e NDD_AUTH_TOKEN="your_secure_token" \
  --name endee-serverless \
  endee-serverless:latest
```

`NDD_AUTH_TOKEN` is **mandatory**. The server will not start without it.


## 4. Running Docker container from registry

You can run Endee directly using the pre-built image from Docker Hub without building locally.

### Using Docker Compose

Create a new directory for Endee:

```bash
mkdir endee && cd endee
```

Inside this directory, create a file named `docker-compose.yml` and copy the following content into it:

```yaml
services:
  endee-serverless:
    image: endee-serverless:latest
    container_name: endee-serverless
    ports:
      - "8080:8080"
    environment:
      NDD_NUM_THREADS: 0
      NDD_AUTH_TOKEN: "your_secure_token"  # Required: set your auth token
    volumes:
      - nd-data:/data
    restart: unless-stopped

volumes:
  nd-data:
```

Then run:
```bash
docker compose up -d
```

for more details visit [docs.endee.io](https://docs.endee.io/quick-start)


## Serverless Build

Endee supports a serverless mode with multi-user authentication, tier-based access control, and admin management APIs. Serverless mode is enabled via the `-DNDD_SERVERLESS=ON` CMake flag and uses the same codebase with conditional compilation.

### Serverless Features

* **Multi-user authentication** with MDBX-backed persistent storage
* **4 user tiers**: Starter, Pro, Scale, Admin — each with configurable limits
* **Tier-based limits**: vector count, dimension, and index count per tier
* **Admin APIs**: 18 endpoints for user management, token management, and index administration
* **Root token**: `NDD_AUTH_TOKEN` serves as the root admin token (mandatory)

### Tier Limits

| Tier | Max Vectors | Max Dimension | Max Indices |
|------|-------------|---------------|-------------|
| Starter | 1M | 2,000 | 3 |
| Pro | 10M | 4,000 | 10 |
| Scale | 100M | 8,000 | Unlimited |
| Admin | 1B | Unlimited | Unlimited |

### Building Serverless

```bash
mkdir build_serverless && cd build_serverless
cmake -DNDD_SERVERLESS=ON -DUSE_NEON=ON ..
make -j$(nproc)
```

Replace `-DUSE_NEON=ON` with the appropriate SIMD flag for your platform (`-DUSE_AVX2=ON`, `-DUSE_AVX512=ON`, `-DUSE_SVE2=ON`).

### Running Serverless

`NDD_AUTH_TOKEN` is **mandatory** in serverless mode. The server will exit with a fatal error if it is not set.

```bash
NDD_AUTH_TOKEN=your-root-token NDD_DATA_DIR=./data ./build_serverless/ndd
```

The root token has Admin tier access (unlimited). Use it to create users and manage the system.

### Serverless Auth Flow

**1. Create a user** (auto-generates a token):
```bash
curl -X POST http://localhost:8080/api/v1/admin/users \
  -H "Authorization: your-root-token" \
  -H "Content-Type: application/json" \
  -d '{"username": "alice", "user_type": "Starter"}'
# Response: {"message":"User created successfully","username":"alice","user_type":"Starter","token":"alice:<generated-token>"}
```

**2. User authenticates with their token**:
```bash
curl http://localhost:8080/api/v1/index/list \
  -H "Authorization: <generated-token>"
```
