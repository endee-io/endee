import numpy as np
from sentence_transformers import SentenceTransformer
from endee_db import load_vectors

model = SentenceTransformer("paraphrase-MiniLM-L3-v2")

def search(query, top_k=2):

    query_vector = model.encode(query)

    data = load_vectors()

    vectors = [d["vector"] for d in data]
    docs = [d["text"] for d in data]

    vectors = np.array(vectors)

    similarity = np.dot(vectors, query_vector)

    top_indices = similarity.argsort()[-top_k:][::-1]

    return [docs[i] for i in top_indices]