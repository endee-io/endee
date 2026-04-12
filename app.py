from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel
from typing import List, Dict, Any
import os
import traceback
from dotenv import load_dotenv

from backend.search import search_documents, ask_question
from backend.ingest import ingest_data

load_dotenv()

app = FastAPI(title="Campus Placement Copilot", version="1.0.0")

# CORS for frontend
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# Debug middleware - catches all exceptions and prints traceback
@app.middleware("http")
async def debug_exceptions(request: Request, call_next):
    try:
        response = await call_next(request)
        return response
    except Exception as e:
        # Print full traceback to terminal
        print("=" * 80)
        print("EXCEPTION CAUGHT IN MIDDLEWARE:")
        print(f"Path: {request.url.path}")
        print(f"Method: {request.method}")
        print("=" * 80)
        print(traceback.format_exc())
        print("=" * 80)
        
        # Re-raise to let FastAPI handle it
        raise

# Request/Response Models
class SearchRequest(BaseModel):
    query: str
    top_k: int = 5

class SearchResponse(BaseModel):
    query: str
    results: List[Dict[str, Any]]

class AskRequest(BaseModel):
    question: str
    top_k: int = 5

class AskResponse(BaseModel):
    question: str
    answer: str
    sources: List[Dict[str, Any]]

# Routes
@app.get("/health")
async def health():
    return {"status": "healthy"}

@app.post("/search", response_model=SearchResponse)
async def search(request: SearchRequest):
    try:
        results = await search_documents(request.query, request.top_k)
        return SearchResponse(query=request.query, results=results)
    except Exception as e:
        # Print traceback for debugging
        print("=" * 80)
        print(f"ERROR in /search endpoint:")
        print(traceback.format_exc())
        print("=" * 80)
        
        # Return error JSON during development
        return JSONResponse(
            status_code=500,
            content={"error": str(e), "detail": traceback.format_exc()}
        )

@app.post("/ask", response_model=AskResponse)
async def ask(request: AskRequest):
    try:
        answer, sources = await ask_question(request.question, request.top_k)
        return AskResponse(question=request.question, answer=answer, sources=sources)
    except Exception as e:
        # Print traceback for debugging
        print("=" * 80)
        print(f"ERROR in /ask endpoint:")
        print(traceback.format_exc())
        print("=" * 80)
        
        # Return error JSON during development
        return JSONResponse(
            status_code=500,
            content={"error": str(e), "detail": traceback.format_exc()}
        )

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "app:app",
        host=os.getenv("HOST", "0.0.0.0"),
        port=int(os.getenv("PORT", 8000)),
        reload=os.getenv("DEBUG", "False").lower() == "true"
    )
