import os

from sentence_transformers import SentenceTransformer

EMBEDDING_MODEL = os.getenv("EMBEDDING_MODEL", "all-MiniLM-L6-v2")
MODEL = SentenceTransformer(EMBEDDING_MODEL)


def embed_texts(texts):
    embeddings = MODEL.encode(
        texts,
        show_progress_bar=False,
        convert_to_numpy=True,
        normalize_embeddings=True,
    )
    return [embedding.tolist() for embedding in embeddings]


def embed_query(query):
    embedding = MODEL.encode(
        query,
        show_progress_bar=False,
        convert_to_numpy=True,
        normalize_embeddings=True,
    )
    return embedding.tolist()
