# search.py
import numpy as np
from embedder import get_embedding

def cosine_similarity(a, b):
    a = np.array(a)
    b = np.array(b)

    return np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-9)


def search(query, vectors, metadata, top_k=5):
    query_vec = get_embedding(query)

    results = []

    for vec, meta in zip(vectors, metadata):
        score = cosine_similarity(query_vec, vec)

        results.append({
            "score": float(score),
            "file": meta["file"],
            "path": meta["path"]
        })

    results.sort(key=lambda x: x["score"], reverse=True)

    return results[:top_k]