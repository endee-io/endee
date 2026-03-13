# 📄 PDF RAG Application using Endee Vector Database

A production-style **Retrieval Augmented Generation (RAG)** application that lets you ask questions about any PDF document. Built using [Endee](https://github.com/endee-io/endee) as the vector database, LangChain for orchestration, and Groq LLM for fast inference.

---

## 🧠 Problem Statement

Large PDF documents contain valuable information that is hard to search through manually. Traditional keyword search misses context and meaning. This project solves that by:
- Converting PDF content into semantic vector embeddings
- Storing them in the Endee vector database
- Retrieving only the most relevant chunks when a question is asked
- Feeding those chunks to an LLM to generate accurate, context-aware answers

---

## 🏗️ System Design

```
┌─────────────────────────────────────────────────────────────┐
│                        INDEXING PHASE                        │
│                       (runs once per PDF)                    │
│                                                              │
│  PDF File → PyMuPDF → Raw Text → Chunker (500 chars)        │
│       → SentenceTransformer → 384-dim Vectors               │
│       → Endee Vector DB (cosine similarity, INT8)           │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                        QUERY PHASE                           │
│                    (runs on every question)                   │
│                                                              │
│  User Question → SentenceTransformer → Query Vector         │
│       → Endee cosine search → Top 4 chunks                  │
│       → LangChain Prompt Template → Groq LLM                │
│       → Final Answer                                         │
└─────────────────────────────────────────────────────────────┘
```

### Tech Stack

| Component | Technology |
|---|---|
| Vector Database | Endee (self-hosted via Docker) |
| Embeddings | `sentence-transformers/all-MiniLM-L6-v2` (384-dim) |
| PDF Parsing | PyMuPDF (`fitz`) |
| LLM | Groq — `llama3-8b-8192` |
| Orchestration | LangChain (LCEL chain) |
| Language | Python 3.11 |

---

## 🔍 How Endee is Used

Endee acts as the core vector store in this project:

1. **Index Creation** — A cosine similarity index with 384 dimensions and INT8 precision is created in Endee
2. **Upsert** — Each PDF chunk is embedded and stored in Endee with metadata (`text`, `chunk_id`)
3. **Query** — On each question, the query vector is searched against Endee using `top_k=4` to retrieve the most semantically similar chunks
4. **Context Assembly** — Retrieved chunks are assembled into a context string and passed to the LLM

Endee runs locally as a Docker container on `localhost:8080`, making the entire pipeline fully self-hosted with no cloud vector DB costs.

---

## ⚙️ Setup & Execution

### Prerequisites
- Python 3.10+
- Docker Desktop (for running Endee server)
- A Groq API key → https://console.groq.com

### 1. Clone the Repository
```bash
git clone https://github.com/YOUR_USERNAME/endee
cd endee
```

### 2. Install Dependencies
```bash
pip install -r requirements.txt
```

### 3. Start Endee Server
```bash
docker compose up -d
```
Endee will be running at `http://localhost:8080`

### 4. Configure the Notebook

Open `rag.ipynb` and set these two values in the config cell:

```python
GROQ_API_KEY = "gsk_your_actual_key_here"   # from console.groq.com
PDF_PATH     = "your_document.pdf"           # path to your PDF
```

### 5. Run the Notebook

Run all cells top to bottom. The notebook will:
- Extract and chunk the PDF
- Embed chunks and store in Endee
- Set up the LangChain + Groq pipeline
- Let you ask questions about the document

### 6. Ask Questions

```python
my_question = "What is the main topic of this document?"
answer = rag_query(my_question)
print(answer)
```

---

## 📁 Project Structure

```
endee/
├── rag.ipynb            ← Main notebook (full RAG pipeline)
├── requirements.txt     ← Python dependencies
├── docker-compose.yml   ← Endee server setup
├── .gitignore           ← Excludes API keys and cache
└── README.md            ← This file
```

---

## 🔄 Docker Commands

```bash
docker compose up -d        # Start Endee
docker compose down         # Stop Endee
docker logs -f endee-server # View logs
```

---

## 📌 Notes

- The Groq API key is set directly in the notebook config cell — never commit a real key to GitHub
- Endee index persists across Docker restarts via a named volume
- To re-index a new PDF, restart the kernel and run all cells again
- For large PDFs, consider using `mixtral-8x7b-32768` (32k context window)
