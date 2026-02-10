
import openai
from endee import Endee
from sentence_transformers import SentenceTransformer

openai.api_key = "YOUR_API_KEY"

db = Endee(collection_name="rag_docs")
model = SentenceTransformer("all-MiniLM-L6-v2")

question = input("Ask a question: ")

query_embedding = model.encode(question).tolist()
docs = db.search(vector=query_embedding, top_k=3)

context = "\n".join([d["metadata"]["text"] for d in docs])

prompt = f"""
Answer the question using the context below:

Context:
{context}

Question:
{question}
"""

response = openai.ChatCompletion.create(
    model="gpt-3.5-turbo",
    messages=[{"role": "user", "content": prompt}]
)

print("\nAnswer:")
print(response.choices[0].message.content)
