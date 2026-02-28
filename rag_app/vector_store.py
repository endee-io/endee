import json
import os
import numpy as np

VECTOR_DB_PATH = "data/vector_store.json"

def cosine_similarity(a, b):
    a = np.array(a)
    b = np.array(b)
    return np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b))

def load_vectors():
    if not os.path.exists(VECTOR_DB_PATH):
        return []
    with open(VECTOR_DB_PATH, "r") as f:
        return json.load(f)

def save_vectors(vectors):
    os.makedirs("data", exist_ok=True)
    with open(VECTOR_DB_PATH, "w") as f:
        json.dump(vectors, f)

def add_vector(id, embedding, text):
    vectors = load_vectors()
    vectors.append({
        "id": id,
        "embedding": embedding,
        "text": text
    })
    save_vectors(vectors)

def search_vectors(query_embedding, top_k=3):
    vectors = load_vectors()

    scored = []
    for v in vectors:
        score = cosine_similarity(query_embedding, v["embedding"])
        scored.append((score, v["text"]))

    scored.sort(reverse=True, key=lambda x: x[0])
    return scored[:top_k]