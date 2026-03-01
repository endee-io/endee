# 📚 AI Research Assistant  
### Retrieval-Augmented Generation (RAG) using Endee

---

## 📌 Overview

This module implements a complete Retrieval-Augmented Generation (RAG) system built on top of the Endee (nD) vector database.

It demonstrates:

- Dynamic document ingestion
- 384-dimensional semantic embeddings (MiniLM)
- Vector indexing and cosine similarity search using Endee
- MessagePack binary response decoding
- Context-grounded answer generation using Groq Llama 3.3 70B
- Interactive UI using Gradio

---

## 🧠 System Architecture
Document Ingestion
↓
MiniLM Embedding (384-dim)
↓
Endee Vector Index
↓
User Query
↓
Query Embedding
↓
Top-K Vector Search
↓
Retrieve Matching Documents
↓
Context Injection
↓
Groq Llama 3.3 70B
↓
Grounded Answer

## 📂 Project Structure
research_assistant/
│
├── core/
│ ├── embeddings.py # Embedding generation
│ ├── document_manager.py # Local document storage (JSON)
│ ├── ingest.py # Document ingestion pipeline
│ └── rag_pipeline.py # Retrieval + generation logic
│
├── data/
│ └── documents.json # Local document store
│
├── app.py # Gradio UI
├── ingest_test.py # Sample ingestion script
└── requirements.txt


---

## ⚙️ How the System Works

### 1️⃣ Document Ingestion

When a document is added:

- Stored locally in `documents.json`
- Converted to a 384-dimensional embedding using:

  `sentence-transformers/all-MiniLM-L6-v2`

- Inserted into Endee using the `/vector/insert` API

---

### 2️⃣ Semantic Search

When a user submits a query:

- Query converted into embedding
- Endee performs cosine similarity search
- Top-K vector IDs are returned (MessagePack format)
- IDs mapped to locally stored documents

---

### 3️⃣ Grounded Answer Generation

- Retrieved documents combined into context
- Context injected into Groq Llama 3.3 70B
- Model instructed to answer using only provided context

This ensures retrieval-grounded responses.

---

## 🚀 Setup Instructions

### 1️⃣ Start Endee Start Endee (Using Official Docker Image) (Docker)


docker run -d \
  -p 8080:8080 \
  -v endee-data:/data \
  --name endee-server \
  endeeio/endee-server:latest

Or if already running:
docker start endee-server

---

### 2️⃣ Install Dependencies

Inside research_assistant/:
pip install -r requirements.txt

---

### 3️⃣ Set Groq API Key

Windows (PowerShell):
$env:GROQ_API_KEY="your_api_key_here"

---

### 4️⃣ Ingest Sample Document
python ingest_test.py

---

### 5️⃣ Run the Application
python app.py
Open:
http://127.0.0.1:7860

---
