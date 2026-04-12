import os
from fastapi import APIRouter, File, HTTPException, UploadFile
from pydantic import BaseModel

from app.services.embedding import embed_query, embed_texts
from app.services.rag import generate_answer
from app.services.retrieval import build_context, retrieve_top_chunks
from app.services.vector_store import InMemoryVectorStore
from app.utils.file_loader import chunk_text, load_text_from_file

router = APIRouter()
vector_store = InMemoryVectorStore()
INDEX_NAME = os.getenv("INDEX_NAME", "documents")


class UploadResponse(BaseModel):
    index: str
    stored_chunks: int
    message: str


class QueryRequest(BaseModel):
    question: str
    top_k: int = 5


class QueryResponse(BaseModel):
    answer: str
    sources: list


@router.post("/upload", response_model=UploadResponse)
async def upload_file(file: UploadFile = File(...)):
    content = await file.read()
    text = load_text_from_file(file.filename, content)

    if not text.strip():
        raise HTTPException(status_code=400, detail="Uploaded file contains no text.")

    chunks = chunk_text(text, chunk_size=350, overlap=75)
    if not chunks:
        raise HTTPException(status_code=400, detail="Unable to split document into chunks.")

    embeddings = embed_texts(chunks)
    if not embeddings:
        raise HTTPException(status_code=500, detail="Embedding generation returned no vectors.")

    documents = []
    for chunk_id, (chunk_text_value, embedding_vector) in enumerate(zip(chunks, embeddings)):
        metadata = {
            "filename": file.filename,
            "chunk_id": chunk_id,
            "text": chunk_text_value,
        }
        documents.append(
            {
                "id": f"{file.filename}::{chunk_id}",
                "vector": embedding_vector,
                "meta": metadata,
            }
        )

    vector_store.add_documents(documents)

    return {
        "index": INDEX_NAME,
        "stored_chunks": len(documents),
        "message": "Document uploaded and indexed successfully.",
    }


@router.post("/query", response_model=QueryResponse)
async def query_document(request: QueryRequest):
    if not request.question.strip():
        raise HTTPException(status_code=400, detail="Question cannot be empty.")

    query_embedding = embed_query(request.question)
    search_results = retrieve_top_chunks(vector_store, query_embedding, top_k=request.top_k)

    if not search_results:
        raise HTTPException(status_code=404, detail="No relevant content found for this query.")

    source_entries = []
    for result in search_results:
        meta = result.get("meta", {})
        text_snippet = meta.get("text", "")[:300] if isinstance(meta, dict) else ""
        source_entries.append(
            {
                "id": result.get("id"),
                "score": float(result.get("score", 0.0)),
                "filename": meta.get("filename") if isinstance(meta, dict) else None,
                "chunk_id": meta.get("chunk_id") if isinstance(meta, dict) else None,
                "snippet": text_snippet,
            }
        )

    context = build_context(search_results, max_chunks=3)
    answer = generate_answer(request.question, context)

    return {"answer": answer, "sources": source_entries}
