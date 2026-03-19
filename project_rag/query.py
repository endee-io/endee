"""
Standalone query engine — CLI tool.
Usage: python query.py "What is the termination clause?"
"""

import argparse
import json
import os

import msgpack
import numpy as np
import requests
from sentence_transformers import SentenceTransformer
from groq import Groq

ENDEE_URL  = os.getenv("ENDEE_URL",    "http://localhost:8080")
INDEX_NAME = os.getenv("INDEX_NAME",   "LEGALDOC")
GROQ_KEY   = os.getenv("GROQ_API_KEY", "")
TOP_K      = 4

def embed(model, text: str) -> list:
    return model.encode([text])[0].astype(np.float32).tolist()

def search(model, question: str, top_k: int = TOP_K) -> list:
    qvec = embed(model, question)
    r = requests.post(
        f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/search",
        json={"vector": qvec, "k": top_k},
        headers={"Content-Type": "application/json"},
        timeout=20,
    )
    if not r.ok:
        print(f"✗ Search error: {r.status_code}"); return []

    raw = msgpack.unpackb(r.content, raw=False)
    results = []
    if isinstance(raw, list):
        for item in raw:
            if isinstance(item, dict):
                mv = item.get("meta","")
                sc = item.get("score", item.get("distance"))
            elif isinstance(item, list) and len(item) > 2:
                sc, mv = item[1], item[2]
            else:
                continue
            if isinstance(mv, bytes):
                mv = mv.decode("utf-8", errors="replace")
            if isinstance(mv, str):
                try:
                    mv = json.loads(mv)
                except Exception:
                    mv = {"text": mv}
            results.append({"text": str(mv.get("text","")), "doc": str(mv.get("doc","")), "score": sc})
    return results

def answer(question: str, sources: list, temperature: float = 0.2) -> str:
    if not GROQ_KEY:
        ctx = "\n\n".join(s["text"] for s in sources)
        return f"[No Groq key — raw context]\n{ctx[:1000]}"
    context = "\n\n".join(s["text"] for s in sources)
    client  = Groq(api_key=GROQ_KEY)
    prompt  = f"Use the context below to answer the question accurately.\nContext:\n{context}\n\nQuestion: {question}\nAnswer:"
    resp    = client.chat.completions.create(
        model="llama-3.3-70b-versatile",
        messages=[{"role": "user", "content": prompt}],
        temperature=temperature,
        max_tokens=600,
    )
    return resp.choices[0].message.content

def main():
    parser = argparse.ArgumentParser(description="Legal RAG Query Engine")
    parser.add_argument("question",              help="Question to ask")
    parser.add_argument("--top-k",    type=int,   default=TOP_K)
    parser.add_argument("--temp",     type=float, default=0.2)
    parser.add_argument("--no-llm",   action="store_true", help="Return raw sources only")
    args = parser.parse_args()

    from dotenv import load_dotenv
    load_dotenv()

    model   = SentenceTransformer("all-MiniLM-L6-v2")
    sources = search(model, args.question, args.top_k)

    print(f"\n❓ {args.question}\n")
    print(f"📚 Retrieved {len(sources)} source(s):")
    for i, s in enumerate(sources, 1):
        score = f"{s['score']:.4f}" if isinstance(s.get('score'), (int,float)) else "N/A"
        print(f"\n  [{i}] Score {score} — {s['doc']}")
        print(f"  {s['text'][:200]}…")

    if not args.no_llm:
        print("\n💡 Answer:")
        print(answer(args.question, sources, args.temp))

if __name__ == "__main__":
    main()