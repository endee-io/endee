# Agentic RAG App (FastAPI + Streamlit + Endee)

## What it does
- Upload PDF/DOCX/TXT → chunk with LangChain → embed via `sentence-transformers/all-MiniLM-L6-v2` → store in Endee.
- Chat endpoint: embed query, search Endee (top_k), rerank with `cross-encoder/ms-marco-MiniLM-L-6-v2`, build context, answer with Gemini model (default `gemini-2.5-flash`).
- Toggle RAG on/off in the UI; responses include source metadata.

## Layout
- `app/backend/main.py` – FastAPI service (`/upload`, `/chat`, `/health`).
- `app/frontend/streamlit_app.py` – Streamlit UI client.
- `app/requirements.txt` – all deps for both frontend + backend.
- `app/.env` – fill with your keys; defaults point to local Endee + backend.

## Setup
```bash
cd app
python -m venv .venv
. .venv/Scripts/activate  # or source .venv/bin/activate
pip install -r requirements.txt
```
Edit `.env` with your `GEMINI_API_KEY` (and `ENDEE_AUTH_TOKEN` if your server requires it).

## Run backend
```bash
uvicorn backend.main:app --host 0.0.0.0 --port 8000 --reload
```

## Run frontend
```bash
streamlit run frontend/streamlit_app.py
```

## Notes
- Endee index name is `rag_app` by default; change via `ENDEE_INDEX_NAME`.
- Embedding dim is fixed at 384 to match `all-MiniLM-L6-v2` and the Endee index is created automatically if missing.
- Reranker uses a cross-encoder; if you want faster responses, you can disable reranking by returning `results` directly in `retrieve`.
