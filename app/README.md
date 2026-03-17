# Agentic RAG Starter (FastAPI + Streamlit + Endee + Gemini + Tavily)

This app lets you chat with three routing paths:
- **Agent** (auto-route): LLM decides whether to use RAG (Endee), Web (Tavily), or Direct (LLM only).
- **RAG**: embed → Endee search → rerank → Gemini answer with sources.
- **Web**: Tavily search → Gemini answer with web citations.
- **Direct**: Gemini only (no retrieval).

## Tech Stack
- Backend: FastAPI (`app/backend/main.py`)
- Frontend: Streamlit (`app/frontend/streamlit_app.py`)
- Vector DB: Endee server at `http://localhost:8080` (cosine, INT8, dim 384)
- Embeddings: `sentence-transformers/all-MiniLM-L6-v2`
- Reranker: `cross-encoder/ms-marco-MiniLM-L-6-v2`
- LLM: Gemini (default `gemini-2.5-flash` via Google AI key)
- Web search: Tavily API

## Prerequisites
- Python 3.10+
- Endee server running locally on 8080 (from `run.sh` or docker-compose)
- API keys:
  - `GEMINI_API_KEY` (required)
  - `TAVILY_API_KEY` (for Web route)

## Setup
```powershell
cd C:\VH811\projects\endee\app
python -m venv .venv
. .venv\Scripts\activate
pip install -r requirements.txt
```

### Environment
Edit `app/.env` (already created) and set:
```
GEMINI_API_KEY=your_gemini_key
GEMINI_MODEL=gemini-2.5-flash
ENDEE_BASE_URL=http://localhost:8080/api/v1
ENDEE_AUTH_TOKEN=           # blank if Endee auth disabled
ENDEE_INDEX_NAME=rag_app
CHUNK_SIZE=800
CHUNK_OVERLAP=120
TOP_K=6
TAVILY_API_KEY=your_tavily_key
BACKEND_URL=http://localhost:8000
```

## Run
Backend:
```powershell
cd C:\VH811\projects\endee\app
. .venv\Scripts\activate
python -m uvicorn backend.main:app --host 0.0.0.0 --port 8000
```
Frontend:
```powershell
cd C:\VH811\projects\endee\app
. .venv\Scripts\activate
streamlit run frontend\streamlit_app.py
```
Open Streamlit at http://localhost:8501.

## How it Works (Pipeline)
1) **Uploads**: pdf/docx/txt/md → text extract (pypdf/docx2txt) → chunk (800/120) → embed (MiniLM) → upsert to Endee with metadata.
2) **Routing** (Agent): quick Gemini prompt decides `rag|web|direct` unless user forces mode via UI radio.
3) **RAG path**: embed query → Endee top_k=6 → rerank top 4 with cross-encoder → build context → Gemini answers; returns sources (chunk previews).
4) **Web path**: Tavily search max 5 results → context → Gemini answers; returns web URLs/titles.
5) **Direct path**: Gemini answers without retrieval.
6) **History**: last 5 turns sent to LLM for coherence (not for facts).

## Routing Modes in UI
- Agent (auto): LLM router picks best path.
- RAG: force vector search.
- Web: force Tavily search.
- Direct: force plain LLM.

## Tuning & Performance
- Lower `TOP_K` or rerank cutoff to reduce latency (currently rerank keeps top 4).
- Smaller reranker model (e.g., `cross-encoder/ms-marco-MiniLM-L-2-v2`) for speed.
- Use Direct mode for general chit-chat to avoid retrieval.
- Warm cache by one request after restart to load HF models.

## Troubleshooting
- `API key not valid / 403 / 429`: check/replace `GEMINI_API_KEY`, quota or billing; switch model if needed.
- `TAVILY_API_KEY not configured`: set the key in `.env` for Web mode.
- Upload errors: ensure file type is pdf/docx/txt/md and Endee server is reachable at `ENDEE_BASE_URL`.

## File Map
- `app/backend/main.py` — API routes, router, RAG/Web/Direct flows
- `app/frontend/streamlit_app.py` — UI and routing selector
- `app/requirements.txt` — dependencies
- `app/.env` — keys and settings

## Notes
- Index auto-creates with dimension 384 to match MiniLM embeddings.
- Sources shown only for RAG/Web modes; direct replies have no citations.
- Last 5 message pairs are passed for conversational continuity.

## Architecture Diagram (Text)
```
User (Streamlit UI)
    |
    v
Routing Selector (Agent/RAG/Web/Direct)
    |
    +-- Agent -> Router Prompt (Gemini) -> choose path
    |        |
    |        +-- RAG Path:
    |        |       embed query (MiniLM)
    |        |       -> Endee search (cosine, INT8, top_k)
    |        |       -> rerank (cross-encoder)
    |        |       -> context -> Gemini answer + chunk sources
    |        |
    |        +-- Web Path:
    |        |       Tavily search (max 5)
    |        |       -> context -> Gemini answer + web sources
    |        |
    |        +-- Direct Path:
    |                Gemini answer (no retrieval)
    |
Uploads
    |
    v
File ingest (pdf/docx/txt/md)
    -> extract text
    -> chunk (800/120)
    -> embed (MiniLM)
    -> Endee upsert (metadata)
```
