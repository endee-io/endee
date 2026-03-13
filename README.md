# 🔍 RAG Agent — Retrieval Augmented Generation with Endee

A production-style RAG pipeline that ingests documents, stores embeddings in [Endee](https://github.com/endee-io/endee) vector database, retrieves relevant context, and generates answers via LLM (Gemini / OpenAI / Claude).

## Architecture

```
User Query
    ↓
Embedding Model (all-MiniLM-L6-v2)
    ↓
Endee Vector Database Search
    ↓
Retrieve Top-K Relevant Chunks
    ↓
Context Construction
    ↓
AI Agent (Gemini / OpenAI / Claude)
    ↓
Generated Response
```

## Project Structure

```
rag_project/
├── config/
│   └── settings.py          # Central configuration (env vars)
├── ingestion/
│   ├── loader.py             # Load .txt, .pdf, .json files
│   ├── chunker.py            # Split documents into chunks
│   └── pipeline.py           # Full ingestion orchestrator
├── retrieval/
│   └── retriever.py          # Query Endee, format context
├── agent/
│   └── llm_client.py         # Multi-provider LLM client
├── prompts/
│   └── templates.py          # RAG prompt templates
├── api/
│   └── server.py             # FastAPI REST API
├── sample_data/
│   └── movies.txt            # Sample data for testing
├── main.py                   # CLI entrypoint
├── docker-compose.yml        # Endee server
├── requirements.txt
├── .env.example
└── README.md
```

## Quick Start

### 1. Prerequisites

- **Python 3.8+**
- **Docker** (for Endee vector database)
- An API key for at least one LLM provider (Gemini, OpenAI, or Claude)

### 2. Start Endee

```bash
docker-compose up -d
```

Verify it's running:
```bash
curl http://localhost:8080/api/v1/health
```

### 3. Install Dependencies

```bash
pip install -r requirements.txt
```

### 4. Configure Environment

```bash
cp .env.example .env
# Edit .env and set your API key(s)
```

At minimum, set one of:
- `GEMINI_API_KEY` (default provider)
- `OPENAI_API_KEY` (set `LLM_PROVIDER=openai`)
- `ANTHROPIC_API_KEY` (set `LLM_PROVIDER=claude`)

### 5. Ingest Sample Data

```bash
python main.py ingest --path ./sample_data/
```

### 6. Ask a Question

```bash
python main.py query --question "Recommend a sci-fi movie similar to Interstellar"
```

### 7. Start the API Server

```bash
python main.py serve
```

The API will be available at `http://localhost:8000` with interactive docs at `/docs`.

## API Endpoints

### `POST /query`

```json
// Request
{
  "question": "Recommend a sci-fi movie similar to Interstellar"
}

// Response
{
  "answer": "Based on the context, I'd recommend...",
  "sources": [
    { "filename": "movies.txt", "score": 0.8523, "chunk_index": 0 }
  ],
  "question": "Recommend a sci-fi movie similar to Interstellar"
}
```

### `POST /ingest`

```json
// Request
{ "path": "./sample_data/" }

// Response
{ "documents": 1, "chunks": 8, "status": "success" }
```

### `GET /health`

```json
{ "status": "healthy", "service": "rag-agent-api" }
```

## Configuration

All settings are configurable via environment variables (see `.env.example`):

| Variable | Default | Description |
|----------|---------|-------------|
| `ENDEE_URL` | `http://localhost:8080` | Endee server URL |
| `ENDEE_INDEX_NAME` | `rag-documents` | Vector index name |
| `EMBEDDING_MODEL` | `all-MiniLM-L6-v2` | SentenceTransformer model |
| `CHUNK_SIZE` | `500` | Characters per chunk |
| `CHUNK_OVERLAP` | `50` | Overlap between chunks |
| `TOP_K` | `5` | Results to retrieve |
| `LLM_PROVIDER` | `gemini` | `gemini` / `openai` / `claude` |
| `GEMINI_API_KEY` | — | Google Gemini API key |
| `OPENAI_API_KEY` | — | OpenAI API key |
| `ANTHROPIC_API_KEY` | — | Anthropic API key |

## Supported File Types

- `.txt` — Plain text
- `.md` — Markdown
- `.pdf` — PDF (via PyPDF2)
- `.json` — JSON (list of objects or single object with `text`/`content` field)
