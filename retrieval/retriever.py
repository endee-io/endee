"""
Retriever — converts a query into an embedding, searches Endee,
and returns formatted context for the LLM.
"""

import logging
import requests
from typing import List, Dict, Optional

from sentence_transformers import SentenceTransformer

from config.settings import settings

logger = logging.getLogger(__name__)

# ── Lazy-loaded embedding model singleton ──────────────────────────────
_model: Optional[SentenceTransformer] = None


def _get_model() -> SentenceTransformer:
    """Load the embedding model once and cache it."""
    global _model
    if _model is None:
        logger.info(f"Loading embedding model: {settings.embedding_model}")
        _model = SentenceTransformer(settings.embedding_model)
    return _model


def retrieve(query: str, top_k: Optional[int] = None) -> List[Dict]:
    """
    Retrieve the most relevant document chunks for a query.

    Args:
        query:  The user's natural-language question.
        top_k:  Number of results (defaults to settings.top_k).

    Returns:
        List of result dicts, each with 'text', 'score', and 'metadata'.
    """
    top_k = top_k or settings.top_k

    # Step 1 — Embed the query
    model = _get_model()
    query_vector = model.encode(query, normalize_embeddings=True).tolist()

    # Step 2 — Search Endee
    url = f"{settings.endee_url}/api/v1/vectors/search"
    payload = {
        "index_name": settings.endee_index_name,
        "vector": query_vector,
        "top_k": top_k,
    }

    try:
        resp = requests.post(
            url, json=payload, headers=settings.endee_headers, timeout=15
        )
        resp.raise_for_status()
        data = resp.json()
    except requests.RequestException as e:
        logger.error(f"Endee search failed: {e}")
        raise ConnectionError(
            f"Endee search failed. Is the server running at {settings.endee_url}?"
        ) from e

    # Step 3 — Parse results into a clean format
    results = []
    raw_results = data.get("results", data.get("matches", []))

    for item in raw_results:
        metadata = item.get("metadata", {})
        results.append({
            "text": metadata.get("text", ""),
            "score": item.get("score", item.get("distance", 0.0)),
            "metadata": {
                "source": metadata.get("source", "unknown"),
                "filename": metadata.get("filename", "unknown"),
                "chunk_index": metadata.get("chunk_index", 0),
            },
        })

    logger.info(f"Retrieved {len(results)} results for query: '{query[:80]}…'")
    return results


def format_context(results: List[Dict]) -> str:
    """
    Format retrieved results into a context string for the LLM prompt.

    Each chunk is numbered and includes its source for traceability.
    """
    if not results:
        return "No relevant context found."

    parts = []
    for i, result in enumerate(results, 1):
        source = result["metadata"].get("filename", "unknown")
        score = result.get("score", 0)
        parts.append(
            f"[Source {i}: {source} (relevance: {score:.3f})]\n{result['text']}"
        )

    return "\n\n---\n\n".join(parts)
