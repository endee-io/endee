"""
rag/rag_pipeline.py
────────────────────
Retrieval-Augmented Generation (RAG) pipeline for WSL Sensei.

Pipeline
────────
  User query
      │
      ▼
  semantic_search()     → Endee vector DB → ranked chunks
      │
      ▼
  build_context()       → single formatted context string
      │
      ▼
  build_prompt()        → system + user messages
      │
      ▼
  LLM call              → Ollama / OpenAI / Anthropic / mock
      │
      ▼
  RAGResponse           → answer + sources + timing metadata

Supported LLM backends (set LLM_BACKEND in .env):
  - "ollama"     – local Ollama server (recommended for offline use)
  - "openai"     – OpenAI API (gpt-4o-mini etc.)
  - "anthropic"  – Anthropic API (claude-3-haiku etc.)
  - "mock"       – echo back context without an LLM (testing / CI)
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any

from config import (
    LLM_BACKEND,
    OPENAI_API_KEY,
    OPENAI_MODEL,
    ANTHROPIC_API_KEY,
    ANTHROPIC_MODEL,
    OLLAMA_BASE_URL,
    OLLAMA_MODEL,
    RAG_TOP_K,
    RAG_MAX_TOKENS,
    ENDEE_INDEX_NAME,
)
from retrieval.semantic_search import (
    SearchResult,
    semantic_search,
    build_context,
)


# ── Response type ─────────────────────────────────────────────────────────────

@dataclass
class RAGResponse:
    """
    Full output from one RAG pipeline run.

    Attributes
    ----------
    question    : Original user question.
    answer      : LLM-generated answer string.
    sources     : The retrieved chunks used as context.
    backend     : Which LLM backend produced the answer.
    latency_ms  : Total wall-clock time (retrieval + generation) in ms.
    context     : The formatted context passed to the LLM.
    """

    question:   str
    answer:     str
    sources:    list[SearchResult]
    backend:    str
    latency_ms: float
    context:    str = ""


# ── Prompt builder ────────────────────────────────────────────────────────────

SYSTEM_PROMPT = """\
You are WSL Sensei, an expert AI assistant specialising in Windows Subsystem \
for Linux (WSL) and Windows developer environments.

You answer questions by reasoning over CONTEXT extracted from the user's own \
development environment — their shell history, configuration files, scripts, \
and logs.

Rules:
1. Answer ONLY based on the provided CONTEXT.
2. Be concise and specific. Cite the source file or command when relevant.
3. If the context does not contain enough information, say so clearly and \
   suggest what the user could check manually.
4. Format shell commands in code blocks.
5. Never hallucinate file paths or commands not present in the context.
"""


def build_prompt(question: str, context: str) -> str:
    """Assemble the full user-turn message."""
    return (
        f"CONTEXT (from your development environment):\n"
        f"{'='*60}\n"
        f"{context}\n"
        f"{'='*60}\n\n"
        f"QUESTION: {question}\n\n"
        f"Answer clearly and cite relevant sources from the context above."
    )


# ── LLM backends ─────────────────────────────────────────────────────────────

def _call_ollama(prompt: str, system: str) -> str:
    from openai import OpenAI  # Ollama exposes an OpenAI-compatible API

    client = OpenAI(api_key="ollama", base_url=OLLAMA_BASE_URL)
    resp   = client.chat.completions.create(
        model=OLLAMA_MODEL,
        messages=[
            {"role": "system",  "content": system},
            {"role": "user",    "content": prompt},
        ],
        max_tokens=RAG_MAX_TOKENS,
        temperature=0.2,
    )
    return resp.choices[0].message.content or ""


def _call_openai(prompt: str, system: str) -> str:
    from openai import OpenAI

    client = OpenAI(api_key=OPENAI_API_KEY)
    resp   = client.chat.completions.create(
        model=OPENAI_MODEL,
        messages=[
            {"role": "system",  "content": system},
            {"role": "user",    "content": prompt},
        ],
        max_tokens=RAG_MAX_TOKENS,
        temperature=0.2,
    )
    return resp.choices[0].message.content or ""


def _call_anthropic(prompt: str, system: str) -> str:
    import anthropic

    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)
    msg    = client.messages.create(
        model=ANTHROPIC_MODEL,
        max_tokens=RAG_MAX_TOKENS,
        system=system,
        messages=[{"role": "user", "content": prompt}],
    )
    return msg.content[0].text


def _call_mock(prompt: str, _system: str) -> str:
    """Return the context verbatim – useful for testing without an LLM."""
    lines = prompt.split("\n")
    return (
        "[MOCK BACKEND – no LLM called]\n\n"
        + "\n".join(lines[:30])
        + "\n\n…(truncated for mock mode)"
    )


_BACKENDS = {
    "ollama":    _call_ollama,
    "openai":    _call_openai,
    "anthropic": _call_anthropic,
    "mock":      _call_mock,
}


# ── Main pipeline ─────────────────────────────────────────────────────────────

def ask(
    question:   str,
    top_k:      int  = RAG_TOP_K,
    index_name: str  = ENDEE_INDEX_NAME,
    backend:    str  = LLM_BACKEND,
    min_score:  float = 0.1,
) -> RAGResponse:
    """
    Run the full RAG pipeline for a natural-language question.

    Steps
    -----
    1. Semantic search → Endee returns top-k chunks.
    2. Build context string from ranked results.
    3. Construct the LLM prompt (system + user).
    4. Call the configured LLM backend.
    5. Return a RAGResponse with the answer and sources.

    Parameters
    ----------
    question:   Natural-language question from the user.
    top_k:      Number of chunks to retrieve.
    index_name: Endee index to search.
    backend:    LLM backend key (overrides .env default).
    min_score:  Minimum similarity threshold for retrieved chunks.

    Returns
    -------
    RAGResponse
    """
    t0 = time.time()

    # ── Step 1: Retrieve ──────────────────────────────────────────────────────
    results = semantic_search(
        query=question,
        top_k=top_k,
        index_name=index_name,
        min_score=min_score,
    )

    if not results:
        return RAGResponse(
            question=question,
            answer=(
                "I couldn't find relevant information in your indexed environment. "
                "Try re-running `python scripts/index_environment.py` to make sure "
                "your files have been indexed."
            ),
            sources=[],
            backend=backend,
            latency_ms=(time.time() - t0) * 1000,
        )

    # ── Step 2: Build context ─────────────────────────────────────────────────
    context = build_context(results)

    # ── Step 3 & 4: Generate answer ───────────────────────────────────────────
    llm_fn  = _BACKENDS.get(backend, _call_mock)
    prompt  = build_prompt(question, context)

    try:
        answer = llm_fn(prompt, SYSTEM_PROMPT)
    except Exception as exc:  # noqa: BLE001
        answer = (
            f"⚠  LLM backend '{backend}' failed: {exc}\n\n"
            f"Top retrieved context:\n\n{context[:800]}"
        )

    latency = (time.time() - t0) * 1000

    return RAGResponse(
        question=question,
        answer=answer,
        sources=results,
        backend=backend,
        latency_ms=latency,
        context=context,
    )
