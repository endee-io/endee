from fastapi import FastAPI, HTTPException
from fastapi.responses import RedirectResponse
from pydantic import BaseModel, Field
from typing import Optional, List

from storage import VectorStore
from embedder import embed

app = FastAPI(
    title="Employee Memory AI",
    description="AI-powered employee memory search using vector embeddings",
    version="1.0.0"
)

store = VectorStore()

# -------- Models --------
class AddRequest(BaseModel):
    text: str = Field(..., min_length=2)
    employee_id: str
    department: str

class SearchRequest(BaseModel):
    query: str = Field(..., min_length=2)
    department: Optional[str] = None
    top_k: int = Field(5, ge=1, le=20)


# -------- Routes --------

# Redirect root → docs
@app.get("/", include_in_schema=False)
def home():
    return RedirectResponse(url="/docs")


# Health check (useful for deployment)
@app.get("/health")
def health():
    return {"status": "ok"}


# Add memory
@app.post("/add")
def add(req: AddRequest):
    try:
        vec = embed(req.text)

        store.add(
            vec,
            req.text,
            req.employee_id,
            req.department
        )

        return {
            "status": "success",
            "message": "Memory added"
        }

    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


# Search memory
@app.post("/search")
def search(req: SearchRequest):
    try:
        vec = embed(req.query)

        results = store.search(
            vec,
            req.top_k,
            req.department
        )

        return {
            "count": len(results),
            "results": results
        }

    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))