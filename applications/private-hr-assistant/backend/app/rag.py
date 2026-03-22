from __future__ import annotations

import httpx

from app.config import get_settings
from app.database import get_chunks_by_ids
from app.embeddings import embed_query
from app.endee_client import get_hr_index


def _normalize_endee_filter(filter_payload: dict | list | None) -> list[dict] | None:
    if filter_payload is None:
        return None
    if isinstance(filter_payload, list):
        return filter_payload
    return [filter_payload]


async def search_chunks(
    query: str,
    top_k: int = 8,
    endee_filter: dict | list | None = None,
) -> list[dict]:
    qv = embed_query(query)
    index = get_hr_index()
    flt = _normalize_endee_filter(endee_filter)
    raw = index.query(vector=qv, top_k=top_k, filter=flt)
    ids = [r.get("id") for r in raw if r.get("id")]
    texts = await get_chunks_by_ids(ids)
    ordered: list[dict] = []
    for r in raw:
        cid = r.get("id")
        if not cid or cid not in texts:
            continue
        row = dict(texts[cid])
        row["similarity"] = r.get("similarity")
        row["distance"] = r.get("distance")
        ordered.append(row)
    return ordered


def _format_context(hits: list[dict]) -> str:
    parts = []
    for i, h in enumerate(hits, start=1):
        cite = f"[{i}] doc={h['document_id'][:8]}… file={h['filename']} p.{h['page']}"
        parts.append(f"{cite}\n{h['text']}")
    return "\n\n---\n\n".join(parts)


async def generate_rag_answer(user_question: str, hits: list[dict]) -> str:
    if not hits:
        return "No relevant passages were retrieved; I cannot answer without sources."
    context = _format_context(hits)
    system = (
        "You are a privacy-conscious HR assistant. Answer only using the provided excerpts. "
        "Cite sources like [1], [2] matching the bracketed numbers. "
        "If the answer is not in the excerpts, say you do not have enough information."
    )
    user = f"Question: {user_question}\n\nExcerpts:\n{context}"
    s = get_settings()
    if s.use_cloud_llm and s.openai_api_key:
        return await _openai_chat(system, user)
    return await _ollama_chat(system, user)


async def _ollama_chat(system: str, user: str) -> str:
    s = get_settings()
    url = f"{s.ollama_base_url.rstrip('/')}/api/chat"
    payload = {
        "model": s.ollama_model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
        "stream": False,
    }
    try:
        async with httpx.AsyncClient(timeout=120.0) as client:
            r = await client.post(url, json=payload)
            r.raise_for_status()
            data = r.json()
            return (data.get("message") or {}).get("content") or str(data)
    except Exception as e:
        return (
            f"[Ollama unavailable: {e}]\n\n"
            "Retrieved context (answer manually or start Ollama):\n\n"
            f"{user[:8000]}"
        )


async def _openai_chat(system: str, user: str) -> str:
    s = get_settings()
    url = f"{s.openai_base_url.rstrip('/')}/chat/completions"
    headers = {"Authorization": f"Bearer {s.openai_api_key}"}
    payload = {
        "model": s.openai_model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
    }
    async with httpx.AsyncClient(timeout=120.0) as client:
        r = await client.post(url, json=payload, headers=headers)
        r.raise_for_status()
        data = r.json()
        return data["choices"][0]["message"]["content"]
