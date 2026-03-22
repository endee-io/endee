from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path

import aiosqlite

from app.config import get_settings


SCHEMA = """
CREATE TABLE IF NOT EXISTS documents (
    id TEXT PRIMARY KEY,
    filename TEXT NOT NULL,
    created_at TEXT NOT NULL,
    dept_code TEXT NOT NULL DEFAULT 'unassigned',
    doc_type TEXT NOT NULL DEFAULT 'general'
);

CREATE TABLE IF NOT EXISTS chunks (
    id TEXT PRIMARY KEY,
    document_id TEXT NOT NULL,
    chunk_index INTEGER NOT NULL,
    page_start INTEGER NOT NULL DEFAULT 1,
    text TEXT NOT NULL,
    FOREIGN KEY (document_id) REFERENCES documents(id)
);

CREATE INDEX IF NOT EXISTS idx_chunks_document ON chunks(document_id);
"""


async def init_db() -> None:
    path = Path(get_settings().database_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    async with aiosqlite.connect(path) as db:
        await db.executescript(SCHEMA)
        await db.commit()


async def insert_document(
    doc_id: str,
    filename: str,
    dept_code: str,
    doc_type: str,
) -> None:
    now = datetime.now(timezone.utc).isoformat()
    async with aiosqlite.connect(get_settings().database_path) as db:
        await db.execute(
            "INSERT INTO documents (id, filename, created_at, dept_code, doc_type) VALUES (?, ?, ?, ?, ?)",
            (doc_id, filename, now, dept_code, doc_type),
        )
        await db.commit()


async def insert_chunks(rows: list[tuple[str, str, int, int, str]]) -> None:
    """rows: (chunk_id, document_id, chunk_index, page_start, text)"""
    async with aiosqlite.connect(get_settings().database_path) as db:
        await db.executemany(
            "INSERT INTO chunks (id, document_id, chunk_index, page_start, text) VALUES (?, ?, ?, ?, ?)",
            rows,
        )
        await db.commit()


async def get_chunks_by_ids(chunk_ids: list[str]) -> dict[str, dict]:
    if not chunk_ids:
        return {}
    placeholders = ",".join("?" * len(chunk_ids))
    async with aiosqlite.connect(get_settings().database_path) as db:
        db.row_factory = aiosqlite.Row
        cur = await db.execute(
            f"SELECT c.id, c.document_id, c.chunk_index, c.page_start, c.text, d.filename, d.dept_code, d.doc_type "
            f"FROM chunks c JOIN documents d ON c.document_id = d.id "
            f"WHERE c.id IN ({placeholders})",
            chunk_ids,
        )
        rows = await cur.fetchall()
    out: dict[str, dict] = {}
    for r in rows:
        out[r["id"]] = {
            "chunk_id": r["id"],
            "document_id": r["document_id"],
            "chunk_index": r["chunk_index"],
            "page": r["page_start"],
            "text": r["text"],
            "filename": r["filename"],
            "dept_code": r["dept_code"],
            "doc_type": r["doc_type"],
        }
    return out


async def get_chunk_text(chunk_id: str) -> str | None:
    async with aiosqlite.connect(get_settings().database_path) as db:
        cur = await db.execute("SELECT text FROM chunks WHERE id = ?", (chunk_id,))
        row = await cur.fetchone()
    return row[0] if row else None


async def list_documents() -> list[dict]:
    async with aiosqlite.connect(get_settings().database_path) as db:
        db.row_factory = aiosqlite.Row
        cur = await db.execute(
            "SELECT id, filename, created_at, dept_code, doc_type FROM documents ORDER BY created_at DESC"
        )
        rows = await cur.fetchall()
    return [dict(r) for r in rows]
