import pickle
import numpy as np
from sentence_transformers import SentenceTransformer

# Load model
model = SentenceTransformer('all-MiniLM-L6-v2')

# Load stored data
with open("vector_store.pkl", "rb") as f:
    docs, vectors = pickle.load(f)

# Ask question
query = input("Ask something: ")

# Convert query to vector
query_vec = model.encode(query)

# Cosine similarity
scores = np.dot(vectors, query_vec) / (
    np.linalg.norm(vectors, axis=1) * np.linalg.norm(query_vec)
)

# Get top results
top_idx = np.argsort(scores)[-3:][::-1]

# Combine context
context = "\n".join([docs[i] for i in top_idx])

# ✅ FINAL OUTPUT (NO OPENAI)
print("\n🤖 Answer:\n")
print(context)