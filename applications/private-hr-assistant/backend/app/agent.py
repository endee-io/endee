from __future__ import annotations

from typing import Any

from app.endee_client import get_hr_index
from app.filters import attribute_filters
from app.rag import generate_rag_answer, search_chunks


async def tool_semantic_search(query: str, top_k: int = 6, dept_code: str | None = None, doc_type: str | None = None) -> dict[str, Any]:
    hits = await search_chunks(query, top_k=top_k, endee_filter=attribute_filters(dept_code, doc_type))
    return {"tool": "semantic_search", "hits": hits, "count": len(hits)}


async def tool_hybrid_search(
    query: str,
    top_k: int = 6,
    sparse_indices: list[int] | None = None,
    sparse_values: list[float] | None = None,
    dept_code: str | None = None,
    doc_type: str | None = None,
) -> dict[str, Any]:
    index = get_hr_index()
    if not index.is_hybrid:
        dense_hits = await search_chunks(query, top_k=top_k, endee_filter=attribute_filters(dept_code, doc_type))
        return {
            "tool": "hybrid_search",
            "mode": "dense_fallback",
            "message": "Index is dense-only; ran semantic (dense) search. Create a hybrid Endee index for sparse+dense queries.",
            "hits": dense_hits,
            "count": len(dense_hits),
        }
    if sparse_indices is None or sparse_values is None:
        dense_hits = await search_chunks(query, top_k=top_k, endee_filter=attribute_filters(dept_code, doc_type))
        return {
            "tool": "hybrid_search",
            "mode": "dense_only_hybrid_index",
            "message": "Hybrid index detected but no sparse query supplied; returned dense query results.",
            "hits": dense_hits,
            "count": len(dense_hits),
        }
    from app.embeddings import embed_query

    qv = embed_query(query)
    flt = attribute_filters(dept_code, doc_type)
    raw = index.query(
        vector=qv,
        top_k=top_k,
        filter=flt,
        sparse_indices=sparse_indices,
        sparse_values=sparse_values,
    )
    from app.database import get_chunks_by_ids

    ids = [r.get("id") for r in raw if r.get("id")]
    texts = await get_chunks_by_ids(ids)
    hits: list[dict] = []
    for r in raw:
        cid = r.get("id")
        if not cid or cid not in texts:
            continue
        row = dict(texts[cid])
        row["similarity"] = r.get("similarity")
        hits.append(row)
    return {"tool": "hybrid_search", "mode": "hybrid", "hits": hits, "count": len(hits)}


def tool_list_sources(hits: list[dict]) -> dict[str, Any]:
    cites = []
    for i, h in enumerate(hits, start=1):
        cites.append(
            {
                "ref": i,
                "chunk_id": h.get("chunk_id"),
                "document_id": h.get("document_id"),
                "filename": h.get("filename"),
                "page": h.get("page"),
            }
        )
    return {"tool": "list_sources", "sources": cites}


async def tool_summarize_with_citations(
    question: str,
    top_k: int = 6,
    dept_code: str | None = None,
    doc_type: str | None = None,
) -> dict[str, Any]:
    hits = await search_chunks(question, top_k=top_k, endee_filter=attribute_filters(dept_code, doc_type))
    sources = tool_list_sources(hits)
    answer = await generate_rag_answer(question, hits)
    return {
        "tool": "summarize_with_citations",
        "answer": answer,
        "sources": sources["sources"],
        "hit_count": len(hits),
    }


async def run_agent_pipeline(task: str, top_k: int = 6, dept_code: str | None = None, doc_type: str | None = None) -> dict[str, Any]:
    """
    Deterministic mini-workflow: semantic_search → list_sources → summarize_with_citations.
    """
    steps: list[dict] = []
    s1 = await tool_semantic_search(task, top_k=top_k, dept_code=dept_code, doc_type=doc_type)
    steps.append(s1)
    hits = s1.get("hits") or []
    s2 = tool_list_sources(hits)
    steps.append(s2)
    s3 = await tool_summarize_with_citations(task, top_k=top_k, dept_code=dept_code, doc_type=doc_type)
    steps.append(s3)
    return {"task": task, "steps": steps, "final_answer": s3.get("answer"), "sources": s3.get("sources")}
