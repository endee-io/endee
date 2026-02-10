
from endee import Endee
from sentence_transformers import SentenceTransformer

# Initialize Endee
db = Endee(collection_name="rag_docs")

# Load embedding model
model = SentenceTransformer("all-MiniLM-L6-v2")

# Load data
with open("data/documents.txt", "r", encoding="utf-8") as f:
    docs = f.readlines()

# Store embeddings
for i, doc in enumerate(docs):
    embedding = model.encode(doc).tolist()
    db.add(
        id=str(i),
        vector=embedding,
        metadata={"text": doc}
    )

print("✅ Documents ingested into Endee")
