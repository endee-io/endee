import chromadb
from sentence_transformers import SentenceTransformer

# We use ChromaDB as our local persistent datastore since Docker/Endee isn't available
client = chromadb.PersistentClient(path="./chroma_data")
model = SentenceTransformer("all-MiniLM-L6-v2")

def get_or_create_collection(name: str):
    return client.get_or_create_collection(name=name)

def generate_embedding(text: str) -> list[float]:
    """Generates embedding for a single string."""
    return model.encode(text).tolist()

def upsert_document(collection_name: str, doc_id: str, text: str, metadata: dict = None):
    """Embeds and upserts a document into the ChromaDB collection."""
    collection = get_or_create_collection(collection_name)
    embedding = generate_embedding(text)
    
    collection.add(
        ids=[doc_id],
        embeddings=[embedding],
        documents=[text],
        metadatas=[metadata] if metadata else None
    )

def query_collection(collection_name: str, query: str, top_k: int = 3):
    """Queries the collection using semantic similarity."""
    collection = get_or_create_collection(collection_name)
    query_embedding = generate_embedding(query)
    
    results = collection.query(
        query_embeddings=[query_embedding],
        n_results=top_k
    )
    
    return results

def get_all_documents(collection_name: str):
    """Retrieves all documents from a collection."""
    collection = get_or_create_collection(collection_name)
    return collection.get()
