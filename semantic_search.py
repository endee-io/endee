import numpy as np
from embeddings import model
from data_store import documents, embeddings


def search(query):

    if len(documents) == 0:
        return "No documents indexed yet. Please upload a PDF first."

    query_vector = model.encode(query)

    scores = []

    for emb in embeddings:
        score = np.dot(query_vector, emb)
        scores.append(score)

    if len(scores) == 0:
        return "No searchable data available."

    best_index = np.argmax(scores)

    return documents[best_index]