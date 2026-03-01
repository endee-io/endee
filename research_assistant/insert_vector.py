import requests
from embeddings import generate_embedding

BASE_URL = "http://localhost:8080"
INDEX_NAME = "research_index"

def insert_text(text, vector_id="vec_001"):
    embedding = generate_embedding(text)

    url = f"{BASE_URL}/api/v1/index/{INDEX_NAME}/vector/insert"

    payload = [
        {
            "id": vector_id,
            "vector": embedding,
            "meta": {
                "text": text
            }
        }
    ]

    response = requests.post(url, json=payload)

    print("Status Code:", response.status_code)
    print("Response:", response.text)


if __name__ == "__main__":
    sample_text = "Machine learning enables computers to learn from data."
    insert_text(sample_text)