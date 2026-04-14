# 🛡️ AI Research Assistant: Powered by Endee

## 🚀 Overview
This project is a high-performance **Semantic Research Engine** built for the Endee SDE/AI Internship.

It uses the **Endee C++ vector database** to index and retrieve 1,000+ technical articles from Wikipedia, and it demonstrates a production-ready **RAG (Retrieval-Augmented Generation)** pipeline.

## 🏗️ System Design
The architecture is designed for scalability and low-latency retrieval.

- **Data Layer**
  - Wikipedia technology dataset with 1,000+ technical articles.
- **Embedding Layer**
  - `all-MiniLM-L6-v2` from Sentence Transformers to map text into 384-dimensional dense vectors.
- **Vector Store**
  - Endee C++ engine running in a containerized environment for fast similarity search.
- **Logic Layer**
  - Python-based retrieval agent that generates embeddings, queries the vector store, and returns metadata-rich search results.

## 🛠️ Setup & Execution

### Prerequisites
- Docker Desktop
- Python 3.8+ (or compatible environment)
- `pip`

### 1. Build and run the Endee engine
From the repository root:
```bash
docker build -t endee-db -f infra/Dockerfile .
docker run -d -p 8080:8080 --name endee-container endee-db
```

### 2. Install Python dependencies
```bash
pip install -r requirements.txt
```

### 3. Run the AI agent
```bash
python app.py
```

## 📂 Project Structure
- `infra/Dockerfile` — Docker build environment for the Endee engine.
- `app.py` — Main application logic for dataset ingestion and semantic search.
- `requirements.txt` — Python dependency manifest.
- `README.md` — Project documentation.

## 🌟 Highlights
- **Infrastructure optimization** — Docker and build setup ready for cross-platform development.
- **Large-scale ingestion** — Indexed 1,000+ technical articles into the vector engine.
- **Semantic retrieval** — Supports intent-aware search beyond simple keyword matching.

## Notes
- Confirm that Docker is running before executing the container steps.
- If the project includes additional scripts or notebooks, add them to this README under the appropriate section.
