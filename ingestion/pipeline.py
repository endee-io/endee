"""
Ingestion pipeline — orchestrates: load → chunk → embed → upsert into Endee.
"""

import logging
import requests
from typing import List, Dict, Optional

from sentence_transformers import SentenceTransformer

from config.settings import settings
from ingestion.loader import load_documents
from ingestion.chunker import chunk_documents

logger = logging.getLogger(__name__)

# ── Lazy-loaded embedding model singleton ──────────────────────────────
_model: Optional[SentenceTransformer] = None


def _get_model() -> SentenceTransformer:
    """Load the embedding model once and cache it."""
    global _model
    if _model is None:
        logger.info(f"Loading embedding model: {settings.embedding_model}")
        _model = SentenceTransformer(settings.embedding_model)
        logger.info("Embedding model loaded successfully")
    return _model


# ── Endee helpers ──────────────────────────────────────────────────────

def _ensure_index_exists() -> None:
    """Create the Endee index if it doesn't already exist."""
    url = f"{settings.endee_url}/api/v1/index/create"
    payload = {
        "name": settings.endee_index_name,
        "dimension": settings.embedding_dimension,
        "metric": "cosine",
    }

    try:
        resp = requests.post(url, json=payload, headers=settings.endee_headers, timeout=10)
        if resp.status_code == 200:
            logger.info(f"Index '{settings.endee_index_name}' created (or already exists)")
        else:
            # Many vector DBs return 409 or an error body when the index exists
            body = resp.text
            if "already exists" in body.lower() or resp.status_code == 409:
                logger.info(f"Index '{settings.endee_index_name}' already exists")
            else:
                logger.warning(f"Index creation response [{resp.status_code}]: {body}")
    except requests.RequestException as e:
        logger.error(f"Failed to connect to Endee at {settings.endee_url}: {e}")
        raise ConnectionError(
            f"Cannot reach Endee at {settings.endee_url}. "
            "Is the server running? Try: docker-compose up -d"
        ) from e


def _upsert_vectors(vectors: List[Dict]) -> None:
    """
    Upsert a batch of vectors into Endee.

    Each vector dict: { "id": str, "values": List[float], "metadata": dict }
    """
    url = f"{settings.endee_url}/api/v1/vectors/upsert"
    batch_size = 100  # Send in batches to avoid oversized payloads

    for i in range(0, len(vectors), batch_size):
        batch = vectors[i : i + batch_size]
        payload = {
            "index_name": settings.endee_index_name,
            "vectors": batch,
        }

        try:
            resp = requests.post(
                url, json=payload, headers=settings.endee_headers, timeout=30
            )
            resp.raise_for_status()
            logger.info(f"Upserted batch {i // batch_size + 1} ({len(batch)} vectors)")
        except requests.RequestException as e:
            logger.error(f"Upsert failed for batch starting at index {i}: {e}")
            raise


# ── Public API ─────────────────────────────────────────────────────────

def run_ingestion(path: str) -> Dict:
    """
    Full ingestion pipeline:
      1. Load documents from the given path
      2. Split into chunks
      3. Generate embeddings
      4. Ensure the Endee index exists
      5. Upsert vectors with metadata

    Args:
        path: File or directory path to ingest.

    Returns:
        Summary dict with counts.
    """
    logger.info(f"Starting ingestion from: {path}")

    # Step 1 — Load documents
    documents = load_documents(path)
    if not documents:
        logger.warning("No documents found to ingest")
        return {"documents": 0, "chunks": 0, "status": "no_documents"}

    # Step 2 — Chunk
    chunks = chunk_documents(
        documents,
        chunk_size=settings.chunk_size,
        chunk_overlap=settings.chunk_overlap,
    )

    # Step 3 — Generate embeddings
    model = _get_model()
    texts = [c["text"] for c in chunks]
    logger.info(f"Generating embeddings for {len(texts)} chunks …")
    embeddings = model.encode(texts, show_progress_bar=True, normalize_embeddings=True)

    # Step 4 — Ensure index
    _ensure_index_exists()

    # Step 5 — Build vector payloads and upsert
    vectors = []
    for chunk, embedding in zip(chunks, embeddings):
        vectors.append({
            "id": chunk["chunk_id"],
            "values": embedding.tolist(),
            "metadata": {
                "text": chunk["text"],  # Store text for retrieval
                **chunk["metadata"],
            },
        })

    _upsert_vectors(vectors)

    summary = {
        "documents": len(documents),
        "chunks": len(chunks),
        "status": "success",
    }
    logger.info(f"Ingestion complete: {summary}")
    return summary
