from sentence_transformers import SentenceTransformer
import numpy as np

# initialize model
embedder = SentenceTransformer('all-MiniLM-L6-v2')

data = [
    "Artificial Intelligence enables automation",
    "Machine learning is a subset of AI",
    "Deep learning uses layered neural networks",
    "Python is commonly used for AI applications"
]

# embeddings
data_vectors = embedder.encode(data)

# user query
user_query = "Explain machine learning"
query_vector = embedder.encode([user_query])[0]

# similarity
similarity_scores = np.dot(data_vectors, query_vector)

# output
result = data[np.argmax(similarity_scores)]

print("Query:", user_query)
print("Result:", result)
