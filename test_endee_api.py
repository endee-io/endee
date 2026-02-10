import httpx
import json

# Test create index endpoint
create_payload = {
    "index_name": "test_index",
    "dim": 8,
    "space_type": "cosine",
    "quant_bit": 8
}

print("=" * 50)
print("1. Creating Index...")
response = httpx.post('http://localhost:3001/api/v1/index/create', json=create_payload)
print(f'Status: {response.status_code}')
print(f'Response: {response.text[:300]}')

# Test insert vectors endpoint
print("\n" + "=" * 50)
print("2. Inserting Vectors...")
insert_payload = [
    {
        "vector": [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
        "id": "vector1",
        "metadata": {"text": "test"}
    }
]

response = httpx.post('http://localhost:3001/api/v1/index/test_index/vector/insert', json=insert_payload)
print(f'Status: {response.status_code}')
print(f'Response: {response.text[:300]}')

# Test search endpoint
print("\n" + "=" * 50)
print("3. Searching...")
search_payload = {
    "vector": [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
    "k": 5
}

response = httpx.post('http://localhost:3001/api/v1/index/test_index/search', json=search_payload)
print(f'Status: {response.status_code}')
print(f'Response: {response.text[:300]}')
