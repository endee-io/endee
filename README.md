# 🎓 Campus Placement Copilot

> AI-powered RAG system to help engineering students prepare for campus placements using semantic search and LLM-generated answers.

![RAG Pipeline](https://img.shields.io/badge/RAG-Pipeline-blue) ![Endee](https://img.shields.io/badge/Vector_DB-Endee-green) ![Groq](https://img.shields.io/badge/LLM-Groq_LLaMA3-orange) ![FastAPI](https://img.shields.io/badge/Backend-FastAPI-teal)

---

## 🚀 What is this?

Campus Placement Copilot is a **Retrieval-Augmented Generation (RAG)** application that helps students get accurate, context-aware answers about campus placement preparation — covering companies like TCS, Infosys, Wipro, Cognizant, and more.

Students can ask questions like:
- *"How to crack TCS Ninja interview?"*
- *"What is the Infosys System Engineer selection process?"*
- *"Tips for Wipro NLTH exam?"*

The system retrieves the most relevant information from a vector database and generates a precise answer using an LLM.

---

## 🏗️ System Design
Student Query
│
▼
Frontend (HTML/CSS/JS)
│  POST /ask
▼
FastAPI Backend (port 8000)
│
▼
SentenceTransformer
(all-MiniLM-L6-v2)
generates query embedding
│
▼
Endee Vector DB (HNSW Index)
searches top-K similar chunks
│
▼
Retrieved Context Chunks
│
▼
Groq LLaMA 3.1 (LLM)
generates final answer
│
▼
Answer + Sources → Student

---

## 🛠️ Tech Stack

| Component | Technology |
|---|---|
| Vector Database | **Endee** (Docker, HNSW index) |
| Embeddings | SentenceTransformers `all-MiniLM-L6-v2` |
| LLM | Groq `llama-3.1-8b-instant` |
| Backend | FastAPI + Python |
| Frontend | Vanilla HTML, CSS, JavaScript |
| Deployment | Docker + Uvicorn |

---

## 🔍 How Endee is Used

[Endee](https://github.com/endee-io/endee) is a high-performance open-source vector database built for speed and efficiency.

In this project, Endee is used to:
1. **Store** document embeddings as 384-dimensional vectors using HNSW index
2. **Search** semantically similar chunks using cosine similarity
3. **Retrieve** top-K relevant context for RAG pipeline

```python
# Create index
client.create_index(
    name="placement_copilot",
    dimension=384,
    space_type="cosine",
    precision=Precision.INT8
)

# Upsert vectors
index.upsert([{
    "id": "doc_0_chunk_0",
    "vector": embedding,
    "meta": {"text": chunk, "company": "TCS"}
}])

# Semantic search
results = index.query(
    vector=query_embedding,
    top_k=5,
    ef=128
)
```

---

## ⚙️ Setup Instructions

### Prerequisites
- Python 3.8+
- Docker Desktop
- Groq API key (free at [console.groq.com](https://console.groq.com))

### Step 1 — Clone the repo
```bash
git clone https://github.com/YOUR_USERNAME/YOUR_REPO.git
cd campus-placement-copilot
```

### Step 2 — Start Endee Vector DB
```bash
docker run -d \
  -p 8080:8080 \
  -v ./endee-data:/data \
  --name endee-server \
  endeeio/endee-server:latest
```

### Step 3 — Install dependencies
```bash
pip install -r requirements.txt
```

### Step 4 — Configure environment
Create `.env` file:
```env
GROQ_API_KEY=your_groq_api_key_here
GROQ_MODEL=llama-3.1-8b-instant
MODEL_NAME=all-MiniLM-L6-v2
```

### Step 5 — Ingest data into Endee
```bash
python -c "import asyncio; from backend.ingest import ingest_data; asyncio.run(ingest_data())"
```

### Step 6 — Start backend
```bash
python app.py
```

### Step 7 — Start frontend
```bash
cd frontend
python -m http.server 3000
```

### Step 8 — Open browser
http://localhost:3000

---

## 📁 Project Structure
campus-placement-copilot/
├── app.py                 # FastAPI entry point
├── backend/
│   ├── ingest.py          # Data ingestion + Endee upsert
│   └── search.py          # Semantic search + Groq LLM
├── frontend/
│   ├── index.html         # UI
│   ├── script.js          # API calls
│   └── style.css          # Styling
├── data/
│   └── placement_data.json # Placement knowledge base
├── requirements.txt
└── README.md

---

## 💡 Features

- ✅ Semantic search powered by Endee HNSW vector index
- ✅ RAG pipeline — retrieve then generate
- ✅ Groq LLaMA 3.1 for fast, accurate answers
- ✅ Source citations with every answer
- ✅ Coverage: TCS, Infosys, Wipro, Cognizant, and more

---

## 🔗 References

- [Endee Vector DB](https://github.com/endee-io/endee)
- [Endee Documentation](https://docs.endee.io)
- [Groq API](https://console.groq.com)
- [SentenceTransformers](https://www.sbert.net)