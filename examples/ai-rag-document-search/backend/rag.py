from embeddings import create_embedding
from vector_store import search_vector
from openai import OpenAI
from config import OPENAI_API_KEY

client = OpenAI(api_key=OPENAI_API_KEY)

def generate_answer(question):
    query_embedding = create_embedding(question)
    results = search_vector(query_embedding)

    context = ""
    for item in results.get("matches", []):
        context += item["metadata"]["text"] + "\n"

    prompt = f"""
Use the following context to answer the question.

Context:
{context}

Question:
{question}
"""

    response = client.chat.completions.create(
        model="gpt-4o-mini",
        messages=[{"role":"user","content":prompt}]
    )

    return response.choices[0].message.content