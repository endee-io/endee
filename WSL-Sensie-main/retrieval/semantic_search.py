"""
retrieval/semantic_search.py
─────────────────────────────
Semantic search over the Endee vector index.

Flow
────
  query_text
      │
      ▼
  embed_text()          ← sentence-transformers (local model)
      │
      ▼
  index.query()         ← Endee vector similarity search
      │
      ▼
  list[SearchResult]    ← ranked results with text + metadata
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from endee import Endee, Precision

from config import (
    ENDEE_BASE_URL,
    ENDEE_AUTH_TOKEN,
    ENDEE_INDEX_NAME,
    RAG_TOP_K,
)
from indexing.embed_store import embed_text, _get_endee_client


# ── Result type ───────────────────────────────────────────────────────────────

@dataclass
class SearchResult:
    """
    One result returned by semantic search.

    Attributes
    ----------
    id          : The vector ID in Endee (content-hash chunk ID).
    score       : Cosine similarity score [0, 1].
    text        : The raw chunk text.
    source      : Origin file or system (e.g. ~/.bashrc).
    doc_type    : Coarse category (bash_history, config, script, log …).
    metadata    : All metadata fields stored alongside the vector.
    """

    id:       str
    score:    float
    text:     str
    source:   str
    doc_type: str
    metadata: dict[str, Any]

    def to_context_block(self) -> str:
        """Format the result as a labelled context block for the RAG prompt."""
        return (
            f"[Source: {self.source}  |  Type: {self.doc_type}  |  "
            f"Similarity: {self.score:.2f}]\n"
            f"{self.text}"
        )


# ── Search function ───────────────────────────────────────────────────────────

def semantic_search(
    query:      str,
    top_k:      int  = RAG_TOP_K,
    index_name: str  = ENDEE_INDEX_NAME,
    min_score:  float = 0.0,
    filter_type: str | None = None,
) -> list[SearchResult]:
    """
    Run a semantic search against the Endee vector index.

    Parameters
    ----------
    query:
        Natural-language question or keyword phrase.
    top_k:
        Maximum number of results to return.
    index_name:
        Endee index to query.
    min_score:
        Minimum cosine similarity to include in results (0 = no filter).
    filter_type:
        Optional doc_type filter, e.g. ``"bash_history"``.

    Returns
    -------
    list[SearchResult]
        Results ordered by descending similarity score.
    """
    # 1. Embed the query
    query_vector = embed_text(query)

    # 2. Connect to Endee and retrieve the index
    client = _get_endee_client()
    index  = client.get_index(name=index_name)

    # 3. Execute the vector search
    raw_results = index.query(vector=query_vector, top_k=top_k)

    # 4. Parse + filter results
    results: list[SearchResult] = []
    for item in raw_results:
        score    = float(item.get("similarity", item.get("score", 0.0)))
        meta     = item.get("meta", {})
        text     = meta.get("text", "")
        source   = meta.get("source", "unknown")
        doc_type = meta.get("doc_type", "unknown")

        if score < min_score:
            continue
        if filter_type and doc_type != filter_type:
            continue
        if not text.strip():
            continue

        results.append(
            SearchResult(
                id=item.get("id", ""),
                score=score,
                text=text,
                source=source,
                doc_type=doc_type,
                metadata=meta,
            )
        )

    return results


def build_context(
    results: list[SearchResult],
    max_tokens: int = 2000,
) -> str:
    """
    Concatenate ranked search results into a single context string
    suitable for inclusion in a RAG prompt.

    Parameters
    ----------
    results:    Ordered list of SearchResult objects.
    max_tokens: Rough character limit (1 token ≈ 4 chars).

    Returns
    -------
    str – Formatted context block.
    """
    max_chars = max_tokens * 4
    lines     = []
    total     = 0

    for i, r in enumerate(results, 1):
        block = f"--- Chunk {i} ---\n{r.to_context_block()}"
        if total + len(block) > max_chars:
            break
        lines.append(block)
        total += len(block)

    return "\n\n".join(lines)
