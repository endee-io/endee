import os
import requests
import msgpack
from groq import Groq
from embeddings import generate_embedding
from core.document_manager import get_text_by_id

BASE_URL = "http://localhost:8080"
INDEX_NAME = "research_index"

client = Groq(api_key=os.getenv("GROQ_API_KEY"))


def retrieve_context(query, top_k=3):
    query_vector = generate_embedding(query)

    url = f"{BASE_URL}/api/v1/index/{INDEX_NAME}/search"

    payload = {
        "vector": query_vector,
        "k": top_k
    }

    response = requests.post(url, json=payload)

    if response.status_code != 200:
        print("Search error:", response.text)
        return []

    decoded = msgpack.unpackb(response.content, raw=False)

    retrieved_ids = []
    for result in decoded:
        retrieved_ids.append(result[1])

    return retrieved_ids


def generate_answer(query, context_text):
    prompt = f"""
You are a helpful AI research assistant.

Use ONLY the context below to answer the question.
If the answer is not in the context, say you don't know.

Context:
{context_text}

Question:
{query}

Answer:
"""

    completion = client.chat.completions.create(
        model="llama-3.3-70b-versatile",
        messages=[{"role": "user", "content": prompt}],
        temperature=0.3,
        max_completion_tokens=512,
    )

    return completion.choices[0].message.content


def rag_query(query):
    retrieved_ids = retrieve_context(query)

    if not retrieved_ids:
        return "No relevant documents found."

    context_text = ""

    for vid in retrieved_ids:
        text = get_text_by_id(vid)
        if text:
            context_text += text + "\n"

    if not context_text:
        return "No matching documents in local store."

    return generate_answer(query, context_text)


if __name__ == "__main__":
    user_query = "Explain deep learning."
    answer = rag_query(user_query)

    print("\nFinal Answer:\n")
    print(answer)