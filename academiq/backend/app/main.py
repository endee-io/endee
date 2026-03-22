from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from app.routes import ingest, search, query
from app.endee_client import EndeeManager
from app.config import settings

app = FastAPI(
    title="AcademIQ API",
    description="AI-powered academic research assistant using Endee vector database",
    version="1.0.0"
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"]
)

app.include_router(ingest.router, prefix="/api", tags=["Ingest"])
app.include_router(search.router, prefix="/api", tags=["Search"])
app.include_router(query.router, prefix="/api", tags=["RAG Query"])


@app.get("/api/health")
async def health():
    """Health check — verifies Endee connectivity."""
    try:
        em = EndeeManager(settings.endee_base_url, settings.endee_auth_token)
        endee_ok = em.is_healthy()
        index_info = em.describe_index()
        all_indexes = em.list_all_indexes()
    except Exception as e:
        endee_ok = False
        index_info = {"error": str(e)}
        all_indexes = []

    return {
        "status": "ok" if endee_ok else "degraded",
        "endee_connected": endee_ok,
        "endee_base_url": settings.endee_base_url,
        "index_info": index_info,
        "all_indexes": all_indexes
    }


@app.get("/api/indexes")
async def list_indexes():
    """BONUS A: List all indexes and their stats from Endee."""
    try:
        em = EndeeManager(settings.endee_base_url, settings.endee_auth_token)
        indexes = em.list_all_indexes()
        # Enrich with describe info for our main index
        main_stats = em.describe_index()
        return {
            "indexes": indexes,
            "academiq_index_stats": main_stats
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/api/vector/{vector_id}")
async def get_vector(vector_id: str):
    """BONUS D: Retrieve a stored vector and its metadata by ID."""
    try:
        em = EndeeManager(settings.endee_base_url, settings.endee_auth_token)
        result = em.get_vector(vector_id)
        if not result or "error" in result:
            raise HTTPException(status_code=404, detail=f"Vector '{vector_id}' not found")
        return result
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/")
async def root():
    return {"message": "AcademIQ is running. Visit /docs for API reference."}
