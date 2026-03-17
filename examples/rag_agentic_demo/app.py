#!/usr/bin/env python
import argparse
import glob
import os
from pathlib import Path
from typing import List, Tuple

from dotenv import load_dotenv
from openai import OpenAI
from endee import Endee, Precision

load_dotenv()

DEFAULT_INDEX = "rag_demo"
EMBED_MODEL = "text-embedding-3-small"
EMBED_DIM = 1536  # dimension for text-embedding-3-small
SAMPLE_DIR = Path(__file__).parent / "data" / "sample_docs"


def get_openai_client() -> OpenAI:
    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is required for embeddings and generation")
    return OpenAI(api_key=api_key)


def get_endee_client() -> Endee:
    auth = os.getenv("ENDEE_AUTH_TOKEN", "")
    base_url = os.getenv("ENDEE_BASE_URL", "http://localhost:8080/api/v1")
    client = Endee(auth) if auth else Endee()
    client.set_base_url(base_url)
    return client


def ensure_index(client: Endee, name: str, dim: int) -> None:
    try:
        client.get_index(name=name)
        return
    except Exception:
        pass
    client.create_index(name=name, dimension=dim, space_type="cosine", precision=Precision.INT8)


def load_corpus() -> List[Tuple[str, str]]:
    docs = []
    for path in glob.glob(str(SAMPLE_DIR / "*.txt")):
        with open(path, "r", encoding="utf-8") as f:
            text = f.read().strip()
            docs.append((Path(path).stem, text))
    if not docs:
        raise RuntimeError(f"No documents found in {SAMPLE_DIR}")
    return docs


def embed_texts(client: OpenAI, texts: List[str]) -> List[List[float]]:
    resp = client.embeddings.create(model=EMBED_MODEL, input=texts)
    return [item.embedding for item in resp.data]


def upsert_docs(index, ids: List[str], texts: List[str], embeddings: List[List[float]]):
    payload = []
    for doc_id, text, emb in zip(ids, texts, embeddings):
        payload.append({
            "id": doc_id,
            "vector": emb,
            "meta": {"source": doc_id, "preview": text[:200]}
        })
    index.upsert(payload)


def rewrite_query(llm: OpenAI, query: str) -> str:
    prompt = (
        "Rewrite the user query to be retrieval-friendly, short, and factual. "
        "Preserve intent and key nouns; drop pronouns and chit-chat."
    )
    res = llm.chat.completions.create(
        model="gpt-4o-mini",
        messages=[
            {"role": "system", "content": prompt},
            {"role": "user", "content": query},
        ],
        max_tokens=50,
    )
    return res.choices[0].message.content.strip()


def search(index, embedding: List[float], k: int):
    return index.query(vector=embedding, top_k=k, include_vectors=False)


def build_context(results) -> str:
    chunks = []
    for i, item in enumerate(results, 1):
        meta = item.get("meta") or {}
        preview = meta.get("preview") or ""
        chunks.append(f"[{i}] id={item['id']} score={item.get('similarity', item.get('distance'))}\n{preview}\n")
    return "\n".join(chunks)


def answer_question(llm: OpenAI, question: str, context: str) -> str:
    system = (
        "You are a concise assistant. Answer using only the provided context. "
        "If the answer is not in the context, say you cannot find it."
    )
    res = llm.chat.completions.create(
        model="gpt-4o-mini",
        messages=[
            {"role": "system", "content": system},
            {"role": "user", "content": f"Context:\n{context}\n\nQuestion: {question}"},
        ],
        max_tokens=200,
        temperature=0.2,
    )
    return res.choices[0].message.content.strip()


def interactive_loop(index, llm: OpenAI):
    print("Type your question (or 'exit' to quit)")
    while True:
        try:
            q = input("ask> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not q or q.lower() in {"exit", "quit"}:
            break
        rewritten = rewrite_query(llm, q)
        embedding = embed_texts(llm, [rewritten])[0]
        results = search(index, embedding, k=4)
        context = build_context(results)
        answer = answer_question(llm, q, context)
        print("\n---\n", answer, "\n---\n", sep="")


def main():
    parser = argparse.ArgumentParser(description="RAG + agentic demo with Endee")
    parser.add_argument("--index", default=DEFAULT_INDEX, help="Index name to use/create")
    parser.add_argument("--skip-ingest", action="store_true", help="Skip loading sample corpus")
    args = parser.parse_args()

    llm = get_openai_client()
    endee_client = get_endee_client()
    ensure_index(endee_client, args.index, EMBED_DIM)
    index = endee_client.get_index(name=args.index)

    if not args.skip_ingest:
        docs = load_corpus()
        ids, texts = zip(*docs)
        embeddings = embed_texts(llm, list(texts))
        upsert_docs(index, list(ids), list(texts), embeddings)
        print(f"Ingested {len(ids)} docs into index '{args.index}'")

    interactive_loop(index, llm)


if __name__ == "__main__":
    main()
