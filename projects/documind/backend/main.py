"""
main.py
-------
FastAPI application exposing the DocuMind RAG API.

Endpoints
---------
GET  /health              — liveness check
GET  /documents           — list ingested documents
POST /upload              — upload & ingest a document
DELETE /documents/{doc_id} — remove a document and its vectors from Endee
POST /query               — ask a question (full RAG pipeline)
"""

from __future__ import annotations

import logging
import os
from contextlib import asynccontextmanager
from typing import List, Optional

from dotenv import load_dotenv
from fastapi import FastAPI, File, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

load_dotenv()

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)

# ──────────────────────────────────────────────────────────────────────────── #
#  Lifespan – initialise heavy resources once at startup                       #
# ──────────────────────────────────────────────────────────────────────────── #

from rag_engine import RAGEngine
from document_processor import process_file

_rag: RAGEngine | None = None


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _rag
    logger.info("Initialising RAG engine …")
    _rag = RAGEngine()
    logger.info("RAG engine ready.")
    yield
    logger.info("Shutting down.")


# ──────────────────────────────────────────────────────────────────────────── #
#  App                                                                         #
# ──────────────────────────────────────────────────────────────────────────── #

app = FastAPI(
    title="DocuMind API",
    description="RAG-powered Document Q&A backed by Endee vector database.",
    version="1.0.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],   # tighten in production
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


def _get_rag() -> RAGEngine:
    if _rag is None:
        raise HTTPException(status_code=503, detail="RAG engine not ready.")
    return _rag


# ──────────────────────────────────────────────────────────────────────────── #
#  Schemas                                                                     #
# ──────────────────────────────────────────────────────────────────────────── #

class QueryRequest(BaseModel):
    question: str
    top_k:    int            = 5
    doc_id:   Optional[str] = None


class SourceItem(BaseModel):
    text:        str
    filename:    str
    chunk_index: int
    similarity:  float


class QueryResponse(BaseModel):
    question: str
    answer:   str
    sources:  List[SourceItem]


class DocumentInfo(BaseModel):
    doc_id:       str
    filename:     str
    total_chunks: int


# ──────────────────────────────────────────────────────────────────────────── #
#  Routes                                                                      #
# ──────────────────────────────────────────────────────────────────────────── #

@app.get("/health", tags=["System"])
def health():
    """Liveness probe."""
    return {"status": "ok", "service": "DocuMind"}


@app.get("/documents", response_model=List[DocumentInfo], tags=["Documents"])
def list_documents():
    """Return all ingested documents."""
    return _get_rag().list_documents()


@app.post("/upload", tags=["Documents"])
async def upload_document(file: UploadFile = File(...)):
    """
    Upload a document (PDF / TXT / MD).
    The file is chunked, embedded with sentence-transformers, and stored in Endee.
    """
    content = await file.read()
    filename = file.filename or "unknown"

    try:
        doc_info = process_file(content, filename)
    except (ValueError, ImportError) as exc:
        raise HTTPException(status_code=422, detail=str(exc))

    rag = _get_rag()
    try:
        rag.add_document(doc_info["chunks"], doc_info["doc_id"], filename)
    except Exception as exc:
        logger.exception("Ingestion failed for '%s'", filename)
        raise HTTPException(status_code=500, detail=f"Ingestion error: {exc}")

    return {
        "message":      "Document ingested successfully.",
        "doc_id":       doc_info["doc_id"],
        "filename":     filename,
        "total_chunks": doc_info["total_chunks"],
    }


@app.delete("/documents/{doc_id}", tags=["Documents"])
def delete_document(doc_id: str):
    """Remove a document and all its vectors from Endee."""
    try:
        _get_rag().delete_document(doc_id)
    except KeyError as exc:
        raise HTTPException(status_code=404, detail=str(exc))
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc))
    return {"message": f"Document '{doc_id}' deleted."}


@app.post("/query", response_model=QueryResponse, tags=["RAG"])
def query(req: QueryRequest):
    """
    Ask a natural-language question.
    Endee retrieves the most relevant chunks; an LLM (or fallback) generates the answer.
    """
    if not req.question.strip():
        raise HTTPException(status_code=422, detail="Question must not be empty.")

    try:
        result = _get_rag().answer(
            question=req.question,
            top_k=req.top_k,
            doc_id=req.doc_id,
        )
    except Exception as exc:
        logger.exception("Query failed")
        raise HTTPException(status_code=500, detail=str(exc))

    return result


# ──────────────────────────────────────────────────────────────────────────── #
#  Dev entry-point                                                             #
# ──────────────────────────────────────────────────────────────────────────── #

if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "main:app",
        host="0.0.0.0",
        port=int(os.getenv("PORT", 8000)),
        reload=True,
    )
