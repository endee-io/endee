import requests
from embeddings import generate_embedding
from core.document_manager import add_document

BASE_URL = "http://localhost:8080"
INDEX_NAME = "research_index"


def ingest_document(text):
    # Save locally
    doc_id = add_document(text)

    # Generate embedding
    embedding = generate_embedding(text)

    url = f"{BASE_URL}/api/v1/index/{INDEX_NAME}/vector/insert"

    payload = [
        {
            "id": doc_id,
            "vector": embedding,
            "meta": {
                "text": text
            }
        }
    ]

    response = requests.post(url, json=payload)

    print("Insert Status:", response.status_code)

    return doc_id