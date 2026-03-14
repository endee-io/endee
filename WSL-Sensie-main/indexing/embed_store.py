"""
indexing/embed_store.py
───────────────────────
Embedding generation and Endee vector-store integration.

Responsibilities
────────────────
  1. Load a sentence-transformers model for local embedding.
  2. Connect to the Endee vector database via the official Python SDK.
  3. Ensure the target index exists (create if absent).
  4. Batch-embed Chunk objects and upsert them into Endee.
  5. Expose a simple ``embed_text()`` helper for query-time embedding.

Endee SDK reference: https://docs.endee.io/python-sdk/usage
"""

from __future__ import annotations

import time
from typing import Any

from endee import Endee, Precision
from sentence_transformers import SentenceTransformer

from config import (
    EMBEDDING_MODEL,
    EMBEDDING_DIMENSION,
    ENDEE_BASE_URL,
    ENDEE_AUTH_TOKEN,
    ENDEE_INDEX_NAME,
)
from indexing.chunker import Chunk

# ── Module-level singletons (lazy-initialised) ─────────────────────────────────

_embedding_model: SentenceTransformer | None = None
_endee_client:    Endee                      | None = None


def _get_embedding_model() -> SentenceTransformer:
    """Return (and cache) the sentence-transformers model."""
    global _embedding_model
    if _embedding_model is None:
        print(f"[embed_store] Loading embedding model: {EMBEDDING_MODEL} …")
        _embedding_model = SentenceTransformer(EMBEDDING_MODEL)
        print(f"[embed_store] ✓  Model ready (dimension={EMBEDDING_DIMENSION})")
    return _embedding_model


def _get_endee_client() -> Endee:
    """Return (and cache) the connected Endee client."""
    global _endee_client
    if _endee_client is None:
        token = ENDEE_AUTH_TOKEN or ""
        _endee_client = Endee(token) if token else Endee()
        _endee_client.set_base_url(ENDEE_BASE_URL)
        print(f"[embed_store] ✓  Connected to Endee at {ENDEE_BASE_URL}")
    return _endee_client


# ── Index management ──────────────────────────────────────────────────────────

def ensure_index(
    index_name:  str = ENDEE_INDEX_NAME,
    dimension:   int = EMBEDDING_DIMENSION,
    space_type:  str = "cosine",
) -> Any:
    """
    Return the Endee Index object, creating it if it does not yet exist.

    Parameters
    ----------
    index_name:  Name of the vector index.
    dimension:   Must match the embedding model's output dimension.
    space_type:  Distance metric – ``"cosine"`` (default) or ``"euclidean"``.

    Returns
    -------
    endee.Index
    """
    client = _get_endee_client()

    # Check if the index already exists
    existing = [idx.name for idx in client.list_indexes()]
    if index_name not in existing:
        print(f"[embed_store] Creating Endee index '{index_name}' …")
        client.create_index(
            name=index_name,
            dimension=dimension,
            space_type=space_type,
            precision=Precision.INT8,   # memory-efficient quantisation
        )
        print(f"[embed_store] ✓  Index '{index_name}' created.")
    else:
        print(f"[embed_store] ✓  Using existing index '{index_name}'.")

    return client.get_index(name=index_name)


def delete_index(index_name: str = ENDEE_INDEX_NAME) -> None:
    """Drop the Endee index (use for a clean re-index)."""
    client = _get_endee_client()
    try:
        client.delete_index(name=index_name)
        print(f"[embed_store] ✓  Deleted index '{index_name}'.")
    except Exception as exc:  # noqa: BLE001
        print(f"[embed_store] ⚠  Could not delete index: {exc}")


# ── Embedding helpers ──────────────────────────────────────────────────────────

def embed_text(text: str) -> list[float]:
    """
    Convert a single text string to a normalised embedding vector.
    Used at query time (see retrieval/semantic_search.py).
    """
    model = _get_embedding_model()
    vector = model.encode(text, normalize_embeddings=True)
    return vector.tolist()


def embed_batch(texts: list[str], batch_size: int = 64) -> list[list[float]]:
    """
    Embed a list of strings in mini-batches for memory efficiency.
    """
    model   = _get_embedding_model()
    vectors = []

    for i in range(0, len(texts), batch_size):
        batch  = texts[i : i + batch_size]
        result = model.encode(batch, normalize_embeddings=True, show_progress_bar=False)
        vectors.extend(result.tolist())

    return vectors


# ── Store pipeline ─────────────────────────────────────────────────────────────

def store_chunks(
    chunks:     list[Chunk],
    index_name: str = ENDEE_INDEX_NAME,
    batch_size: int = 128,
) -> int:
    """
    Embed and upsert a list of Chunk objects into Endee.

    Process
    ───────
    1. Ensure the Endee index exists.
    2. Extract raw text from each Chunk.
    3. Generate embeddings in batches (sentence-transformers).
    4. Upsert (id, vector, meta) items to Endee in batches.

    Parameters
    ----------
    chunks:     Chunks produced by ``indexing.chunker.chunk_documents()``.
    index_name: Target Endee index.
    batch_size: How many vectors to send per upsert call.

    Returns
    -------
    int  – Number of chunks successfully upserted.
    """
    if not chunks:
        print("[embed_store] ⚠  No chunks to store.")
        return 0

    index = ensure_index(index_name)

    texts   = [c.text for c in chunks]
    vectors = embed_batch(texts)

    upserted = 0
    t_start  = time.time()

    for i in range(0, len(chunks), batch_size):
        batch_chunks  = chunks[i : i + batch_size]
        batch_vectors = vectors[i : i + batch_size]

        items = [
            chunk.to_endee_item(vector)
            for chunk, vector in zip(batch_chunks, batch_vectors)
        ]

        index.upsert(items)
        upserted += len(items)

        pct = upserted / len(chunks) * 100
        elapsed = time.time() - t_start
        print(
            f"[embed_store]  {upserted}/{len(chunks)} chunks upserted "
            f"({pct:.0f}%)  [{elapsed:.1f}s]"
        )

    print(f"[embed_store] ✓  Stored {upserted} vectors in '{index_name}'")
    return upserted


def get_index_stats(index_name: str = ENDEE_INDEX_NAME) -> dict[str, Any]:
    """Return basic statistics about the index."""
    client = _get_endee_client()
    try:
        index = client.get_index(name=index_name)
        info  = index.describe()
        return {
            "name":      info.get("name", index_name),
            "dimension": info.get("dimension", EMBEDDING_DIMENSION),
            "count":     info.get("count", 0),
            "space":     info.get("space_type", "cosine"),
        }
    except Exception as exc:  # noqa: BLE001
        return {"error": str(exc)}
