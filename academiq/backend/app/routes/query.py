from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from app.rag_pipeline import RAGPipeline

router = APIRouter()
pipeline = RAGPipeline()


class QueryRequest(BaseModel):
    question: str
    top_k: int = 5


@router.post("/query")
async def rag_query(req: QueryRequest):
    """
    RAG question answering over ingested documents.
    Returns AI-generated answer grounded in retrieved chunks,
    along with the source passages used for grounding.
    """
    if not req.question.strip():
        raise HTTPException(status_code=400, detail="Question cannot be empty")
    result = pipeline.rag_query(req.question, top_k=req.top_k)
    return result
