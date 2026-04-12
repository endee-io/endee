from dotenv import load_dotenv
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from app.routes import router

load_dotenv()

app = FastAPI(
    title="Endee RAG Search API",
    description="FastAPI backend for a document retrieval and generation system using sentence-transformers and local text generation.",
    version="0.1.0",
)
app.include_router(router)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/")
def health_check():
    return {"status": "ok", "message": "Endee RAG Search API is running."}
