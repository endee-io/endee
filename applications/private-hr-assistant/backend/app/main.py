from __future__ import annotations

import contextlib
from contextlib import asynccontextmanager

from fastapi import Depends, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware

from app.agent import (
    run_agent_pipeline,
    tool_hybrid_search,
    tool_semantic_search,
    tool_summarize_with_citations,
)
from app.auth import create_access_token, verify_bearer_or_jwt
from app.config import get_settings
from app.database import init_db, list_documents
from app.endee_client import ensure_hr_index, get_endee, invalidate_endee_cache
from app.filters import attribute_filters
from app.models_api import (
    AgentRequest,
    ChatRequest,
    HybridSearchRequest,
    LoginRequest,
    SearchRequest,
    SimilarRecommendRequest,
)
from app.pdf_service import ingest_pdf
from app.rag import generate_rag_answer, search_chunks


@asynccontextmanager
async def lifespan(app: FastAPI):
    await init_db()
    with contextlib.suppress(Exception):
        ensure_hr_index()
    yield
    invalidate_endee_cache()


app = FastAPI(title="Privacy-First HR Assistant", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/health")
async def health():
    s = get_settings()
    endee_ok = False
    endee_err: str | None = None
    try:
        get_endee().list_indexes()
        endee_ok = True
    except Exception as e:
        endee_err = str(e)
    return {
        "status": "ok" if endee_ok else "degraded",
        "endee_reachable": endee_ok,
        "endee_error": endee_err,
        "use_cloud_llm": s.use_cloud_llm,
    }


@app.post("/auth/login")
async def login(body: LoginRequest):
    """Returns a JWT for mobile clients; you can also use API_BEARER_TOKEN directly."""
    token = create_access_token(sub=body.username)
    return {"access_token": token, "token_type": "bearer"}


@app.post("/documents/upload")
async def upload_document(
    file: UploadFile = File(...),
    dept_code: str = Form("unassigned"),
    doc_type: str = Form("general"),
    _: str = Depends(verify_bearer_or_jwt),
):
    if not file.filename or not file.filename.lower().endswith(".pdf"):
        raise HTTPException(400, "Only PDF uploads are supported")
    data = await file.read()
    if len(data) > 20 * 1024 * 1024:
        raise HTTPException(413, "PDF too large (max 20MB demo limit)")
    result = await ingest_pdf(data, file.filename, dept_code, doc_type)
    return result


@app.get("/documents")
async def documents(_: str = Depends(verify_bearer_or_jwt)):
    return await list_documents()


@app.post("/search")
async def search(body: SearchRequest, _: str = Depends(verify_bearer_or_jwt)):
    fl = attribute_filters(body.dept_code, body.doc_type)
    hits = await search_chunks(body.query, top_k=body.top_k, endee_filter=fl)
    return {"query": body.query, "hits": hits}


@app.post("/chat")
async def chat(body: ChatRequest, _: str = Depends(verify_bearer_or_jwt)):
    last_user = next((m.content for m in reversed(body.messages) if m.role == "user"), None)
    if not last_user:
        raise HTTPException(400, "Need at least one user message")
    fl = attribute_filters(body.dept_code, body.doc_type)
    hits = await search_chunks(last_user, top_k=body.top_k, endee_filter=fl)
    answer = await generate_rag_answer(last_user, hits)
    sources = [
        {
            "chunk_id": h["chunk_id"],
            "document_id": h["document_id"],
            "filename": h["filename"],
            "page": h["page"],
        }
        for h in hits
    ]
    return {"answer": answer, "sources": sources, "retrieval_count": len(hits)}


@app.post("/recommendations/similar")
async def similar(body: SimilarRecommendRequest, _: str = Depends(verify_bearer_or_jwt)):
    from app.database import get_chunk_text

    text = await get_chunk_text(body.chunk_id)
    if not text:
        raise HTTPException(404, "Unknown chunk_id")
    fl = attribute_filters(body.dept_code, body.doc_type)
    seed = text[:2000]
    hits = await search_chunks(seed, top_k=body.top_k + 3, endee_filter=fl)
    filtered = [h for h in hits if h.get("chunk_id") != body.chunk_id][: body.top_k]
    return {"chunk_id": body.chunk_id, "recommendations": filtered}


@app.post("/agent/run")
async def agent_run(body: AgentRequest, _: str = Depends(verify_bearer_or_jwt)):
    return await run_agent_pipeline(body.task, top_k=body.top_k, dept_code=body.dept_code, doc_type=body.doc_type)


@app.post("/tools/semantic_search")
async def tool_semantic_http(body: SearchRequest, _: str = Depends(verify_bearer_or_jwt)):
    return await tool_semantic_search(body.query, top_k=body.top_k, dept_code=body.dept_code, doc_type=body.doc_type)


@app.post("/tools/hybrid_search")
async def tool_hybrid_http(body: HybridSearchRequest, _: str = Depends(verify_bearer_or_jwt)):
    return await tool_hybrid_search(
        body.query,
        top_k=body.top_k,
        sparse_indices=body.sparse_indices,
        sparse_values=body.sparse_values,
        dept_code=body.dept_code,
        doc_type=body.doc_type,
    )


@app.post("/tools/summarize_with_citations")
async def tool_summarize_http(body: SearchRequest, _: str = Depends(verify_bearer_or_jwt)):
    return await tool_summarize_with_citations(
        body.query, top_k=body.top_k, dept_code=body.dept_code, doc_type=body.doc_type
    )
