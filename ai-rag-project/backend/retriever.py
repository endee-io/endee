from pathlib import Path

from sentence_transformers import SentenceTransformer
from backend.vector_store import VectorStore

MODEL_NAME = "all-MiniLM-L6-v2"
KNOWLEDGE_FILE = Path(__file__).resolve().parents[1] / "data" / "knowledge.txt"

model = SentenceTransformer(MODEL_NAME)
vector_db = None


def load_knowledge():

    global vector_db

    if not KNOWLEDGE_FILE.exists():
        raise FileNotFoundError(f"Knowledge file not found at: {KNOWLEDGE_FILE}")

    with open(KNOWLEDGE_FILE, encoding="utf-8") as f:
        docs = [line.strip() for line in f if line.strip()]

    embeddings = model.encode(docs)

    dim = embeddings.shape[1]

    vector_db = VectorStore(dim)

    vector_db.add(embeddings, docs)


def retrieve(query):

    query_vec = model.encode([query])

    results = vector_db.search(query_vec)

    return results