# 🧠 EndeeRAG — Production-Grade RAG System

<div align="center">

**Intelligent Document Q&A powered by Endee Vector Database**

*Hybrid Search (Dense + Sparse + RRF) · Client-Side Encryption · Live Benchmarks · Conversation Memory*

[![Python](https://img.shields.io/badge/Python-3.9+-blue.svg)](https://python.org)
[![Endee](https://img.shields.io/badge/Endee-Vector%20DB-purple.svg)](https://endee.io)
[![Streamlit](https://img.shields.io/badge/Streamlit-UI-red.svg)](https://streamlit.io)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)

</div>

---

## 📋 Problem Statement

Organizations struggle to efficiently extract answers from large document collections. Traditional keyword search misses semantic meaning, while pure vector search misses exact terminology. There's a need for a system that combines both approaches with production-grade features like encryption, performance monitoring, and conversation awareness.

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    EndeeRAG Architecture                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  📄 PDF Upload                                                  │
│       │                                                         │
│       ▼                                                         │
│  ┌──────────┐    ┌──────────────┐    ┌────────────────────┐    │
│  │  Parse    │───▶│   Chunk      │───▶│  Embed (Dense +   │    │
│  │  (PyMuPDF)│    │ (512 tokens  │    │  Sparse BM25)     │    │
│  │           │    │  + 50 overlap│    │                    │    │
│  └──────────┘    └──────────────┘    └─────────┬──────────┘    │
│                                                 │               │
│                                    ┌────────────▼────────────┐  │
│                                    │  🔐 Client-Side         │  │
│                                    │  Encryption (AES-128)   │  │
│                                    └────────────┬────────────┘  │
│                                                 │               │
│                                    ┌────────────▼────────────┐  │
│                                    │  Endee Vector Database  │  │
│                                    │  ┌──────────────────┐   │  │
│                                    │  │ Hybrid Index      │   │  │
│                                    │  │ • Dense: 384-dim  │   │  │
│                                    │  │ • Sparse: BM25    │   │  │
│                                    │  │ • Filters: $eq,$in│   │  │
│                                    │  └──────────────────┘   │  │
│                                    └────────────┬────────────┘  │
│                                                 │               │
│  ┌──────────────────────────────────────────────┘               │
│  │                                                              │
│  ▼                                                              │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              Hybrid Retrieval                            │    │
│  │  ┌─────────┐  ┌─────────┐  ┌──────────────────┐       │    │
│  │  │ Dense   │  │ Sparse  │  │ RRF Fusion       │       │    │
│  │  │ Search  │+▶│ Search  │+▶│ (Server-side)    │       │    │
│  │  │         │  │ (BM25)  │  │                  │       │    │
│  │  └─────────┘  └─────────┘  └────────┬─────────┘       │    │
│  └──────────────────────────────────────┘                  │    │
│                                         │                  │    │
│                            ┌────────────▼──────────────┐   │    │
│                            │  Google Gemini LLM        │   │    │
│                            │  (Context + Citations)    │   │    │
│                            └────────────┬──────────────┘   │    │
│                                         │                  │    │
│                            ┌────────────▼──────────────┐   │    │
│                            │  Streamlit UI             │   │    │
│                            │  • Chat Interface         │   │    │
│                            │  • Live Dashboard         │   │    │
│                            │  • Document Manager       │   │    │
│                            └───────────────────────────┘   │    │
└─────────────────────────────────────────────────────────────────┘
```

## ⭐ Why Endee?

| Feature | Endee | Other Vector DBs |
|---|---|---|
| **Native Hybrid Search** | ✅ Built-in RRF fusion | ❌ Requires custom impl |
| **BM25 Sparse Model** | ✅ `endee_bm25` server-side IDF | ❌ Manual BM25 setup |
| **Metadata Filtering** | ✅ `$eq`, `$in`, `$range` operators | ⚠️ Limited operators |
| **Quantization** | ✅ 5 precision levels (BINARY→FLOAT32) | ⚠️ Limited options |
| **Performance** | ✅ HNSW, sub-100ms queries | ⚠️ Varies |
| **Easy Setup** | ✅ `pip install endee` + Docker | ⚠️ Complex setup |

## 🔧 Tech Stack

| Component | Technology |
|---|---|
| Vector Database | **Endee** (hybrid index with `endee_bm25`) |
| Dense Embeddings | `all-MiniLM-L6-v2` (384-dim, sentence-transformers) |
| Sparse Embeddings | `endee/bm25` (via endee-model) |
| LLM | Google Gemini 2.0 Flash |
| UI | Streamlit + Plotly |
| PDF Parsing | PyMuPDF |
| Encryption | Fernet (AES-128-CBC) |
| Chunking | tiktoken (512 tokens + 50 overlap) |

## 🚀 Setup Instructions

### Prerequisites

- Python 3.9+
- Docker Desktop (for Endee server)
- Google API Key (for Gemini LLM)

### 1. Start Endee Server

```bash
docker run -p 8080:8080 -v ./endee-data:/data --name endee-server endeeio/endee-server:latest
```

### 2. Install Dependencies

```bash
cd project
pip install -r requirements.txt
```

### 3. Configure Environment

```bash
cp .env.example .env
# Edit .env and add your GOOGLE_API_KEY
```

### 4. Run the Application

```bash
streamlit run app.py
```

The app will open at `http://localhost:8501`

## 📂 Project Structure

```
project/
├── app.py              # Streamlit UI (chat, upload, dashboard)
├── ingest.py           # PDF → Parse → Chunk → Embed → Store
├── retriever.py        # Hybrid search (Dense + Sparse + RRF)
├── rag.py              # RAG pipeline with LLM + citations
├── benchmarks.py       # Latency & accuracy benchmarking
├── config.py           # Centralized configuration
├── encryption.py       # Client-side AES encryption
├── requirements.txt    # Python dependencies
├── .env.example        # Environment variable template
├── handoff.md          # Project handoff document
└── README.md           # This file
```

## 🎯 Features

### Core RAG Pipeline
- **PDF Ingestion**: Parse → Chunk (512 tokens, 50 overlap) → Embed → Store
- **Hybrid Search**: Dense + Sparse + RRF fusion via Endee
- **LLM Generation**: Context-aware answers with Google Gemini
- **Citation Support**: Every answer includes `[Source N]` references

### Metadata Filtering
- **Document Filter**: Search within specific documents using `$eq`
- **Multi-Document**: Search across selected docs using `$in`
- **Filter Fields**: `doc_hash`, `filename` stored as Endee filter fields

### WOW Features

#### 🔐 1. Client-Side Encryption
- AES-128-CBC encryption via Fernet before data leaves the client
- Documents are encrypted before storing in Endee
- Transparent decryption on retrieval
- Toggle on/off from the UI

#### 📊 2. Live Performance Dashboard
- Real-time latency tracking per query
- Plotly charts showing retrieval vs generation time trends
- Benchmark runner comparing all three search modes
- Exportable results

#### 💬 3. Conversation Memory
- Multi-turn context-aware conversations
- Previous Q&A pairs included in LLM prompt
- Configurable memory window (last 10 turns)
- Clear history option

## 📊 Performance Benchmarks

### Search Latency Comparison

| Mode | Avg Latency | Description |
|---|---|---|
| **Dense** | ~50-80ms | Semantic similarity (all-MiniLM-L6-v2) |
| **Sparse** | ~30-60ms | BM25 keyword matching |
| **Hybrid** | ~60-100ms | Dense + Sparse with RRF fusion |

### RAG Pipeline Breakdown

| Stage | Avg Time |
|---|---|
| Query Embedding | ~15ms |
| Endee Retrieval | ~50ms |
| LLM Generation | ~800-1500ms |
| Total E2E | ~900-1600ms |

*Benchmarks run on local Docker deployment. Results vary by hardware and data volume.*

## 🔄 How It Works

1. **Upload**: Drop a PDF into the Streamlit UI
2. **Ingest**: PDF is parsed (PyMuPDF), chunked (512 tokens), embedded (dense + sparse), optionally encrypted, and stored in Endee
3. **Query**: Type a question in the chat
4. **Retrieve**: Hybrid search finds the top-K most relevant chunks
5. **Generate**: Gemini LLM constructs an answer using retrieved context
6. **Display**: Answer shown with citations, performance metrics, and source links

## 🔮 Future Improvements

- [ ] Multi-modal RAG (images + tables from PDFs)
- [ ] Re-ranking with cross-encoder models
- [ ] Streaming LLM responses
- [ ] Document versioning
- [ ] Role-based access control
- [ ] Auto-chunking strategy selection
- [ ] Evaluation with RAGAS framework
- [ ] Deployment to cloud (Render/Railway)

## 📄 License

Apache License 2.0 — see the Endee repository for full terms.

---

<div align="center">

**Built with ❤️ for the Endee AI/ML Internship Evaluation**

[Endee.io](https://endee.io) · [Docs](https://docs.endee.io) · [GitHub](https://github.com/endee-io/endee)

</div>
