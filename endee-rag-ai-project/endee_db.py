import json
import os

DB_FILE = "vector_store.json"

def store_embeddings(vectors, docs):

    data = []

    for i in range(len(docs)):
        data.append({
            "vector": vectors[i].tolist(),
            "text": docs[i]
        })

    with open(DB_FILE, "w") as f:
        json.dump(data, f)


def load_vectors():

    if not os.path.exists(DB_FILE):
        return []

    with open(DB_FILE, "r") as f:
        return json.load(f)