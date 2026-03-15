"""
embedder.py
-----------
Wraps sentence-transformers to produce 384-dimensional dense embeddings
(all-MiniLM-L6-v2) that match the Endee index dimension used throughout
the project.
"""

from __future__ import annotations

import logging
from typing import List

from sentence_transformers import SentenceTransformer

logger = logging.getLogger(__name__)

_MODEL_NAME = "all-MiniLM-L6-v2"
VECTOR_DIM = 384          # must match the Endee index dimension


class EmbeddingService:
    """Singleton-friendly embedding service backed by sentence-transformers."""

    def __init__(self, model_name: str = _MODEL_NAME) -> None:
        logger.info("Loading embedding model: %s", model_name)
        self.model = SentenceTransformer(model_name)
        self.dimension = VECTOR_DIM
        logger.info("Embedding model loaded (dim=%d)", self.dimension)

    # ------------------------------------------------------------------ #
    #  Public API                                                          #
    # ------------------------------------------------------------------ #

    def embed(self, text: str) -> List[float]:
        """Embed a single string and return a Python list of floats."""
        vec = self.model.encode(text, normalize_embeddings=True)
        return vec.tolist()

    def embed_batch(self, texts: List[str], batch_size: int = 64) -> List[List[float]]:
        """Embed a list of strings in batches and return a list of float lists."""
        vecs = self.model.encode(
            texts,
            normalize_embeddings=True,
            batch_size=batch_size,
            show_progress_bar=len(texts) > 100,
        )
        return vecs.tolist()
