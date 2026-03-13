import requests

ENDEE_URL = "http://localhost:8001"

def store_vector(id, embedding, text):
    data = {
        "id": id,
        "vector": embedding,
        "metadata": {"text": text}
    }

    requests.post(f"{ENDEE_URL}/vectors", json=data)


def search_vector(embedding):
    data = {
        "vector": embedding,
        "top_k": 3
    }

    r = requests.post(f"{ENDEE_URL}/search", json=data)
    return r.json()