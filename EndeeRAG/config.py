"""
Configuration module for the RAG system.
Centralizes all settings and environment variables.
"""
import os
from pathlib import Path
from dotenv import load_dotenv

load_dotenv()

# ─── Paths ───────────────────────────────────────────────────────────────
PROJECT_ROOT = Path(__file__).parent
DATA_DIR = PROJECT_ROOT / "data"
UPLOAD_DIR = DATA_DIR / "uploads"
CHUNKS_DIR = DATA_DIR / "chunks"

# Create directories
DATA_DIR.mkdir(exist_ok=True)
UPLOAD_DIR.mkdir(exist_ok=True)
CHUNKS_DIR.mkdir(exist_ok=True)

# ─── Endee Vector Database ───────────────────────────────────────────────
ENDEE_URL = os.getenv("ENDEE_URL", "http://localhost:8080")
ENDEE_AUTH_TOKEN = os.getenv("ENDEE_AUTH_TOKEN", "")
ENDEE_INDEX_NAME = "rag_hybrid_index"
ENDEE_DENSE_DIM = 384  # all-MiniLM-L6-v2 output dimension
ENDEE_SPACE_TYPE = "cosine"
ENDEE_SPARSE_MODEL = "endee_bm25"

# ─── Embedding Models ───────────────────────────────────────────────────
DENSE_MODEL_NAME = "all-MiniLM-L6-v2"
SPARSE_MODEL_NAME = "endee/bm25"

# ─── Chunking ────────────────────────────────────────────────────────────
CHUNK_SIZE = 512          # tokens
CHUNK_OVERLAP = 50        # tokens overlap
MAX_CHUNKS_PER_DOC = 500  # safety limit

# ─── Retrieval ───────────────────────────────────────────────────────────
DEFAULT_TOP_K = 5
DEFAULT_EF = 128

# ─── LLM (OpenAI ChatGPT) ───────────────────────────────────────────────
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "")
GOOGLE_API_KEY = os.getenv("GOOGLE_API_KEY", "")  # backup
LLM_MODEL = "gpt-3.5-turbo"
LLM_MAX_TOKENS = 2048
LLM_TEMPERATURE = 0.3

# ─── Encryption ──────────────────────────────────────────────────────────
ENCRYPTION_KEY = os.getenv("ENCRYPTION_KEY", "")

# ─── Benchmarks ──────────────────────────────────────────────────────────
BENCHMARK_QUERIES = [
    "What is machine learning?",
    "How does neural network work?",
    "Explain deep learning architectures",
    "What are transformers in NLP?",
    "How does backpropagation work?",
]
