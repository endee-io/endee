"""
api/server.py
──────────────
FastAPI web server for WSL Sensei.

Endpoints
─────────
  POST /ask                – Ask a question (full RAG pipeline)
  POST /search             – Semantic search only (no LLM generation)
  POST /index              – Trigger re-indexing of the environment
  GET  /status             – Health check + Endee index stats
  GET  /docs               – OpenAPI documentation (FastAPI built-in)

Run with:
  uvicorn api.server:app --reload --port 8000
"""

from __future__ import annotations

import time
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, HTTPException, BackgroundTasks
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

from config import (
    ENDEE_INDEX_NAME,
    LLM_BACKEND,
    RAG_TOP_K,
    EMBEDDING_MODEL,
    EMBEDDING_DIMENSION,
)
from indexing.embed_store import get_index_stats
from retrieval.semantic_search import semantic_search
from rag.rag_pipeline import ask, RAGResponse


# ── Startup / shutdown ────────────────────────────────────────────────────────

@asynccontextmanager
async def lifespan(app: FastAPI):
    """Pre-load the embedding model on startup so the first request is fast."""
    print("[server] 🚀 WSL Sensei starting …")
    from indexing.embed_store import _get_embedding_model
    _get_embedding_model()
    print("[server] ✓  Ready")
    yield
    print("[server] 👋 WSL Sensei shutting down.")


# ── App ───────────────────────────────────────────────────────────────────────

app = FastAPI(
    title="WSL Sensei",
    description=(
        "AI assistant for Windows + WSL developer environments. "
        "Powered by Endee vector database and sentence-transformers."
    ),
    version="1.0.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ── Request / Response models ─────────────────────────────────────────────────

class AskRequest(BaseModel):
    question: str  = Field(..., min_length=3, max_length=1000,
                           example="How do I start my project?")
    top_k:    int  = Field(default=RAG_TOP_K, ge=1, le=20)
    backend:  str  = Field(default=LLM_BACKEND,
                           description="ollama | openai | anthropic | mock")
    min_score: float = Field(default=0.1, ge=0.0, le=1.0)


class SourceItem(BaseModel):
    id:       str
    score:    float
    source:   str
    doc_type: str
    text:     str


class AskResponse(BaseModel):
    question:   str
    answer:     str
    sources:    list[SourceItem]
    backend:    str
    latency_ms: float


class SearchRequest(BaseModel):
    query:       str   = Field(..., min_length=2, max_length=500,
                               example="nginx config location")
    top_k:       int   = Field(default=RAG_TOP_K, ge=1, le=20)
    filter_type: str | None = Field(default=None,
                                    description="bash_history | config | script | log")
    min_score:   float = Field(default=0.0, ge=0.0, le=1.0)


class SearchResponse(BaseModel):
    query:      str
    results:    list[SourceItem]
    count:      int
    latency_ms: float


class IndexRequest(BaseModel):
    sources: list[str] = Field(
        default=["bash_history", "powershell_history", "config_files"],
        description="Which collectors to run.",
    )
    force_rebuild: bool = Field(
        default=False,
        description="Delete the existing index before re-indexing.",
    )


class IndexResponse(BaseModel):
    message:        str
    chunks_indexed: int
    duration_s:     float


class StatusResponse(BaseModel):
    status:       str
    index_name:   str
    index_stats:  dict[str, Any]
    embedding:    dict[str, Any]
    llm_backend:  str


# ── Background indexing task ──────────────────────────────────────────────────

_indexing_in_progress = False


def _run_indexing(sources: list[str], force_rebuild: bool) -> tuple[int, float]:
    """
    Run collectors, chunk, embed, and store.
    Returns (chunks_indexed, duration_seconds).
    """
    global _indexing_in_progress
    _indexing_in_progress = True

    try:
        from collectors.bash_history_collector    import collect_bash_history
        from collectors.powershell_history_collector import collect_powershell_history
        from collectors.config_file_scanner       import collect_config_files
        from indexing.chunker                     import chunk_documents
        from indexing.embed_store                 import store_chunks, delete_index

        t0 = time.time()

        if force_rebuild:
            delete_index()

        docs = []
        if "bash_history"       in sources: docs += collect_bash_history()
        if "powershell_history" in sources: docs += collect_powershell_history()
        if "config_files"       in sources: docs += collect_config_files()

        chunks = chunk_documents(docs)
        count  = store_chunks(chunks)
        return count, time.time() - t0
    finally:
        _indexing_in_progress = False


# ── Endpoints ─────────────────────────────────────────────────────────────────

@app.get("/status", response_model=StatusResponse, summary="Health check")
async def status():
    """Return server health and Endee index statistics."""
    stats = get_index_stats()
    return StatusResponse(
        status="ok",
        index_name=ENDEE_INDEX_NAME,
        index_stats=stats,
        embedding={
            "model":     EMBEDDING_MODEL,
            "dimension": EMBEDDING_DIMENSION,
        },
        llm_backend=LLM_BACKEND,
    )


@app.post("/ask", response_model=AskResponse, summary="Ask a question (RAG)")
async def ask_question(req: AskRequest):
    """
    Full RAG pipeline:
    1. Embed the question
    2. Semantic search in Endee
    3. Build context from top-k chunks
    4. Generate answer via the configured LLM backend
    """
    try:
        result: RAGResponse = ask(
            question=req.question,
            top_k=req.top_k,
            backend=req.backend,
            min_score=req.min_score,
        )
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    return AskResponse(
        question=result.question,
        answer=result.answer,
        sources=[
            SourceItem(
                id=s.id,
                score=round(s.score, 4),
                source=s.source,
                doc_type=s.doc_type,
                text=s.text[:300],  # truncate for API response
            )
            for s in result.sources
        ],
        backend=result.backend,
        latency_ms=round(result.latency_ms, 1),
    )


@app.post("/search", response_model=SearchResponse, summary="Semantic search only")
async def search(req: SearchRequest):
    """
    Run semantic search over the Endee index without LLM generation.
    Useful for exploring what's in the index.
    """
    t0 = time.time()
    try:
        results = semantic_search(
            query=req.query,
            top_k=req.top_k,
            filter_type=req.filter_type,
            min_score=req.min_score,
        )
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    return SearchResponse(
        query=req.query,
        results=[
            SourceItem(
                id=r.id,
                score=round(r.score, 4),
                source=r.source,
                doc_type=r.doc_type,
                text=r.text[:400],
            )
            for r in results
        ],
        count=len(results),
        latency_ms=round((time.time() - t0) * 1000, 1),
    )


@app.post("/index", response_model=IndexResponse, summary="Re-index the environment")
async def trigger_indexing(req: IndexRequest, background_tasks: BackgroundTasks):
    """
    Trigger a (re-)indexing of the developer environment.
    Runs synchronously for simplicity; switch to background_tasks for large envs.
    """
    if _indexing_in_progress:
        raise HTTPException(status_code=409, detail="Indexing already in progress.")

    try:
        count, duration = _run_indexing(req.sources, req.force_rebuild)
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    return IndexResponse(
        message=f"Indexed {count} chunks successfully.",
        chunks_indexed=count,
        duration_s=round(duration, 2),
    )


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("api.server:app", host="0.0.0.0", port=8000, reload=True)
