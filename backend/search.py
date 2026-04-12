import os
import httpx
from typing import List, Dict, Tuple
from dotenv import load_dotenv
from backend.ingest import generate_embedding, search_endee

load_dotenv()

GROQ_API_KEY = os.getenv("GROQ_API_KEY")
GROQ_MODEL = "llama-3.1-8b-instant"


async def search_documents(query: str, top_k: int = 5) -> List[Dict]:
    """Search for relevant documents using Endee vector search"""

    query_embedding = generate_embedding(query)
    results = await search_endee(query_embedding, top_k)

    formatted_results = []
    for result in results:
        meta = result.get("meta", {})
        formatted_results.append({
            "text": meta.get("text", ""),
            "score": result.get("similarity", 0.0),
            "metadata": {k: v for k, v in meta.items() if k != "text"}
        })

    return formatted_results


async def ask_question(question: str, top_k: int = 5) -> Tuple[str, List[Dict]]:
    """Answer question using RAG — Endee retrieval + Groq LLM"""

    query_embedding = generate_embedding(question)
    results = await search_endee(query_embedding, top_k)

    if not results:
        return "No relevant information found in the database.", []

    # Build sources and context
    sources = []
    context_parts = []

    for idx, result in enumerate(results, 1):
        meta = result.get("meta", {})
        text = meta.get("text", "")

        sources.append({
            "text": text,
            "score": result.get("similarity", 0.0),
            "metadata": {k: v for k, v in meta.items() if k != "text"}
        })
        context_parts.append(f"[{idx}] {text}")

    context = "\n\n".join(context_parts)

    # Call Groq LLM
    answer = await call_groq(question, context)

    return answer, sources


async def call_groq(question: str, context: str) -> str:
    """Call Groq LLaMA3 with retrieved context"""

    if not GROQ_API_KEY:
        # Fallback if no Groq key
        return f"Based on the retrieved information:\n\n{context}"

    prompt = f"""You are a Campus Placement Copilot. Answer the student's question using only the context below.
If the answer is not in the context, say "I don't have information on that."

Context:
{context}

Question: {question}

Answer:"""

    try:
        async with httpx.AsyncClient(timeout=30) as http:
            response = await http.post(
                "https://api.groq.com/openai/v1/chat/completions",
                headers={
                    "Authorization": f"Bearer {GROQ_API_KEY}",
                    "Content-Type": "application/json"
                },
                json={
                    "model": GROQ_MODEL,
                    "messages": [{"role": "user", "content": prompt}],
                    "max_tokens": 512,
                    "temperature": 0.3
                }
            )
            response.raise_for_status()
            return response.json()["choices"][0]["message"]["content"].strip()

    except Exception as e:
        print(f"✗ Groq API error: {e}")
        return f"Based on the retrieved information:\n\n{context}"