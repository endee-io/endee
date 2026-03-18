from openai import OpenAI
from sentence_transformers import SentenceTransformer
from app.services.vector_store import search_vectors

client = OpenAI()

model = SentenceTransformer("all-MiniLM-L6-v2")


def retrieve(query):
    query_embedding = model.encode([query]).tolist()[0]
    results = search_vectors(query_embedding)
    return [item["text"] for item in results]


def generate_answer(context, query):
    prompt = f"Context:\n{context}\n\nQuestion:\n{query}"

    try:
        response = client.chat.completions.create(
            model="gpt-4.1-mini",
            messages=[{"role": "user", "content": prompt}]
        )
        return response.choices[0].message.content

    except Exception as e:
        return "⚠️ LLM service unavailable. Please check API quota or billing."

    return response.choices[0].message.content