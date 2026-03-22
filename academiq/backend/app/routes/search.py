from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from typing import Optional
from app.rag_pipeline import RAGPipeline

router = APIRouter()
pipeline = RAGPipeline()


class SearchRequest(BaseModel):
    query: str
    top_k: int = 5
    doc_name_filter: Optional[str] = None  # BONUS B: filtered search


@router.post("/search")
async def semantic_search(req: SearchRequest):
    """
    Semantic vector search over ingested documents.
    Returns top_k most similar chunks with similarity scores.
    Optionally filter by doc_name (BONUS B).
    """
    if not req.query.strip():
        raise HTTPException(status_code=400, detail="Query cannot be empty")
    results = pipeline.semantic_search(
        req.query,
        top_k=req.top_k,
        doc_name_filter=req.doc_name_filter
    )
    return {"query": req.query, "results": results, "count": len(results)}
