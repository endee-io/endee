"""
searcher.py

Semantic code search using Endee vector database
"""

import requests
import msgpack
from core.embedder import CodeEmbedder

ENDEE_URL = "http://localhost:8080"
INDEX_NAME = "code_index"


def search_code(query, top_k=5):

    print("\nEmbedding query...")

    embedder = CodeEmbedder()
    query_vector = embedder.embed_text(query)

    print("Searching Endee...")

    url = f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/search"

    payload = {
        "vector": query_vector,
        "k": top_k
    }

    response = requests.post(url, json=payload)

    if response.status_code != 200:
        print("Search failed:", response.text)
        return

    # Endee returns MsgPack
    results = msgpack.unpackb(response.content, raw=False)

    print("\nTop Results:\n")

    for i, result in enumerate(results):

        # Endee format
        score = result[0]
        vector_id = result[1]
        meta = result[2]

        # decode bytes if needed
        if isinstance(meta, bytes):
            meta = meta.decode()

        print(f"Result {i+1}")
        print("Score:", score)
        print("ID:", vector_id)
        print("File:", meta)
        print("-" * 40)


if __name__ == "__main__":

    query = input("\nEnter search query: ")
    search_code(query)