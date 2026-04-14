from sentence_transformers import SentenceTransformer
from endee import VectorDB

model = SentenceTransformer('all-MiniLM-L6-v2')
db = VectorDB()

# Load data
with open("data.txt", "r") as f:
    lines = f.readlines()

# Store in DB
embeddings = [model.encode(line) for line in lines]
db.add(vectors=embeddings, metadata=lines)

# Query
query = input("Ask a question: ")
query_vector = model.encode(query)

results = db.search(query_vector, top_k=1)

print("\nBest Answer:")
print(results[0]["metadata"])