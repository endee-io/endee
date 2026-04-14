# 📄 DocuSearch — AI-Powered PDF Q&A with RAG

Upload any PDF and ask questions about it. Get AI-generated answers grounded in your document.

## How It Works

1. Upload a PDF → app splits it into chunks
2. Each chunk is converted to a vector (number list) using Sentence Transformers
3. Vectors are stored in **Endee Vector Database**
4. When you ask a question, Endee finds the most relevant chunks via cosine similarity
5. Those chunks + your question go to **Ollama (llama3.2)** → you get a clean answer

## System Architecture
User Query 
↓ 
[Streamlit UI] 
↓
[Sentence Transformers - all-MiniLM-L6-v2] ← generates embeddings ↓ 
[Endee Vector DB] ← stores & retrieves top-k relevant chunks 
↓ 
[Ollama llama3.2] ← generates final answer 
↓ 
Answer displayed to User

## How Endee is Used

- Creates a `documents` index with 384 dimensions and cosine similarity
- Upserts document chunk embeddings with metadata
- Performs vector similarity search to retrieve top-5 relevant chunks

## Setup Instructions

### Prerequisites
- Python 3.10+
- Docker
- Ollama

### 1. Start Endee Vector Database
```bash
docker compose up -d
```

### 2. Pull the Ollama model
```bash
ollama pull llama3.2
```

### 3. Install dependencies
```bash
pip install -r requirements.txt
```

### 4. Run the app
```bash
streamlit run app.py
```

Open http://localhost:8501, upload a PDF, and start asking questions!

## Tech Stack

| Tool | Purpose |
|------|---------|
| Endee | Vector database for storing & searching embeddings |
| Sentence Transformers | Converting text to vector embeddings |
| Ollama (llama3.2) | Local LLM for generating answers |
| Streamlit | Web UI |
| PyPDF2 | PDF text extraction |
