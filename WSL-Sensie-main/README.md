# WSL Sensei 🥋
### AI Assistant for Windows + WSL Development

> **"Know your environment. Command it."**
>
> WSL Sensei indexes your developer environment — shell history, config files,
> scripts, and logs — then lets you query it in plain English using
> **vector embeddings**, **semantic search via Endee**, and a
> **RAG (Retrieval-Augmented Generation)** pipeline.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Problem Statement](#2-problem-statement)
3. [System Architecture](#3-system-architecture)
4. [Endee Vector Database](#4-endee-vector-database)
5. [Setup Instructions](#5-setup-instructions)
6. [Indexing Your Environment](#6-indexing-your-environment)
7. [Querying – CLI](#7-querying--cli)
8. [Querying – REST API](#8-querying--rest-api)
9. [Example Queries](#9-example-queries)
10. [Running Tests](#10-running-tests)
11. [Project Structure](#11-project-structure)
12. [Configuration Reference](#12-configuration-reference)
13. [Future Improvements](#13-future-improvements)

---

## 1. Project Overview

WSL Sensei is a **local, privacy-first developer utility** that builds a
semantic memory of your development environment and answers questions about it
in natural language.

It combines three modern AI primitives:

| Primitive | Technology used |
|---|---|
| **Vector Embeddings** | `sentence-transformers/all-MiniLM-L6-v2` (local, no API key) |
| **Semantic Vector Search** | **Endee** – open-source, high-performance vector database |
| **Answer Generation (RAG)** | Ollama (local) / OpenAI / Anthropic Claude |

### What gets indexed

| Source | Description |
|---|---|
| `~/.bash_history` | Every Bash command you have ever run |
| `~/.zsh_history` | Zsh history (auto-detected) |
| PowerShell history | PSReadLine `ConsoleHost_history.txt` (via `/mnt/c/`) |
| Config files | `.bashrc`, `.zshrc`, `.gitconfig`, `nginx.conf`, `.env` … |
| Project scripts | `Makefile`, `startup.sh`, `docker-compose.yml` … |
| Logs | Application and system log files |

---

## 2. Problem Statement

Windows + WSL developers constantly context-switch between two operating
systems, dozens of config files, project scripts, and shell histories.

Common pain points:

* **"How did I start this project last week?"** — buried in bash history
* **"Where is the nginx config?"** — could be in five different places
* **"Why is port 3000 already in use?"** — need to grep through logs
* **"Which command installs the dependencies?"** — check three Makefiles
* **"What was that `docker` one-liner?"** — lost in PowerShell history

WSL Sensei solves this by building a **queryable semantic index** of your
entire development environment so you can ask questions naturally instead of
grepping files manually.

---

## 3. System Architecture

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                        WSL SENSEI – SYSTEM ARCHITECTURE                     ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                              ║
║  ┌─────────────────────────────────────────────────────────────────────┐    ║
║  │                      INDEXING PIPELINE                               │    ║
║  │                                                                      │    ║
║  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐  │    ║
║  │  │ bash_history │  │  ps_history  │  │    config_file_scanner   │  │    ║
║  │  │  collector   │  │  collector   │  │ (.conf .sh .yml .env …)  │  │    ║
║  │  └──────┬───────┘  └──────┬───────┘  └───────────┬──────────────┘  │    ║
║  │         │                 │                       │                  │    ║
║  │         └─────────────────┴───────────────────────┘                 │    ║
║  │                                  │                                   │    ║
║  │                                  ▼                                   │    ║
║  │                    ┌─────────────────────────┐                      │    ║
║  │                    │   chunker.py             │                      │    ║
║  │                    │  CharacterChunker /       │                      │    ║
║  │                    │  LineChunker              │                      │    ║
║  │                    └────────────┬────────────┘                      │    ║
║  │                                 │ list[Chunk]                        │    ║
║  │                                 ▼                                    │    ║
║  │                    ┌─────────────────────────┐                      │    ║
║  │                    │   embed_store.py          │                      │    ║
║  │                    │  sentence-transformers    │                      │    ║
║  │                    │  (all-MiniLM-L6-v2)       │                      │    ║
║  │                    │  → 384-dim float vectors  │                      │    ║
║  │                    └────────────┬────────────┘                      │    ║
║  └─────────────────────────────────│────────────────────────────────────┘    ║
║                                    │ index.upsert(id, vector, meta)          ║
║                                    ▼                                         ║
║  ╔═══════════════════════════════════════════════════════════════════╗        ║
║  ║               ENDEE VECTOR DATABASE  (port 8080)                  ║        ║
║  ║                                                                   ║        ║
║  ║   Index: wsl_sensei  │  dimension=384  │  space=cosine            ║        ║
║  ║   Precision: INT8    │  HNSW indexing  │  up to 1B vectors        ║        ║
║  ╚═══════════════════════════════════════════════════════════════════╝        ║
║                                    │                                         ║
║  ┌─────────────────────────────────│────────────────────────────────────┐    ║
║  │                       QUERY PIPELINE                                 │    ║
║  │                                 │                                    │    ║
║  │  User question                  │ index.query(vector, top_k)        │    ║
║  │      │                          │                                    │    ║
║  │      ▼                          ▼                                    │    ║
║  │  embed_text()   ──────►  semantic_search.py                         │    ║
║  │  (same model)           list[SearchResult] (ranked by similarity)   │    ║
║  │                                 │                                    │    ║
║  │                                 ▼                                    │    ║
║  │                       build_context()                               │    ║
║  │                       (format top-k chunks as prompt context)       │    ║
║  │                                 │                                    │    ║
║  │                                 ▼                                    │    ║
║  │                    ┌────────────────────────┐                       │    ║
║  │                    │      LLM Backend        │                       │    ║
║  │                    │  Ollama / OpenAI /       │                       │    ║
║  │                    │  Anthropic / mock        │                       │    ║
║  │                    └────────────┬───────────┘                       │    ║
║  │                                 │                                    │    ║
║  │                                 ▼                                    │    ║
║  │                          RAGResponse                                │    ║
║  │                   (answer + sources + latency)                      │    ║
║  │                                 │                                    │    ║
║  │              ┌──────────────────┴────────────────┐                  │    ║
║  │              ▼                                    ▼                  │    ║
║  │        CLI (sensei_cli.py)           REST API (FastAPI :8000)       │    ║
║  └──────────────────────────────────────────────────────────────────────┘    ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

### Data flow summary

```
Raw files  →  Collectors  →  Chunker  →  Embedder  →  Endee (store)
                                                            │
User query →  Embedder  →  Endee (search)  →  LLM  →  Answer
```

---

## 4. Endee Vector Database

[Endee](https://github.com/endee-io/endee) is a high-performance, open-source
vector database designed to handle up to **1 billion vectors on a single node**.

### Why Endee for WSL Sensei?

| Feature | Benefit |
|---|---|
| **Runs locally via Docker** | No cloud dependency, data stays on your machine |
| **HNSW indexing** | Sub-millisecond approximate nearest-neighbour search |
| **INT8 quantisation** | 4× memory reduction with minimal accuracy loss |
| **Python SDK** (`pip install endee`) | Clean, minimal API |
| **Cosine similarity** | Perfect for text embeddings |
| **Open-source (Apache 2.0)** | Free to use and modify |

### How WSL Sensei uses Endee

**1. Index creation** (`indexing/embed_store.py`)
```python
from endee import Endee, Precision

client = Endee()                     # connects to localhost:8080
client.create_index(
    name="wsl_sensei",
    dimension=384,                   # matches all-MiniLM-L6-v2
    space_type="cosine",
    precision=Precision.INT8,        # memory-efficient
)
```

**2. Upserting vectors**
```python
index = client.get_index(name="wsl_sensei")
index.upsert([
    {
        "id":     "chunk_a3f9d2c1",
        "vector": [0.12, -0.05, ...],   # 384 floats
        "meta": {
            "text":     "npm install && npm run dev",
            "source":   "/home/user/.bash_history",
            "doc_type": "bash_history",
        }
    }
])
```

**3. Semantic search at query time**
```python
query_vector = embed_text("how do I install dependencies?")
results = index.query(vector=query_vector, top_k=6)
# → returns chunks ranked by cosine similarity
```

The metadata stored alongside each vector (`text`, `source`, `doc_type`) is
retrieved with the search results and assembled into the RAG context window.

---

## 5. Setup Instructions

### Prerequisites

| Requirement | Version |
|---|---|
| Python | ≥ 3.11 |
| Docker + Docker Compose | 20.10 + v2 |
| RAM | ≥ 4 GB |

### Step 1 – Start Endee

```bash
# Option A: Docker Hub image (recommended – no build needed)
mkdir endee && cd endee
cat > docker-compose.yml << 'EOF'
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
EOF

docker compose up -d
```

Verify it's running:
```bash
curl http://localhost:8080/api/v1/index/list
# → {"indexes": []}
```

### Step 2 – Clone and configure WSL Sensei

```bash
git clone https://github.com/your-org/wsl-sensei.git
cd wsl-sensei

# Copy environment config
cp .env.example .env
```

Edit `.env` and set your preferred LLM backend:

```dotenv
# Free / local (recommended for getting started)
LLM_BACKEND=mock       # no LLM required – shows raw context

# OR: Ollama (free, local LLM)
LLM_BACKEND=ollama
OLLAMA_MODEL=llama3.2   # run: ollama pull llama3.2

# OR: OpenAI
LLM_BACKEND=openai
OPENAI_API_KEY=sk-...

# OR: Anthropic
LLM_BACKEND=anthropic
ANTHROPIC_API_KEY=sk-ant-...
```

### Step 3 – Install Python dependencies

```bash
python3 -m venv .venv
source .venv/bin/activate       # Windows: .venv\Scripts\activate

pip install -r requirements.txt
```

> **Note:** The `sentence-transformers` model (`all-MiniLM-L6-v2`, ~90 MB)
> downloads automatically on first run.

### Step 4 – Index your environment

```bash
# Index everything (bash history + config files)
python scripts/index_environment.py

# OR use sample data for a quick demo
python scripts/index_environment.py --sample
```

### Step 5 – Ask a question

```bash
# CLI
python -m cli.sensei_cli ask "How do I start my project?"

# Interactive chat
python -m cli.sensei_cli chat

# REST API
uvicorn api.server:app --port 8000 --reload
curl -X POST http://localhost:8000/ask \
  -H "Content-Type: application/json" \
  -d '{"question": "Where is the nginx configuration?"}'
```

---

## 6. Indexing Your Environment

```bash
# Index everything
python scripts/index_environment.py

# Index only specific sources
python scripts/index_environment.py --sources bash_history config_files

# Force a clean rebuild (deletes and recreates the Endee index)
python scripts/index_environment.py --rebuild

# Dry-run: collect and chunk, but DO NOT write to Endee
python scripts/index_environment.py --dry-run

# Use bundled sample data (no real files needed)
python scripts/index_environment.py --sample
```

Re-index whenever you want to pick up new shell history or changed config files.

---

## 7. Querying – CLI

```bash
# Single question
python -m cli.sensei_cli ask "How do I start my project?"
python -m cli.sensei_cli ask "Where is the nginx config?" --top-k 8
python -m cli.sensei_cli ask "Which command installs deps?" --backend mock

# Raw semantic search (no LLM)
python -m cli.sensei_cli search "port 3000"
python -m cli.sensei_cli search "npm install" --filter-type bash_history

# Status check
python -m cli.sensei_cli status

# Interactive REPL
python -m cli.sensei_cli chat
# Inside chat:
#   /search <query>   – raw vector search
#   /status           – index stats
#   exit              – quit
```

---

## 8. Querying – REST API

Start the server:
```bash
uvicorn api.server:app --host 0.0.0.0 --port 8000 --reload
```

### `POST /ask` – Full RAG pipeline

```bash
curl -X POST http://localhost:8000/ask \
  -H "Content-Type: application/json" \
  -d '{
    "question": "How do I start my project?",
    "top_k": 6,
    "backend": "ollama"
  }'
```

Response:
```json
{
  "question": "How do I start my project?",
  "answer": "Based on your environment, you can start the project by running...",
  "sources": [
    {
      "id": "chunk_a3f9...",
      "score": 0.91,
      "source": "/home/user/.bash_history",
      "doc_type": "bash_history",
      "text": "cd ~/projects/my-app\nnpm install\nnpm run dev"
    }
  ],
  "backend": "ollama",
  "latency_ms": 1234.5
}
```

### `POST /search` – Semantic search only

```bash
curl -X POST http://localhost:8000/search \
  -H "Content-Type: application/json" \
  -d '{"query": "nginx config location", "top_k": 5}'
```

### `POST /index` – Trigger re-indexing

```bash
curl -X POST http://localhost:8000/index \
  -H "Content-Type: application/json" \
  -d '{"sources": ["bash_history", "config_files"], "force_rebuild": false}'
```

### `GET /status` – Health check

```bash
curl http://localhost:8000/status
```

Interactive API docs: **http://localhost:8000/docs**

---

## 9. Example Queries

These are realistic questions WSL Sensei can answer after indexing your environment:

| Question | What it retrieves |
|---|---|
| `"How do I start my project?"` | `startup.sh`, Makefile `dev` target, bash history `npm run dev` |
| `"Where is the nginx configuration?"` | `/etc/nginx/nginx.conf`, `nginx.conf` in project |
| `"Why is port 3000 already in use?"` | Log warnings + `lsof -i :3000` history |
| `"Which command installs dependencies?"` | `npm install`, `pip install -r requirements.txt` from history |
| `"How do I kill a process on a port?"` | `killport` function in `.bashrc` |
| `"What Docker services are running?"` | `docker-compose.yml`, `docker ps` from history |
| `"How do I run database migrations?"` | `alembic upgrade head` from history / Makefile |
| `"What is the database connection string?"` | `.env`, `.bashrc` exports |
| `"How do I deploy to production?"` | Makefile `deploy` target, deploy scripts |
| `"What PostgreSQL user and db are configured?"` | `docker-compose.yml`, `.env` |

---

## 10. Running Tests

```bash
# All tests (no Endee server or LLM needed)
pytest tests/ -v

# Specific test module
pytest tests/test_chunker.py -v
pytest tests/test_collectors.py -v
pytest tests/test_rag_pipeline.py -v

# With coverage
pytest tests/ --cov=. --cov-report=term-missing
```

---

## 11. Project Structure

```
wsl-sensei/
│
├── collectors/                      # Gather raw text from the environment
│   ├── bash_history_collector.py    # ~/.bash_history + ~/.zsh_history
│   ├── powershell_history_collector.py   # PSReadLine history (via WSL /mnt/c)
│   └── config_file_scanner.py       # Config files, scripts, logs
│
├── indexing/                        # Chunk → Embed → Store
│   ├── chunker.py                   # CharacterChunker + LineChunker
│   └── embed_store.py               # sentence-transformers + Endee SDK
│
├── retrieval/                       # Query-time vector search
│   └── semantic_search.py           # embed query → Endee.query() → SearchResult[]
│
├── rag/                             # Retrieval-Augmented Generation
│   └── rag_pipeline.py              # retrieve → build_context → LLM → RAGResponse
│
├── api/                             # FastAPI REST server
│   └── server.py                    # /ask  /search  /index  /status
│
├── cli/                             # Rich terminal interface
│   └── sensei_cli.py                # ask / search / index / status / chat
│
├── scripts/
│   └── index_environment.py         # Standalone indexing entrypoint
│
├── data/
│   └── sample/                      # Demo data for testing without a real env
│       ├── bash_history.txt
│       ├── powershell_history.txt
│       ├── nginx.conf
│       ├── docker-compose.yml
│       ├── startup.sh
│       ├── .bashrc
│       ├── Makefile
│       ├── pyproject.toml
│       └── app.log
│
├── tests/
│   ├── test_chunker.py
│   ├── test_collectors.py
│   └── test_rag_pipeline.py
│
├── config.py                        # Centralised configuration + .env loading
├── requirements.txt
├── .env.example
└── README.md
```

---

## 12. Configuration Reference

All settings can be set in `.env` or as environment variables.

| Variable | Default | Description |
|---|---|---|
| `ENDEE_BASE_URL` | `http://localhost:8080/api/v1` | Endee server URL |
| `ENDEE_AUTH_TOKEN` | `` | Auth token (empty = no auth) |
| `ENDEE_INDEX_NAME` | `wsl_sensei` | Vector index name |
| `EMBEDDING_MODEL` | `all-MiniLM-L6-v2` | sentence-transformers model |
| `EMBEDDING_DIMENSION` | `384` | Must match the model |
| `LLM_BACKEND` | `ollama` | `ollama` / `openai` / `anthropic` / `mock` |
| `OLLAMA_MODEL` | `llama3.2` | Model name for Ollama |
| `OPENAI_MODEL` | `gpt-4o-mini` | Model name for OpenAI |
| `ANTHROPIC_MODEL` | `claude-3-haiku-20240307` | Model for Anthropic |
| `CHUNK_SIZE` | `300` | Characters per chunk |
| `CHUNK_OVERLAP` | `50` | Overlap between chunks |
| `RAG_TOP_K` | `6` | Chunks retrieved per query |
| `RAG_MAX_TOKENS` | `1024` | Max tokens for LLM response |

---

## 13. Future Improvements

### Short-term (days 3–7)
- [ ] **Incremental indexing** – only re-embed changed/new files (hash-based)
- [ ] **Web UI** – simple React front-end for the `/ask` endpoint
- [ ] **File-type icons** in CLI source table
- [ ] **Index multiple workspaces** – tag chunks by project

### Medium-term (weeks 2–4)
- [ ] **Git diff indexing** – answer "What did I change this week?"
- [ ] **VS Code extension** – inline question widget inside the editor
- [ ] **Windows Notification** trigger – index on shell close
- [ ] **Hybrid search** – combine BM25 keyword search with Endee vector search
- [ ] **Streaming answers** – SSE endpoint for real-time token streaming

### Long-term
- [ ] **Multi-user / team mode** – shared Endee index with per-user namespaces
- [ ] **Scheduled re-indexing** – cron job / systemd timer
- [ ] **Plugin system** – collect from more sources (Jira, Slack, GitHub PRs)
- [ ] **Fine-tuned embedding model** – domain-adapted on developer text
- [ ] **Answer citations** – highlight exact file lines in the answer

---

## License

MIT — free for personal and commercial use.

---

*Built for the Endee Vector Database internship evaluation.*
*Demonstrates: vector embeddings · semantic search · RAG pipeline · real-world developer tooling.*
