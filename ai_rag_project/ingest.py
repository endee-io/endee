import pickle
from sentence_transformers import SentenceTransformer

# Load model
model = SentenceTransformer('all-MiniLM-L6-v2')

# Sample data
docs = [
    "Endee is a vector database designed for AI applications.",
    "It helps store embeddings and perform similarity search.",
    "AI systems use vector databases in retrieval augmented generation.",
    "Machine learning models convert text into vectors."
]

# Create embeddings
vectors = model.encode(docs)

# Save as local vector DB
with open("vector_store.pkl", "wb") as f:
    pickle.dump((docs, vectors), f)

print("✅ Data stored successfully")