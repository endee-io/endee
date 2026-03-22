from __future__ import annotations

from functools import lru_cache

from sentence_transformers import SentenceTransformer

from app.config import get_settings


@lru_cache
def _model() -> SentenceTransformer:
    name = get_settings().embedding_model
    return SentenceTransformer(name)


def embed_texts(texts: list[str]) -> list[list[float]]:
    if not texts:
        return []
    m = _model()
    vectors = m.encode(
        texts,
        normalize_embeddings=True,
        show_progress_bar=False,
    )
    return vectors.tolist()


def embed_query(text: str) -> list[float]:
    return embed_texts([text])[0]
