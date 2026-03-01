import requests
import msgpack
from embeddings import generate_embedding

BASE_URL = "http://localhost:8080"
INDEX_NAME = "research_index"

def search_text(query, top_k=3):
    query_vector = generate_embedding(query)

    url = f"{BASE_URL}/api/v1/index/{INDEX_NAME}/search"

    payload = {
        "vector": query_vector,
        "k": top_k
    }

    response = requests.post(url, json=payload)

    print("Status Code:", response.status_code)

    if response.status_code == 200:
        decoded = msgpack.unpackb(response.content, raw=False)

        print("\nFormatted Results:")
        for result in decoded:
            score = result[0]
            vector_id = result[1]
            raw_meta = result[2]

            # Decode metadata if present
            meta = None
            if raw_meta:
                try:
                    meta = msgpack.unpackb(raw_meta, raw=False)
                except:
                    meta = raw_meta

            print(f"\nID: {vector_id}")
            print(f"Score: {score}")
            print(f"Metadata: {meta}")

    else:
        print("Error:", response.text)


if __name__ == "__main__":
    query = "How do computers learn from data?"
    search_text(query)