# 🧠 DocuMind — AI-Powered Document Q&A with RAG

> **Intelligent document question-answering powered by [Endee](https://github.com/endee-io/endee) vector database, sentence-transformers, and an optional LLM backend.**

[![Endee Vector DB](https://img.shields.io/badge/Vector%20DB-Endee-6366f1?style=flat-square)](https://github.com/endee-io/endee)
[![Python](https://img.shields.io/badge/Python-3.11-3776ab?style=flat-square&logo=python)](https://python.org)
[![FastAPI](https://img.shields.io/badge/FastAPI-0.115-009688?style=flat-square&logo=fastapi)](https://fastapi.tiangolo.com)
[![React](https://img.shields.io/badge/React-18-61dafb?style=flat-square&logo=react)](https://reactjs.org)
[![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)](LICENSE)

---

## 📋 Table of Contents

- [Project Overview](#-project-overview)
- [Problem Statement](#-problem-statement)
- [System Design](#-system-design)
- [How Endee is Used](#-how-endee-is-used)
- [Tech Stack](#-tech-stack)
- [Project Structure](#-project-structure)
- [Setup & Execution](#-setup--execution)
- [API Reference](#-api-reference)
- [LLM Configuration](#-llm-configuration)
- [Screenshots](#-screenshots)

---

## 🌟 Project Overview

**DocuMind** is a full-stack Retrieval-Augmented Generation (RAG) application that lets you upload documents (PDF, TXT, Markdown) and ask natural-language questions about them.

Instead of relying on simple keyword search, DocuMind converts every document chunk into a semantic embedding and stores it in **Endee** — a high-performance open-source vector database. When a user asks a question, the system retrieves the most contextually relevant passages from Endee and uses an LLM (or a retrieval-only fallback) to compose a grounded, accurate answer.

**Key Highlights**

| Feature | Detail |
|---|---|
| Vector Store | **Endee** (dense cosine search, INT8 quantisation) |
| Embeddings | `all-MiniLM-L6-v2` via `sentence-transformers` (384 dims) |
| LLM (optional) | OpenAI GPT-3.5/4 or Ollama (local) |
| Backend | FastAPI + Python 3.11 |
| Frontend | React 18 with live chat UI |
| Deployment | Docker Compose (Endee + backend + frontend) |

---

## 🎯 Problem Statement

Large language models hallucinate when asked about private or domain-specific documents they were never trained on. The standard solution — fine-tuning — is expensive and becomes stale as documents change.

**RAG** solves this by dynamically injecting relevant document excerpts into the LLM's context window at query time. The challenge is fast, high-quality retrieval: **DocuMind uses Endee's vector search to find semantically similar passages in milliseconds**, regardless of how large the document corpus grows.

---

## 🏗 System Design

```
┌─────────────────────────────────────────────────────────────────┐
│                         Ingestion Pipeline                       │
│                                                                  │
│  📄 Document  ──► Chunker ──► sentence-transformers ──► Endee   │
│  (PDF/TXT/MD)     (300 words    (384-dim cosine        (upsert   │
│                    + 50 overlap)  embeddings)           vectors) │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                         Query Pipeline (RAG)                     │
│                                                                  │
│  ❓ Question                                                     │
│      │                                                           │
│      ▼                                                           │
│  sentence-transformers (embed query)                             │
│      │                                                           │
│      ▼                                                           │
│  Endee.query(vector, top_k=5, filter=[doc_id])  ◄── Endee       │
│      │                                                           │
│      ▼                                                           │
│  Top-K relevant chunks + similarity scores                       │
│      │                                                           │
│      ▼                                                           │
│  LLM (OpenAI / Ollama / fallback)                                │
│      │                                                           │
│      ▼                                                           │
│  💬 Answer + cited source passages                               │
└─────────────────────────────────────────────────────────────────┘
```

### Component Responsibilities

| Component | Role |
|---|---|
| `document_processor.py` | Reads PDF/TXT files; splits text into word-based overlapping chunks |
| `embedder.py` | Wraps `sentence-transformers` — produces 384-dim float embeddings |
| `rag_engine.py` | Core RAG logic — ingest, retrieve, generate; owns all Endee interactions |
| `main.py` | FastAPI HTTP server exposing REST API |
| React frontend | Chat UI — upload, select documents, display answers with sources |
| Endee | Vector database — stores embeddings, serves similarity search |

---

## 🔷 How Endee is Used

Endee is the **sole vector store** in this project — every vector operation flows through it.

### 1. Index Creation

```python
from endee import Endee, Precision

client = Endee()          # connects to localhost:8080 by default
client.create_index(
    name="documind_knowledge_base",
    dimension=384,         # matches all-MiniLM-L6-v2 output
    space_type="cosine",   # cosine similarity
    precision=Precision.INT8,  # INT8 quantisation for faster search
)
```

### 2. Upserting Document Chunks (Ingestion)

Each text chunk is stored as a vector record with full metadata and a filterable `doc_id` field:

```python
index = client.get_index(name="documind_knowledge_base")
index.upsert([
    {
        "id":     "abc123_chunk_0",
        "vector": [0.042, -0.117, ...],   # 384-dim embedding
        "meta":   {
            "text":        "The transformer architecture was introduced…",
            "filename":    "attention_paper.pdf",
            "doc_id":      "abc123",
            "chunk_index": 0,
        },
        "filter": {"doc_id": "abc123"},   # enables per-doc filtering
    },
    # … more chunks
])
```

### 3. Semantic Search (Retrieval)

```python
query_vector = embedder.embed("What is self-attention?")

# Search across ALL documents
results = index.query(vector=query_vector, top_k=5)

# Search within a SPECIFIC document (Endee payload filter)
results = index.query(
    vector=query_vector,
    top_k=5,
    filter=[{"doc_id": {"$eq": "abc123"}}],
)
```

### 4. Vector Deletion (Document Removal)

```python
for i in range(total_chunks):
    index.delete_vector(f"{doc_id}_chunk_{i}")
```

### Endee Features Leveraged

| Endee Feature | Usage in DocuMind |
|---|---|
| Dense vector index (HNSW) | Core semantic similarity search |
| Cosine space type | Normalised embedding similarity |
| INT8 precision | Faster search with lower memory |
| Payload filtering (`$eq`) | Restrict search to a single document |
| Batch upsert | Efficient ingestion of large documents |
| `delete_vector` | Clean document removal |

---

## 🛠 Tech Stack

| Layer | Technology |
|---|---|
| Vector Database | [Endee](https://github.com/endee-io/endee) |
| Embeddings | [sentence-transformers](https://www.sbert.net/) `all-MiniLM-L6-v2` |
| Backend Framework | [FastAPI](https://fastapi.tiangolo.com) |
| PDF Parsing | [pypdf](https://pypdf.readthedocs.io) |
| LLM (optional) | OpenAI API or [Ollama](https://ollama.ai) |
| Frontend | React 18 |
| Container | Docker + Docker Compose |

---

## 📁 Project Structure

```
DocuMind/
├── backend/
│   ├── main.py                # FastAPI application & REST routes
│   ├── rag_engine.py          # Core RAG logic + all Endee interactions
│   ├── embedder.py            # sentence-transformers wrapper
│   ├── document_processor.py  # File reading + text chunking
│   ├── requirements.txt       # Python dependencies
│   ├── Dockerfile             # Backend container
│   └── .env.example           # Environment variable template
├── docker-compose.yml         # Endee + backend orchestration
├── setup.sh                   # One-command local dev launcher
└── README.md

frontend/                     # React frontend
├── src/
│   ├── App.js                 # Main chat + document management UI
│   └── App.css                # Full UI styling
└── public/
    └── index.html
```

---

## 🚀 Setup & Execution

### Prerequisites

| Tool | Version |
|---|---|
| Docker + Docker Compose | 20.10+ / v2 |
| Python | 3.11+ |
| Node.js | 18+ |

### Option 1 — Automated Script (Recommended)

```bash
git clone <your-forked-repo-url>
cd DocuMind
chmod +x setup.sh
./setup.sh
```

This starts Endee, the FastAPI backend, and the React frontend automatically.

### Option 2 — Manual Step-by-Step

#### Step 1: Start Endee

```bash
cd DocuMind
docker compose up -d endee
```

Verify Endee is running:
```bash
curl http://localhost:8080/api/v1/indexes
```

#### Step 2: Start the Backend

```bash
cd DocuMind/backend
python -m venv .venv
source .venv/bin/activate        # Windows: .venv\Scripts\activate
pip install -r requirements.txt

# Copy and edit environment variables
cp .env.example .env

# Launch API server
uvicorn main:app --reload --port 8000
```

#### Step 3: Start the Frontend

```bash
cd frontend
npm install
npm start
```

Open **http://localhost:3000** in your browser.

#### Step 4 (Optional): Full Docker Stack

```bash
cd DocuMind
docker compose up --build
```

> The backend service depends on Endee and will wait for it to be healthy before starting.

### Environment Variables (`backend/.env`)

| Variable | Default | Description |
|---|---|---|
| `ENDEE_BASE_URL` | `http://localhost:8080/api/v1` | Endee server URL |
| `ENDEE_AUTH_TOKEN` | *(empty)* | Auth token (if Endee auth is enabled) |
| `OPENAI_API_KEY` | *(empty)* | OpenAI key — enables GPT answer generation |
| `OPENAI_MODEL` | `gpt-3.5-turbo` | OpenAI model to use |
| `OLLAMA_BASE_URL` | *(empty)* | Ollama URL — enables local LLM generation |
| `OLLAMA_MODEL` | `llama3` | Ollama model name |

> **Note:** If neither `OPENAI_API_KEY` nor `OLLAMA_BASE_URL` is set, DocuMind runs in **retrieval-only mode** — it returns the retrieved document passages directly, which is useful for evaluation without any API keys.

---

## 📡 API Reference

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/health` | Liveness check |
| `GET` | `/documents` | List all ingested documents |
| `POST` | `/upload` | Upload a file (multipart/form-data) |
| `DELETE` | `/documents/{doc_id}` | Delete a document and its vectors |
| `POST` | `/query` | Ask a question (RAG) |

### Query Request Body

```json
{
  "question": "What is the main contribution of this paper?",
  "top_k": 5,
  "doc_id": "abc123"   // optional — omit to search all documents
}
```

### Query Response

```json
{
  "question": "What is the main contribution of this paper?",
  "answer": "The paper introduces the Transformer architecture, which relies entirely on attention mechanisms…",
  "sources": [
    {
      "text": "We propose a new simple network architecture, the Transformer…",
      "filename": "attention_paper.pdf",
      "chunk_index": 2,
      "similarity": 0.9312
    }
  ]
}
```

Interactive API docs available at **http://localhost:8000/docs** (Swagger UI).

---

## 🤖 LLM Configuration

DocuMind supports three modes:

### Mode 1: OpenAI (Cloud)
Set `OPENAI_API_KEY` in `backend/.env`. Uses `gpt-3.5-turbo` by default.

### Mode 2: Ollama (Local, fully private)
1. Install [Ollama](https://ollama.ai)
2. Pull a model: `ollama pull llama3`
3. Set `OLLAMA_BASE_URL=http://localhost:11434` in `backend/.env`

### Mode 3: Retrieval-only (No API key needed)
Leave both LLM variables unset. DocuMind returns the retrieved passages from Endee as the answer — useful for demos and evaluation.

---

## 🖥 Screenshots

### Upload & Chat Interface
The left sidebar shows ingested documents (each with chunk count). The main area is a chat interface where you can ask questions and see AI answers with source citations.

### Source Citations
Each AI answer includes expandable source cards showing the exact passage retrieved from Endee, along with the similarity score.

---

## 🔗 Mandatory Repository Steps

> As required by the evaluation guidelines:
> 1. ⭐ **Star** the official [endee-io/endee](https://github.com/endee-io/endee) repository
> 2. 🍴 **Fork** it to your personal GitHub account
> 3. 🏗 **Build on the fork** — this project is built on top of the forked Endee repository

---

## 📄 License

MIT — see [LICENSE](LICENSE)
