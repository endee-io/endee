from rag_app.embedding import generate_embedding
from rag_app.vector_store import search_vectors

def search_similar(query, top_k=3):
    query_embedding = generate_embedding(query)
    results = search_vectors(query_embedding, top_k)

    contexts = [text for _, text in results]
    return contexts