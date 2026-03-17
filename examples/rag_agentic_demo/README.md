# RAG + Agentic Demo with Endee

This example shows a minimal retrieval-augmented generation loop that uses the Endee vector database for retrieval and OpenAI for embeddings + answers. It includes a tiny agentic step: each user query is first rewritten by the LLM for better retrieval, then the rewritten query is embedded and searched in Endee.

## Files
- `app.py` – main script (ingest sample docs, interactive Q&A loop)
- `requirements.txt` – Python dependencies
- `data/sample_docs/*.txt` – tiny corpus to load by default

## Prerequisites
- Python 3.10+
- Running Endee server at `http://localhost:8080` (default from `run.sh`)
- OpenAI API key (set `OPENAI_API_KEY`)

## Setup
```bash
cd examples/rag_agentic_demo
python -m venv .venv
. .venv/Scripts/activate  # Windows
# or: source .venv/bin/activate
pip install -r requirements.txt
```

## Environment
Create `.env` in this folder:
```
OPENAI_API_KEY=sk-...
ENDEE_BASE_URL=http://localhost:8080/api/v1  # optional
ENDEE_AUTH_TOKEN=                             # optional if you enabled auth
```

## Run
```bash
python app.py --index rag_demo
```
- On first run, the script ingests the sample docs.
- Ask questions in the prompt; type `exit` to quit.
- Use `--skip-ingest` if the index already has your data.

## Using your own data
Place text files under `data/sample_docs` (or modify `SAMPLE_DIR` in `app.py`). The script will embed and upsert them on startup.

## How it works (flow)
1) **Rewrite** the user question via `rewrite_query` (agentic query optimization).
2) **Embed** rewritten query with `text-embedding-3-small`.
3) **Search** Endee (`top_k=4`).
4) **Grounded answer** with `gpt-4o-mini`, constrained to retrieved context.

You can swap models, add filters, or extend the agent step to call other tools.
