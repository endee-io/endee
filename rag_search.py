import numpy as np
from embeddings import embed_query

def cosine_similarity(a,b):
    return np.dot(a,b)/(np.linalg.norm(a)*np.linalg.norm(b))

def search(query, documents, embeddings):

    q = embed_query(query)

    scores = []

    for emb in embeddings:
        scores.append(cosine_similarity(q, emb))

    best_index = np.argmax(scores)

    return documents[best_index]