"""
indexer.py

This module takes parsed code chunks,
generates embeddings, and inserts them
into the Endee vector database.
"""

import requests
from core.embedder import CodeEmbedder
from scripts.code_parser import parse_repository


# Endee server configuration
ENDEE_URL = "http://localhost:8080"
INDEX_NAME = "code_index"
VECTOR_DIM = 384


def create_index():
    """
    Create a vector index in Endee.
    """

    print("Creating index...")

    url = f"{ENDEE_URL}/api/v1/index/create"

    payload = {
        "index_name": INDEX_NAME,
        "dim": VECTOR_DIM,
        "space_type": "cosine"
    }

    response = requests.post(url, json=payload)

    if response.status_code == 200:
        print("Index created successfully.")
    else:
        print("Index creation response:", response.text)


def insert_vector(vector_id, vector, metadata):
    """
    Insert a vector into Endee.
    Endee expects a list of objects.
    """

    url = f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/vector/insert"

    payload = [
        {
            "id": vector_id,
            "vector": vector,
            "meta": metadata
        }
    ]

    response = requests.post(url, json=payload)

    if response.status_code == 200:
        print(f"Inserted {vector_id}")
    else:
        print("Insert failed:", response.status_code, response.text)


def index_repository(repo_path):
    """
    Parse repository, generate embeddings,
    and store them in Endee.
    """

    print("\nParsing repository...")

    chunks = parse_repository(repo_path)

    embedder = CodeEmbedder()

    print("\nIndexing code chunks...")

    for i, chunk in enumerate(chunks):

        code = chunk["code"]
        file_path = chunk["file"]

        # Generate embedding
        embedding = embedder.embed_text(code)

        vector_id = f"chunk_{i}"

        insert_vector(
            vector_id,
            embedding,
            file_path
        )

        if i % 10 == 0:
            print(f"Indexed {i} chunks")

    print("\nIndexing complete.")


if __name__ == "__main__":

    repo_path = "data/repos/requests"

    create_index()

    index_repository(repo_path)