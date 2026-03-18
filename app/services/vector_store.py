import numpy as np

# In-memory storage (Endee-style)
vectors = []
metadata_store = []


def add_vector(vector, metadata):
    vectors.append(np.array(vector))
    metadata_store.append(metadata)


def search_vectors(query_vector, top_k=3):
    if len(vectors) == 0:
        return []

    query = np.array(query_vector)

    similarities = []
    for i, vec in enumerate(vectors):
        score = np.dot(query, vec) / (
            np.linalg.norm(query) * np.linalg.norm(vec)
        )
        similarities.append((score, i))

    similarities.sort(reverse=True)

    results = []
    for score, idx in similarities[:top_k]:
        results.append(metadata_store[idx])

    return results