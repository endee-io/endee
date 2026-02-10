
from endee import Endee
from sentence_transformers import SentenceTransformer

db = Endee(collection_name="rag_docs")
model = SentenceTransformer("all-MiniLM-L6-v2")

query = input("Enter your question: ")
query_embedding = model.encode(query).tolist()

results = db.search(
    vector=query_embedding,
    top_k=3
)

for res in results:
    print(res["metadata"]["text"])
