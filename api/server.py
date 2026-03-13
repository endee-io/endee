"""
FastAPI server — exposes the RAG pipeline as a REST API.

Endpoints:
  POST /query   — ask a question, get an AI-generated answer with sources
  POST /ingest  — ingest documents from a file path
  GET  /health  — health check
"""

import logging
from typing import List, Optional

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

from retrieval.retriever import retrieve, format_context
from agent.llm_client import answer_question
from ingestion.pipeline import run_ingestion

logger = logging.getLogger(__name__)

# ── FastAPI app ────────────────────────────────────────────────────────

app = FastAPI(
    title="RAG Agent API",
    description="Retrieval Augmented Generation pipeline powered by Endee vector database",
    version="1.0.0",
)

# Allow cross-origin requests for local dev
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# ── Request / Response models ─────────────────────────────────────────

class QueryRequest(BaseModel):
    question: str = Field(..., min_length=1, description="The user's question")
    top_k: Optional[int] = Field(None, ge=1, le=20, description="Number of results to retrieve")


class SourceInfo(BaseModel):
    filename: str
    score: float
    chunk_index: int


class QueryResponse(BaseModel):
    answer: str
    sources: List[SourceInfo]
    question: str


class IngestRequest(BaseModel):
    path: str = Field(..., min_length=1, description="File or directory path to ingest")


class IngestResponse(BaseModel):
    documents: int
    chunks: int
    status: str


class HealthResponse(BaseModel):
    status: str
    service: str


# ── Endpoints ─────────────────────────────────────────────────────────

@app.get("/health", response_model=HealthResponse)
async def health_check():
    """Health check endpoint."""
    return HealthResponse(status="healthy", service="rag-agent-api")


@app.post("/query", response_model=QueryResponse)
async def query_endpoint(request: QueryRequest):
    """
    Accept a user question, retrieve relevant context from Endee,
    send context + question to the LLM, and return the answer.
    """
    try:
        # Step 1 — Retrieve relevant chunks
        results = retrieve(request.question, top_k=request.top_k)

        # Step 2 — Format context
        context = format_context(results)

        # Step 3 — Generate answer via LLM
        answer = answer_question(request.question, context)

        # Step 4 — Build source list
        sources = [
            SourceInfo(
                filename=r["metadata"].get("filename", "unknown"),
                score=round(r.get("score", 0.0), 4),
                chunk_index=r["metadata"].get("chunk_index", 0),
            )
            for r in results
        ]

        return QueryResponse(
            answer=answer,
            sources=sources,
            question=request.question,
        )

    except ConnectionError as e:
        logger.error(f"Connection error: {e}")
        raise HTTPException(status_code=503, detail=str(e))
    except ValueError as e:
        logger.error(f"Configuration error: {e}")
        raise HTTPException(status_code=500, detail=str(e))
    except Exception as e:
        logger.error(f"Unexpected error in /query: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=f"Internal error: {e}")


@app.post("/ingest", response_model=IngestResponse)
async def ingest_endpoint(request: IngestRequest):
    """
    Ingest documents from the given path into the Endee vector database.
    """
    try:
        result = run_ingestion(request.path)
        return IngestResponse(**result)

    except FileNotFoundError as e:
        raise HTTPException(status_code=404, detail=str(e))
    except ConnectionError as e:
        logger.error(f"Endee connection error: {e}")
        raise HTTPException(status_code=503, detail=str(e))
    except Exception as e:
        logger.error(f"Unexpected error in /ingest: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=f"Internal error: {e}")
