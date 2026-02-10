# Nexus - Self-Evolving AI Knowledge Network

<div align="center">

**Transform static knowledge into a living intelligence graph**

[![TypeScript](https://img.shields.io/badge/TypeScript-007ACC?style=for-the-badge&logo=typescript&logoColor=white)](https://www.typescriptlang.org/)
[![Python](https://img.shields.io/badge/Python-3776AB?style=for-the-badge&logo=python&logoColor=white)](https://www.python.org/)
[![Next.js](https://img.shields.io/badge/Next.js-000000?style=for-the-badge&logo=next.js&logoColor=white)](https://nextjs.org/)
[![FastAPI](https://img.shields.io/badge/FastAPI-009688?style=for-the-badge&logo=fastapi&logoColor=white)](https://fastapi.tiangolo.com/)

</div>

---

# Nexus — Endee-Powered Semantic Knowledge Graph Explorer

<div align="center">

### Transform Documents Into Interactive Intelligence Networks

AI-driven knowledge exploration platform that converts uploaded documents into a semantic vector space and dynamically visualizes relationships using the **Endee Vector Database**.

Built as a project submission demonstrating real-world vector search, RAG-style retrieval, and graph-based exploration.

</div>

---

# 🚀 Overview

**Nexus** is a full-stack AI web application designed to solve a fundamental problem:

> Information exists in isolation — understanding exists in connections.

Traditional document tools provide keyword search.
Nexus provides:

* Semantic similarity search
* Concept discovery
* Relationship visualization
* Interactive exploration

The system transforms documents into embeddings, stores them in **Endee**, and constructs a dynamic knowledge graph based on vector similarity.

---

# 🎯 Problem Statement

Students, researchers, and professionals consume large amounts of textual knowledge but lack tools to:

* Understand conceptual relationships
* Explore semantic connections
* Visualize knowledge structures
* Navigate information intuitively

Search engines retrieve fragments — they do not reveal structure.

---

# 💡 Solution

Nexus creates a **vector-native intelligence layer** over documents.

### Workflow

1️⃣ Upload document
2️⃣ Extract text
3️⃣ Chunk content
4️⃣ Generate embeddings
5️⃣ Store vectors in Endee
6️⃣ Discover relationships via similarity search
7️⃣ Build knowledge graph
8️⃣ Render interactive visualization

Result: A living semantic network users can explore.

---

# ⭐ Core Features

## Automatic Knowledge Graph Generation

Documents are automatically converted into nodes and edges.

* No manual labeling
* No schema definition
* No tagging required

Relationships emerge from embedding similarity.

---

## Semantic Query Exploration

Users ask natural language questions:

* "Show concepts related to attention mechanisms"
* "Find sections about cloud architecture"

System:

* Embeds query
* Searches Endee
* Highlights relevant nodes
* Displays contextual information

---

## Interactive Node Intelligence Panel

Clicking a node reveals:

* Concept text
* Source document reference
* Similar related chunks
* Connection strength metrics

Visualization becomes actionable.

---

## Graph Evolution

Adding documents:

* Expands network
* Strengthens links
* Creates clusters
* Refines semantic landscape

System behaves like a growing intelligence map.

---

# 🏗️ System Architecture

## High-Level Architecture

```
┌─────────────────┐
│   Next.js       │  ← Frontend (React Flow visualization)
│   Frontend      │
└────────┬────────┘
         │ HTTP/REST
┌────────▼────────┐
│   FastAPI       │  ← Backend (Document processing, embeddings)
│   Backend       │
└────────┬────────┘
         │ HTTP/REST
┌────────▼────────┘
│   Endee         │  ← Vector Database (C++ high-performance)
│   Vector DB     │     - Similarity search
└─────────────────┘     - Embedding storage
```

---

## Detailed Pipeline

```
Document Upload
      ↓
   Text Extraction (PyPDF2, python-docx)
      ↓
   Text Chunking (500 chars, 50 char overlap)
      ↓
   Embedding Generation (sentence-transformers)
      ↓
   Vector Storage in Endee (HNSW indexing)
      ↓
   Similarity Search for Relationships
      ↓
   Knowledge Graph Construction
      ↓
   React Flow Visualization
      ↓
   Interactive Exploration
```

---

# 🧰 Technology Stack

## Frontend

* **Next.js 14** - React framework with App Router
* **TypeScript** - Type-safe JavaScript
* **React Flow** - Graph visualization library
* **Zustand** - State management
* **Tailwind CSS** - Utility-first CSS
* **Lucide React** - Icons

Purpose: Graph visualization, upload interface, query interaction, node exploration

---

## Backend

* **FastAPI** - Modern async Python web framework
* **Python 3.10+** - Core language
* **Sentence-Transformers** - Embedding generation (all-MiniLM-L6-v2, 384 dims)
* **PyPDF2** - PDF parsing
* **python-docx** - DOCX parsing
* **httpx** - Async HTTP client for Endee

Responsibilities: Text extraction, chunking, embedding generation, Endee communication, graph construction

---

## Vector Database

### Endee

* High-performance C++ vector database
* HNSW algorithm for approximate nearest neighbor search
* Sub-10ms query latency
* Cosine similarity metric
* Batch vector insertion

Used for:
* Embedding storage
* Semantic retrieval
* Relationship discovery

---

## Embedding Model

**Sentence Transformers** with `all-MiniLM-L6-v2` model
* 384-dimensional embeddings
* Semantic text representation
* Normalized for cosine similarity

---

# 📦 Project Structure

```
endee-network-map/
│
├── src/                    # Endee vector database core (C++)
│   ├── main.cpp
│   ├── core/
│   ├── hnsw/
│   └── server/
│
├── nexus-backend/          # FastAPI Backend (Python)
│   ├── main.py             # REST API routes
│   ├── services/
│   │   ├── endee_client.py          # Endee HTTP client
│   │   ├── embedding_service.py     # Sentence-transformers wrapper
│   │   ├── document_processor.py    # PDF/TXT/DOCX parsing
│   │   ├── graph_builder.py         # Knowledge graph logic
│   │   └── query_engine.py          # Semantic search
│   ├── requirements.txt
│   └── README.md
│
├── nexus-frontend/         # Next.js Frontend (TypeScript)
│   ├── src/
│   │   ├── app/            # Next.js pages
│   │   ├── components/     # React components
│   │   │   ├── KnowledgeGraph.tsx   # React Flow visualization
│   │   │   ├── ControlPanel.tsx     # Upload & search
│   │   │   ├── NodePanel.tsx        # Node details panel
│   │   │   └── Header.tsx
│   │   ├── store/          # Zustand state management
│   │   ├── lib/            # API client
│   │   └── types/          # TypeScript interfaces
│   ├── package.json
│   └── README.md
│
├── nexus-docs/             # Documentation
│   ├── README.md           # This file
│   ├── ARCHITECTURE.md
│   ├── DEPLOYMENT.md
│   ├── QUICK_REFERENCE.md
│   ├── NEXT_STEPS.md
│   └── PROJECT_SUMMARY.md
│
├── scripts/                # Startup scripts
│   ├── start-nexus.sh
│   ├── stop-nexus.sh
│   ├── start-nexus.ps1
│   └── stop-nexus.ps1
│
└── infra/                  # Docker configuration
    └── Dockerfile
```

---

# 🔄 Data Flow Explained

## Document Processing Pipeline

1. **File Upload** → Multipart form data to `/api/documents/upload`
2. **File Validation** → Check type (PDF, TXT, MD, DOCX)
3. **Text Extraction** → Parse file content
4. **Chunking** → Split into overlapping segments (500 chars, 50 overlap)
5. **Embedding Generation** → Each chunk → 384-dim vector
6. **Vector Storage** → Insert into Endee with metadata
7. **Relationship Discovery** → Search for similar chunks
8. **Graph Construction** → Create JSON with nodes + edges
9. **Frontend Update** → Send graph to React Flow

---

## Relationship Discovery

For each embedding:

1. Query Endee for top-5 similar vectors
2. Apply similarity threshold (default: 0.7)
3. Create edges between related concepts
4. Calculate connection strength

Result: A semantic network based on vector similarity

---

## Query Flow

User enters query:

1. Embed query text
2. Vector search in Endee
3. Retrieve top-k relevant chunks
4. Highlight relevant nodes
5. Show contextual panel

---

# 🎨 User Experience

### The "WOW" Moment

When a user uploads their first document:

1. **Instant Processing** - Document chunked in seconds
2. **Graph Forms** - Concepts appear as nodes
3. **Relationships Emerge** - Edges connect similar ideas
4. **Visual Intelligence** - Understanding becomes explorable

### UI/UX Principles

* Minimal and clean interface
* Professional styling inspired by Endee colors
* Smooth animations
* Intuitive interactions
* Real-time feedback
* Zero learning curve

---

# ⚡ Quick Start

## Prerequisites

- **Python 3.10+**
- **Node.js 18+**
- **Clang 19** (for building Endee)

## Step 1: Build Endee

```bash
# From project root
chmod +x install.sh run.sh

# Build with CPU optimization
./install.sh release native

# Start Endee server (port 3001)
./run.sh
```

## Step 2: Start Backend

```bash
cd nexus-backend

# Create virtual environment
python -m venv venv
source venv/bin/activate  # Windows: venv\Scripts\activate

# Install dependencies
pip install -r requirements.txt

# Run backend (port 8000)
python main.py
```

## Step 3: Start Frontend

```bash
cd nexus-frontend

# Install dependencies
npm install

# Start development server (port 3000)
npm run dev
```

## Step 4: Use Nexus

Open http://localhost:3000

1. Upload a document
2. Watch graph form automatically
3. Click nodes to explore
4. Search semantically

---

# 📊 API Endpoints

**Base URL:** `http://localhost:8000`

| Method | Endpoint | Purpose |
|--------|----------|---------|
| GET | `/health` | Health check |
| POST | `/api/initialize` | Initialize system |
| POST | `/api/documents/upload` | Upload document |
| GET | `/api/graph` | Get knowledge graph |
| POST | `/api/query` | Semantic search |
| GET | `/api/node/{node_id}` | Node details |
| GET | `/api/stats` | System statistics |

**Interactive docs:** http://localhost:8000/docs

---

# 🚀 Deployment

## Backend

Deploy to Render, Railway, or Heroku:

```bash
# Platform: Render / Railway
Build Command: pip install -r requirements.txt
Start Command: uvicorn main:app --host 0.0.0.0 --port $PORT
Environment: ENDEE_URL=<your-endee-instance>
```

## Frontend

Deploy to Vercel:

```bash
cd nexus-frontend
npm i -g vercel
vercel
```

Set environment: `NEXT_PUBLIC_API_URL=<your-backend-url>`

## Endee

Run as Docker container:

```bash
docker build -t endee -f infra/Dockerfile .
docker run -p 3001:3001 endee
```

---

# 📈 Performance

### Benchmarks

* Vector search: <10ms for 10K vectors
* Embedding generation: ~50ms per chunk (CPU)
* Graph rendering: 60 FPS with 100+ nodes
* Document processing: Linear with file size

### Scalability

* **Endee**: Billions of vectors
* **Backend**: Horizontal scaling ready
* **Frontend**: Static deployment, CDN-ready
* **Recommended limits**: 1,000 documents, 50K nodes, 200K edges

---

# 🧠 How It Works

### Embedding Space

Text → Embedding (384 dims) → Vector space → Similarity search

Similar embeddings = Related concepts

### Graph Construction

Nodes: Text chunks
Edges: Similarity relationships
Weight: Cosine distance

### Visualization

React Flow renders:
* Nodes with circular layout
* Edges with animation
* Interactive click handlers
* Real-time updates

---

# 🔮 Future Enhancements

**Near-term:**
- Multi-user authentication
- PostgreSQL persistence
- Export graph (JSON, GraphML)
- Advanced layouts (force-directed)

**Long-term:**
- Multi-modal support (images, audio)
- Collaborative networks
- WebSocket real-time updates
- Mobile app
- LLM integration
- Graph analytics
- Custom embeddings

---

# 🧠 Engineering Demonstrations

This project showcases:

✅ Vector database integration
✅ Embedding pipelines
✅ Semantic retrieval
✅ Graph algorithms
✅ Full-stack architecture
✅ Visualization engineering
✅ Async Python patterns
✅ TypeScript best practices
✅ Production-ready deployment
✅ Real-time data flow

---

# 🤝 Contributing

1. Fork the repository
2. Create feature branch: `git checkout -b feature/amazing-feature`
3. Commit: `git commit -m 'Add amazing feature'`
4. Push: `git push origin feature/amazing-feature`
5. Open Pull Request

### Code Style

* **Python**: PEP 8
* **TypeScript**: Prettier + ESLint
* **Commits**: Conventional Commits

---

# 📝 License

MIT License - see [LICENSE](LICENSE) file

---

# 🙏 Acknowledgments

* **Endee** - Vector database foundation
* **Sentence-Transformers** - Embeddings
* **React Flow** - Graph visualization
* **FastAPI** - Web framework
* **Next.js** - React framework

---

# 👤 Author

**Pranav Venu**
Bengaluru, India

---

# 📞 Links

* **GitHub**: [endee-network-map](https://github.com/pranavv1210/endee-network-map.git)
* **Deployed Link**:

---

<div align="center">

**Built with 🧠 AI · Powered by ⚡ Endee · Visualized with 🎨 React Flow**

*Nexus: Where knowledge becomes intelligence*

Transform documents. Discover connections. Understand networks.

</div>
