# Privacy-first HR Assistant (Endee + RAG)

Flutter client + FastAPI backend: PDFs are chunked locally, **chunk text stays in SQLite** on the API host, while **embeddings are indexed in [Endee](https://docs.endee.io/)** via the Python SDK (Queryable / client-side encrypted vector plane). RAG defaults to **Ollama**; set `USE_CLOUD_LLM=1` for OpenAI.

## Prerequisites

- Docker (for Endee; optional Ollama profile)
- Python 3.12+
- Flutter SDK (for the mobile/desktop client)

## 1. Start Endee

```bash
docker compose up -d endee
```

Optional local LLM:

```bash
docker compose --profile llm up -d ollama
docker exec -it ollama-server ollama pull llama3.2
```

## 2. Backend

```bash
cd backend
python -m venv .venv
.venv\Scripts\activate   # Windows
pip install -r requirements.txt
copy ..\.env.example .env   # adjust values
uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

Verify Endee from the host (with Endee running):

```bash
set ENDEE_BASE_URL=http://127.0.0.1:8080/api/v1
python scripts\verify_endee.py
```

## 3. Flutter

- **Physical device / desktop:** point API URL at your machine’s LAN IP, e.g. `http://192.168.1.10:8000`.
- **Android emulator:** use `http://10.0.2.2:8000`.
- **iOS simulator:** `http://127.0.0.1:8000` works.

```bash
cd frontend/hr_assistant
flutter run --dart-define=API_BASE=http://127.0.0.1:8000
```

Login with JWT (default) or turn off JWT in the app and paste the same value as `API_BEARER_TOKEN` from `.env`.

## API highlights

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/documents/upload` | PDF → chunks → SQLite + Endee vectors |
| POST | `/search` | Semantic search + optional `dept_code` / `doc_type` filters |
| POST | `/chat` | RAG answer + sources |
| POST | `/recommendations/similar` | Similar chunks (same index, optional filters) |
| POST | `/agent/run` | Search → list sources → summarize |
| POST | `/tools/hybrid_search` | Hybrid query if index supports sparse; else dense fallback |

## Docker “full” stack

```bash
docker compose --profile full up -d --build
```

Add `--profile llm` if you also want the Ollama container; point `OLLAMA_BASE_URL` at it from the backend when not using compose profiles.

## Threat model (short)

- **Endee:** holds vectors and opaque metadata/filters — not the full resume text (that is in the app SQLite DB).
- **Residual:** text is in memory during PDF parse/embed; the LLM sees retrieved snippets; cloud LLM mode sends snippets to the vendor.
