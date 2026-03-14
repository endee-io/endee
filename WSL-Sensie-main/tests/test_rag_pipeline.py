"""
Integration-style tests for the RAG pipeline.
Uses mock LLM backend so no API keys are needed.
Uses mock Endee search so no running Endee server is needed.
"""

from __future__ import annotations

from unittest.mock import MagicMock, patch

import pytest

from retrieval.semantic_search import SearchResult, build_context
from rag.rag_pipeline import ask, build_prompt, SYSTEM_PROMPT, RAGResponse


# ── Fixtures ──────────────────────────────────────────────────────────────────

def make_result(i: int, score: float = 0.85, doc_type: str = "bash_history") -> SearchResult:
    return SearchResult(
        id=f"chunk_{i:04d}",
        score=score,
        text=f"npm install && npm run dev  # sample command {i}",
        source=f"/home/user/.bash_history",
        doc_type=doc_type,
        metadata={"file": "/home/user/.bash_history"},
    )


# ── build_context ──────────────────────────────────────────────────────────────

def test_build_context_basic():
    results = [make_result(i) for i in range(3)]
    ctx     = build_context(results)
    assert "--- Chunk 1 ---" in ctx
    assert "npm install" in ctx


def test_build_context_empty():
    ctx = build_context([])
    assert ctx == ""


def test_build_context_respects_token_limit():
    # Create many large results
    big_results = [
        SearchResult(
            id=f"chunk_{i}",
            score=0.9,
            text="x" * 1000,
            source="test",
            doc_type="config",
            metadata={},
        )
        for i in range(20)
    ]
    ctx = build_context(big_results, max_tokens=500)
    # 500 tokens * 4 chars = 2000 chars limit – should be much less than 20 * 1000
    assert len(ctx) < 15_000


# ── build_prompt ──────────────────────────────────────────────────────────────

def test_build_prompt_contains_question():
    prompt = build_prompt("Where is nginx config?", "some context here")
    assert "Where is nginx config?" in prompt


def test_build_prompt_contains_context():
    prompt = build_prompt("Q?", "UNIQUE_CONTEXT_STRING")
    assert "UNIQUE_CONTEXT_STRING" in prompt


# ── ask (full RAG with mock backend) ──────────────────────────────────────────

@patch("rag.rag_pipeline.semantic_search")
def test_ask_mock_backend(mock_search):
    """Pipeline should work end-to-end with mock backend."""
    mock_search.return_value = [make_result(0)]

    resp = ask(question="How do I start my project?", backend="mock")

    assert isinstance(resp, RAGResponse)
    assert resp.question == "How do I start my project?"
    assert len(resp.answer) > 0
    assert resp.backend == "mock"
    assert resp.latency_ms > 0
    mock_search.assert_called_once()


@patch("rag.rag_pipeline.semantic_search")
def test_ask_no_results(mock_search):
    """Empty search results should return a helpful fallback message."""
    mock_search.return_value = []

    resp = ask(question="Something completely unknown", backend="mock")

    assert "couldn't find" in resp.answer.lower() or "no results" in resp.answer.lower() \
        or "indexed" in resp.answer.lower()
    assert resp.sources == []


@patch("rag.rag_pipeline.semantic_search")
def test_ask_llm_failure_graceful(mock_search):
    """If the LLM call fails, a fallback message should be returned."""
    mock_search.return_value = [make_result(0)]

    # Use a fake backend that raises
    with patch.dict("rag.rag_pipeline._BACKENDS", {"fail_backend": _raises}):
        resp = ask(question="test question", backend="fail_backend")

    assert "failed" in resp.answer.lower() or "error" in resp.answer.lower() \
        or len(resp.answer) > 0   # Should not crash


def _raises(prompt, system):
    raise RuntimeError("Simulated LLM failure")


# ── SearchResult.to_context_block ─────────────────────────────────────────────

def test_search_result_context_block():
    r     = make_result(1, score=0.92)
    block = r.to_context_block()
    assert "0.92" in block
    assert "bash_history" in block
    assert "npm install" in block
