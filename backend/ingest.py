import os
import json
from typing import List, Dict, Any
from sentence_transformers import SentenceTransformer
from endee import Endee, Precision
from dotenv import load_dotenv

load_dotenv()

# Load model once
model = SentenceTransformer(os.getenv("MODEL_NAME", "all-MiniLM-L6-v2"))

# Endee client — local Docker server
client = Endee()
client.set_base_url("http://localhost:8080/api/v1")

INDEX_NAME = "placement_copilot"
DIMENSION = 384  # all-MiniLM-L6-v2 output size


def get_or_create_index():
    """Create index if not exists, return index object"""
    try:
        index = client.get_index(INDEX_NAME)
        print(f"✓ Index already exists: {INDEX_NAME}")
        return index
    except Exception:
        pass

    try:
        client.create_index(
            name=INDEX_NAME,
            dimension=DIMENSION,
            space_type="cosine",
            precision=Precision.INT8
        )
        print(f"✓ Created Endee index: {INDEX_NAME}")
        return client.get_index(INDEX_NAME)
    except Exception as e:
        print(f"✗ Failed to create index: {e}")
        raise


def generate_embedding(text: str) -> List[float]:
    """Generate embedding for a single text"""
    return model.encode(text).tolist()


def generate_embeddings(texts: List[str]) -> List[List[float]]:
    """Generate embeddings for multiple texts"""
    return model.encode(texts).tolist()


async def search_endee(query_embedding: List[float], top_k: int = 5) -> List[Dict]:
    """Search Endee index using query embedding"""
    try:
        index = client.get_index(INDEX_NAME)
        results = index.query(
            vector=query_embedding,
            top_k=top_k,
            ef=128,
            include_vectors=False
        )
        print(f"✓ Endee search returned {len(results)} results")
        return results
    except Exception as e:
        print(f"✗ Endee search failed: {e}")
        return []


async def ingest_data(file_path: str = "data/placement_data.json") -> Dict:
    """Load JSON data, chunk, embed, and upsert to Endee"""

    # Load JSON
    with open(file_path, 'r', encoding='utf-8') as f:
        documents = json.load(f)

    if not isinstance(documents, list):
        raise ValueError("JSON must contain a list of documents")

    print(f"Loaded {len(documents)} documents")

    # Get or create Endee index
    index = get_or_create_index()

    # Process documents into vectors
    vectors = []
    for idx, doc in enumerate(documents):
        content = doc.get("content", "")
        metadata = doc.get("metadata", {})

        if not content:
            continue

        chunks = [content] if len(content) <= 1000 else split_into_chunks(content, 1000)
        embeddings = generate_embeddings(chunks)

        for chunk_idx, (chunk, embedding) in enumerate(zip(chunks, embeddings)):
            vectors.append({
                "id": f"doc_{idx}_chunk_{chunk_idx}",
                "vector": embedding,
                "meta": {
                    **metadata,
                    "text": chunk,
                    "chunk_index": chunk_idx
                }
            })

    print(f"Generated {len(vectors)} vectors")

    # Upsert in batches of 1000 (Endee limit)
    batch_size = 1000
    for i in range(0, len(vectors), batch_size):
        batch = vectors[i:i + batch_size]
        index.upsert(batch)
        print(f"✓ Upserted batch {i // batch_size + 1} ({len(batch)} vectors)")

    print(f"✓ Ingested {len(vectors)} vectors into Endee index '{INDEX_NAME}'")
    return {"ingested": len(vectors), "documents": len(documents)}


def split_into_chunks(text: str, chunk_size: int = 1000) -> List[str]:
    """Split text into chunks at sentence boundaries"""
    sentences = text.replace('. ', '.\n').split('\n')
    chunks = []
    current_chunk = []
    current_length = 0

    for sentence in sentences:
        sentence = sentence.strip()
        if not sentence:
            continue

        if current_length + len(sentence) > chunk_size and current_chunk:
            chunks.append(' '.join(current_chunk))
            current_chunk = [sentence]
            current_length = len(sentence)
        else:
            current_chunk.append(sentence)
            current_length += len(sentence)

    if current_chunk:
        chunks.append(' '.join(current_chunk))

    return chunks