"""Unit tests for indexing/chunker.py (no external deps required)."""

import pytest
from indexing.chunker import (
    Chunk,
    CharacterChunker,
    LineChunker,
    chunk_documents,
)


# ── Fixtures ──────────────────────────────────────────────────────────────────

SAMPLE_BASH_DOC = {
    "content": "\n".join([f"git commit -m 'fix #{i}'" for i in range(50)]),
    "source":  "/home/user/.bash_history",
    "type":    "bash_history",
    "metadata": {"file": "/home/user/.bash_history"},
}

SAMPLE_CONFIG_DOC = {
    "content": (
        "server {\n"
        "    listen 80;\n"
        "    server_name localhost;\n"
        "    location / {\n"
        "        proxy_pass http://127.0.0.1:8000;\n"
        "    }\n"
        "}\n" * 20   # repeat to ensure multiple chunks
    ),
    "source":  "/etc/nginx/nginx.conf",
    "type":    "config",
    "metadata": {"file": "/etc/nginx/nginx.conf"},
}


# ── Chunk ID stability ─────────────────────────────────────────────────────────

def test_chunk_id_deterministic():
    id1 = Chunk.make_id("hello world", "src", 0)
    id2 = Chunk.make_id("hello world", "src", 0)
    assert id1 == id2


def test_chunk_id_different_content():
    id1 = Chunk.make_id("hello world",  "src", 0)
    id2 = Chunk.make_id("hello world!", "src", 0)
    assert id1 != id2


def test_chunk_id_different_offset():
    id1 = Chunk.make_id("hello", "src", 0)
    id2 = Chunk.make_id("hello", "src", 100)
    assert id1 != id2


# ── CharacterChunker ──────────────────────────────────────────────────────────

def test_character_chunker_basic():
    chunker = CharacterChunker(chunk_size=50, chunk_overlap=10)
    doc     = {"content": "A" * 200, "source": "test", "type": "config", "metadata": {}}
    chunks  = chunker.chunk(doc)
    assert len(chunks) > 1
    for c in chunks:
        assert len(c.text) <= 50


def test_character_chunker_overlap():
    chunker = CharacterChunker(chunk_size=100, chunk_overlap=20)
    long_text = "word " * 100
    doc       = {"content": long_text, "source": "t", "type": "config", "metadata": {}}
    chunks    = chunker.chunk(doc)
    # With overlap, consecutive chunks should share a suffix/prefix
    assert len(chunks) >= 2


def test_character_chunker_empty_doc():
    chunker = CharacterChunker()
    chunks  = chunker.chunk({"content": "", "source": "x", "type": "config", "metadata": {}})
    assert chunks == []


def test_character_chunker_invalid_overlap():
    with pytest.raises(ValueError):
        CharacterChunker(chunk_size=50, chunk_overlap=60)


# ── LineChunker ───────────────────────────────────────────────────────────────

def test_line_chunker_basic():
    chunker = LineChunker(lines_per_chunk=10, overlap_lines=2)
    chunks  = chunker.chunk(SAMPLE_BASH_DOC)
    assert len(chunks) > 1
    for c in chunks:
        assert "\n" in c.text or len(c.text) > 0


def test_line_chunker_empty():
    chunker = LineChunker()
    chunks  = chunker.chunk({"content": "", "source": "x", "type": "bash_history", "metadata": {}})
    assert chunks == []


# ── chunk_documents ───────────────────────────────────────────────────────────

def test_chunk_documents_mixed():
    docs   = [SAMPLE_BASH_DOC, SAMPLE_CONFIG_DOC]
    chunks = chunk_documents(docs)
    assert len(chunks) > 0
    # All chunks should have an id, text, source, doc_type
    for c in chunks:
        assert c.id
        assert c.text
        assert c.source
        assert c.doc_type


def test_chunk_documents_deduplication():
    """Duplicate documents should produce the same chunk IDs which get deduplicated."""
    docs   = [SAMPLE_CONFIG_DOC, SAMPLE_CONFIG_DOC]
    chunks = chunk_documents(docs)
    ids    = [c.id for c in chunks]
    assert len(ids) == len(set(ids)), "Duplicate chunk IDs found"


def test_chunk_to_endee_item():
    """to_endee_item should produce a dict matching Endee's upsert schema."""
    c    = Chunk(id="test_id", text="sample", source="/test", doc_type="config")
    vec  = [0.1] * 384
    item = c.to_endee_item(vec)
    assert item["id"]     == "test_id"
    assert item["vector"] == vec
    assert item["meta"]["text"]     == "sample"
    assert item["meta"]["source"]   == "/test"
    assert item["meta"]["doc_type"] == "config"
