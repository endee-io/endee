from __future__ import annotations

import uuid
from io import BytesIO

from pypdf import PdfReader

from app.database import insert_chunks, insert_document
from app.embeddings import embed_texts
from app.endee_client import get_hr_index


def _chunk_page_text(text: str, page: int, size: int = 900, overlap: int = 150) -> list[tuple[int, int, str]]:
    text = " ".join(text.split())
    if not text:
        return []
    chunks: list[tuple[int, int, str]] = []
    start = 0
    local_idx = 0
    n = len(text)
    while start < n:
        end = min(start + size, n)
        piece = text[start:end].strip()
        if piece:
            chunks.append((page, local_idx, piece))
            local_idx += 1
        if end >= n:
            break
        start = max(0, end - overlap)
    return chunks


def extract_pages(pdf_bytes: bytes) -> list[tuple[int, str]]:
    reader = PdfReader(BytesIO(pdf_bytes))
    out: list[tuple[int, str]] = []
    for i, page in enumerate(reader.pages):
        t = page.extract_text() or ""
        out.append((i + 1, t))
    return out


async def ingest_pdf(
    pdf_bytes: bytes,
    filename: str,
    dept_code: str,
    doc_type: str,
) -> dict:
    """
    Store plaintext chunks in SQLite (application vault). Store only vectors + opaque
    ids/codes in Endee so a vector-DB breach does not expose resume/review text.
    """
    doc_id = str(uuid.uuid4())
    await insert_document(doc_id, filename, dept_code, doc_type)

    all_chunks: list[tuple[int, int, str]] = []
    for page_num, page_text in extract_pages(pdf_bytes):
        all_chunks.extend(_chunk_page_text(page_text, page_num))

    if not all_chunks:
        return {"document_id": doc_id, "chunks_indexed": 0, "message": "No extractable text"}

    chunk_rows: list[tuple[str, str, int, int, str]] = []
    endee_batch: list[dict] = []
    texts_for_embed: list[str] = []

    for page, _local_i, piece in all_chunks:
        cid = str(uuid.uuid4())
        chunk_rows.append((cid, doc_id, len(chunk_rows), page, piece))
        texts_for_embed.append(piece)

    vectors = embed_texts(texts_for_embed)
    index = get_hr_index()

    for i, (cid, _doc_id, chunk_index, page, piece) in enumerate(chunk_rows):
        endee_batch.append(
            {
                "id": cid,
                "vector": vectors[i],
                "meta": {
                    "chunk_id": cid,
                    "document_id": doc_id,
                    "chunk_index": chunk_index,
                    "page": page,
                },
                "filter": {"dept_code": dept_code, "doc_type": doc_type},
            }
        )

    batch_size = 256
    for b in range(0, len(endee_batch), batch_size):
        index.upsert(endee_batch[b : b + batch_size])

    await insert_chunks(chunk_rows)
    return {"document_id": doc_id, "chunks_indexed": len(chunk_rows), "filename": filename}
