# 🎓 AcademIQ — AI-Powered Academic Research Assistant

> Built on top of the [Endee](https://github.com/endee-io/endee) high-performance
> vector database. Lives inside the forked `endee-io/endee` repository as `academiq/`.

![Python](https://img.shields.io/badge/Python-3.11-3776ab?style=flat-square&logo=python&logoColor=white)
![FastAPI](https://img.shields.io/badge/FastAPI-0.115-009688?style=flat-square&logo=fastapi&logoColor=white)
![Endee](https://img.shields.io/badge/Endee-SDK_0.1.3-6366f1?style=flat-square)
![Docker](https://img.shields.io/badge/Docker-Compose-2496ed?style=flat-square&logo=docker&logoColor=white)
![License](https://img.shields.io/badge/License-Apache_2.0-green?style=flat-square)

---

## 🎯 Problem Statement

Academic researchers today are drowning in information. A typical graduate student must sift through hundreds of PDF papers, lecture notes, and research articles just to answer a single focused question — and keyword search fails them entirely because it cannot understand *meaning*, only exact text matches. The result is wasted hours, missed connections across papers, and research bottlenecks that stall progress.

**AcademIQ** solves this by combining Endee's blazing-fast vector database with sentence-level semantic embeddings and a Retrieval-Augmented Generation (RAG) pipeline. Instead of keyword matching, AcademIQ understands the *concept* behind your query, finds the most relevant passages across all your uploaded documents, and feeds them to an LLM to generate a precise, grounded answer — with citations. Whether you're studying for exams, conducting literature reviews, or exploring a new domain, AcademIQ transforms your document collection into an intelligent research assistant.

---

## 🏗️ System Architecture

```
User Browser (port 3000)
        │
        │  HTTP / fetch()
        ▼
┌────────────────────────────────────────────────────────┐
│              Nginx Frontend (port 3000)                │
│   index.html + style.css + app.js                     │
│   3 tabs: Upload │ Search │ Ask (RAG)                 │
└───────────────────────┬────────────────────────────────┘
                        │  REST API calls
                        ▼
┌────────────────────────────────────────────────────────┐
│           FastAPI Backend (port 8000)                  │
│                                                        │
│  ┌─────────────────────────────────────────────────┐  │
│  │  Document Processor                             │  │
│  │  PyMuPDF extraction + sentence-aware chunking   │  │
│  └───────────────────┬─────────────────────────────┘  │
│                      │                                 │
│  ┌─────────────────────────────────────────────────┐  │
│  │  Embedding Engine                               │  │
│  │  sentence-transformers all-MiniLM-L6-v2 (384d) │  │
│  └───────────────────┬─────────────────────────────┘  │
│                      │                                 │
│  ┌─────────────────────────────────────────────────┐  │
│  │  RAG Pipeline                                   │  │
│  │  Ingest │ Search │ LLM answer generation         │  │
│  └───────────────────┬─────────────────────────────┘  │
│                      │                                 │
│  ┌─────────────────────────────────────────────────┐  │
│  │  Endee Python SDK  (pip install endee)           │  │
│  │  EndeeManager: create_index, upsert, query      │  │
│  └───────────────────┬─────────────────────────────┘  │
└──────────────────────┼─────────────────────────────────┘
                       │  HTTP (official Python SDK)
                       ▼
┌────────────────────────────────────────────────────────┐
│      Endee Vector Database (port 8080)                 │
│      endeeio/endee-server:latest via Docker            │
│      Index: academiq_docs (384-dim, cosine, INT8)      │
└────────────────────────────────────────────────────────┘
```

---

## 🔧 How Endee Is Used

### Why Endee?

| Feature | Detail |
|---------|--------|
| **Performance** | Scales to 1 billion vectors per node |
| **Open Source** | Apache 2.0 licensed, self-hostable |
| **Docker-Ready** | `endeeio/endee-server:latest` is the entire deployment |
| **Official Python SDK** | `pip install endee` — clean API, no raw HTTP needed |
| **Precision Options** | INT8 quantization cuts memory usage ~4× vs FP32 |

### Index Schema

```python
from endee import Endee, Precision

client = Endee()
client.set_base_url("http://localhost:8080/api/v1")

client.create_index(
    name      = "academiq_docs",
    dimension = 384,          # all-MiniLM-L6-v2 output size
    space_type= "cosine",     # normalized similarity
    precision = Precision.INT8  # memory-efficient storage
)
```

### Ingestion

```python
index = client.get_index(name="academiq_docs")
index.upsert([
    {
        "id":     "abc123_0",                        # deterministic MD5+index ID
        "vector": [...],                             # 384 floats from MiniLM
        "meta":   {"text": "...", "doc_name": "...", "chunk_index": 0},
        "filter": {"doc_name": "machine_learning/..."}  # for filtered queries
    }
])
```

### Similarity Search

```python
results = index.query(
    vector = query_embedding,   # 384-dim query vector
    top_k  = 5,
    ef     = 128                # HNSW search parameter
)
# Each result: {"id": ..., "similarity": 0.87, "meta": {...}}

# Optional filtered search (Bonus B):
results = index.query(
    vector = query_embedding,
    top_k  = 5,
    ef     = 128,
    filter = [{"doc_name": {"$eq": "machine_learning/ml_transformers"}}]
)
```

---

## 🚀 Quick Start (Docker — Recommended)

### Prerequisites
- Docker Desktop running
- 4 GB free RAM (for embedding model)

```bash
# 1. Navigate to the academiq folder (inside the forked endee repo)
cd path/to/endee/academiq/

# 2. Copy environment template
cp .env.example .env
# Edit .env if you want OpenAI GPT-4o-mini (optional)
# Leave OPENAI_API_KEY blank to use free local Flan-T5

# 3. Start all services
docker compose up --build

# First run downloads the embedding model (~90MB) — wait ~60s
# Services:
#   Endee:    http://localhost:8080
#   Backend:  http://localhost:8000  (API docs: http://localhost:8000/docs)
#   Frontend: http://localhost:3000

# 4. Open the UI
# Visit http://localhost:3000 in your browser
```

---

## 🌱 Seed Sample Data

After services are running, seed 25 pre-written academic chunks across 5 domains:

```bash
docker exec academiq-backend python scripts/seed_sample_data.py
```

**Domains seeded:**
- 🧠 Machine Learning (transformers, backprop, CNN, RL, overfitting)
- ⚛️ Quantum Computing (qubits, entanglement, Shor's algorithm, error correction)
- 🌍 Climate Science (carbon cycle, tipping points, ocean, mitigation)
- 🔬 Neuroscience (plasticity, memory, neurotransmitters, neurogenesis)
- 🧬 Bioinformatics (sequencing, AlphaFold, CRISPR, metagenomics, GWAS)

---

## 💻 Manual Setup (Without Docker)

### 1. Start Endee

```bash
# Pull and run Endee server (from the forked repo root, or separately)
docker run -p 8080:8080 endeeio/endee-server:latest
```

### 2. Backend

```bash
cd academiq/backend/
python -m venv venv && source venv/bin/activate  # Windows: venv\Scripts\activate
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload
```

### 3. Frontend

Open `academiq/frontend/index.html` directly in your browser, **or** serve it:

```bash
# Option A: Python simple server
cd academiq/frontend/
python -m http.server 3000

# Option B: npx serve
npx serve academiq/frontend/ -p 3000
```

### 4. Seed data

```bash
cd academiq/
python scripts/seed_sample_data.py
```

---

## 📡 API Reference

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET`  | `/api/health` | Endee connectivity + index stats |
| `POST` | `/api/ingest` | Ingest PDF (multipart) or text (form) |
| `POST` | `/api/search` | Semantic vector search |
| `POST` | `/api/query`  | RAG question answering |
| `GET`  | `/api/indexes` | List all Endee indexes with stats *(Bonus A)* |
| `GET`  | `/api/vector/{id}` | Retrieve stored vector by ID *(Bonus D)* |

### Example Requests

```bash
# Health check
curl http://localhost:8000/api/health

# Ingest text
curl -X POST http://localhost:8000/api/ingest \
  -F "text=Neural networks learn hierarchical representations." \
  -F "doc_name=ml_notes"

# Ingest PDF
curl -X POST http://localhost:8000/api/ingest \
  -F "file=@paper.pdf" -F "doc_name=my_paper"

# Semantic search
curl -X POST http://localhost:8000/api/search \
  -H "Content-Type: application/json" \
  -d '{"query": "attention mechanism transformers", "top_k": 3}'

# Filtered search (Bonus B)
curl -X POST http://localhost:8000/api/search \
  -H "Content-Type: application/json" \
  -d '{"query": "transformer", "top_k": 3, "doc_name_filter": "machine_learning/ml_transformers"}'

# RAG Q&A
curl -X POST http://localhost:8000/api/query \
  -H "Content-Type: application/json" \
  -d '{"question": "How does backpropagation work?"}'
```

---

## 🎬 Demo Walkthrough

```
Step 1: Open http://localhost:3000
        └─ Green dot confirms "Endee Connected"

Step 2: Upload tab → Run seed first, OR paste text → click "Ingest Document"
        └─ Success card shows: "✓ Ingested N chunks from [doc_name]"

Step 3: Search tab → type "attention mechanism" → click Search
        └─ Result cards appear with animated similarity score bars

Step 4: Search tab → set "Filter by doc name" to filter to one document
        └─ Demonstrates Endee's filter parameter (Bonus B)

Step 5: Ask tab → type "How does backpropagation compute gradients?"
        └─ AI answer appears, grounded in retrieved document passages

Step 6: Expand "📎 Sources Used (N)" accordion
        └─ See exactly which chunks from which documents grounded the answer
```

---

## 🔬 Technical Details

### RAG Pipeline Flow

```
1. INGEST
   PDF/Text → PyMuPDF extract → clean whitespace
   → sentence-aware chunking (450 chars, 60-char word overlap)
   → batch embed (MiniLM, 384-dim) → Endee upsert with meta+filter

2. SEARCH
   Query string → MiniLM embed (384-dim)
   → Endee index.query(vector, top_k=5, ef=128)
   → ranked list [{id, similarity, meta}]

3. RAG QUERY
   Question → semantic_search (step 2)
   → build numbered context string from top-K passages
   → structured prompt → LLM → answer with source citations
```

### Embedding Model

| Property | Value |
|----------|-------|
| Model | `all-MiniLM-L6-v2` |
| Dimensions | 384 |
| Similarity | Cosine (L2-normalized) |
| Size | ~22 MB |
| Inference | CPU (GPU optional via CUDA) |
| Source | sentence-transformers (HuggingFace) |

### Chunking Strategy

| Parameter | Value |
|-----------|-------|
| Chunk size | 450 characters |
| Overlap | 60 words (shared with next chunk) |
| Boundary detection | Sentence-end punctuation (`.!?`) |
| ID generation | MD5(doc_name+idx)[:16] + `_idx` |

### LLM Configuration

| Mode | Model | Cost |
|------|-------|------|
| With `OPENAI_API_KEY` | GPT-4o-mini | ~$0.001/query |
| Without key (default) | google/flan-t5-base | Free, local |

---

## 📁 Project Structure

```
academiq/                        ← All AcademIQ code (inside forked endee repo)
├── backend/
│   ├── app/
│   │   ├── __init__.py          ← Package marker
│   │   ├── main.py              ← FastAPI app + CORS + bonus routes
│   │   ├── config.py            ← Pydantic settings from env
│   │   ├── endee_client.py      ← Official Endee SDK integration ★
│   │   ├── embeddings.py        ← Local MiniLM sentence embeddings
│   │   ├── rag_pipeline.py      ← Ingest + search + RAG orchestrator
│   │   ├── document_processor.py← PDF/text extraction & chunking
│   │   └── routes/
│   │       ├── __init__.py
│   │       ├── ingest.py        ← POST /api/ingest
│   │       ├── search.py        ← POST /api/search (+ filter support)
│   │       └── query.py         ← POST /api/query (RAG)
│   ├── requirements.txt         ← Python deps including endee==0.1.3
│   └── Dockerfile               ← Pre-downloads embedding model
├── frontend/
│   ├── index.html               ← 3-tab SPA
│   ├── style.css                ← Premium dark UI (Inter font, navy/blue)
│   └── app.js                   ← fetch() API calls, toast, drag-drop
├── scripts/
│   └── seed_sample_data.py      ← Seeds 25 academic chunks into Endee
├── docker-compose.yml           ← Starts Endee + backend + Nginx frontend
├── .env.example                 ← Template for ENDEE_BASE_URL, OPENAI_API_KEY
└── README.md                    ← This file
```

---

## 🎁 Bonus Features Implemented

| Bonus | Feature | Implementation |
|-------|---------|----------------|
| **A** | `GET /api/indexes` — list all Endee indexes | `main.py` `/api/indexes` route |
| **B** | Filtered search by `doc_name` | `search.py` + Endee `filter` param |
| **D** | `GET /api/vector/{id}` — inspect stored vector | `main.py` + `endee_client.get_vector()` |
| **E** | Index stats card in frontend header | Health check shows `dim` + `vectors` count |

---

## ⭐ Acknowledgements

AcademIQ is built on the **Endee** open-source, high-performance vector database.

- **GitHub**: [https://github.com/endee-io/endee](https://github.com/endee-io/endee)
- **Docs**: [https://docs.endee.io](https://docs.endee.io)
- **Python SDK**: `pip install endee` ([PyPI](https://pypi.org/project/endee/))
- **License**: Apache 2.0

---

*AcademIQ was built as a demonstration of the Endee vector database for academic RAG use cases.*
