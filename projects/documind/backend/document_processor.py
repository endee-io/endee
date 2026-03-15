"""
document_processor.py
----------------------
Handles reading and chunking of uploaded documents (TXT, MD, PDF).
Returns a normalised structure consumed by RAGEngine.add_document().
"""

from __future__ import annotations

import hashlib
import io
import logging
from typing import Dict, List

logger = logging.getLogger(__name__)


# ──────────────────────────────────────────────────────────────────────────── #
#  Chunking                                                                    #
# ──────────────────────────────────────────────────────────────────────────── #

def chunk_text(
    text: str,
    chunk_size: int = 300,   # words per chunk
    overlap: int = 50,        # overlapping words between adjacent chunks
) -> List[Dict]:
    """
    Split *text* into overlapping word-based chunks.

    Returns a list of dicts:
        {"text": str, "start_word": int, "end_word": int}
    """
    words = text.split()
    chunks: List[Dict] = []
    step = max(1, chunk_size - overlap)

    for start in range(0, len(words), step):
        end = start + chunk_size
        chunk_words = words[start:end]
        if not chunk_words:
            break
        chunks.append(
            {
                "text": " ".join(chunk_words),
                "start_word": start,
                "end_word": start + len(chunk_words),
            }
        )
        if end >= len(words):
            break

    return chunks


# ──────────────────────────────────────────────────────────────────────────── #
#  File processors                                                             #
# ──────────────────────────────────────────────────────────────────────────── #

def _make_doc_id(filename: str, content_prefix: str) -> str:
    """Deterministic, short document ID based on filename + first bytes."""
    raw = f"{filename}::{content_prefix}"
    return hashlib.md5(raw.encode()).hexdigest()[:16]


def process_text_file(content: str, filename: str) -> Dict:
    """Process a plain-text / Markdown file."""
    doc_id = _make_doc_id(filename, content[:200])
    chunks = chunk_text(content)
    logger.info("Processed text file '%s' → %d chunks", filename, len(chunks))
    return {
        "doc_id": doc_id,
        "filename": filename,
        "chunks": chunks,
        "total_chunks": len(chunks),
    }


def process_pdf_file(file_bytes: bytes, filename: str) -> Dict:
    """Extract text from a PDF and process it like a text file."""
    try:
        import pypdf  # lazy import so PDF support is optional

        reader = pypdf.PdfReader(io.BytesIO(file_bytes))
        pages_text: List[str] = []
        for page in reader.pages:
            extracted = page.extract_text()
            if extracted:
                pages_text.append(extracted)

        full_text = "\n\n".join(pages_text)
        if not full_text.strip():
            raise ValueError("No readable text found in PDF.")

        return process_text_file(full_text, filename)

    except ImportError as exc:
        raise ImportError(
            "pypdf is required for PDF support: pip install pypdf"
        ) from exc
    except Exception as exc:
        raise ValueError(f"Failed to process PDF '{filename}': {exc}") from exc


# ──────────────────────────────────────────────────────────────────────────── #
#  Dispatcher                                                                  #
# ──────────────────────────────────────────────────────────────────────────── #

def process_file(file_bytes: bytes, filename: str) -> Dict:
    """Auto-detect format and process the file."""
    lower = filename.lower()
    if lower.endswith(".pdf"):
        return process_pdf_file(file_bytes, filename)
    if lower.endswith((".txt", ".md", ".rst", ".csv")):
        text = file_bytes.decode("utf-8", errors="replace")
        return process_text_file(text, filename)
    raise ValueError(
        f"Unsupported file type: '{filename}'. "
        "Supported formats: PDF, TXT, MD, RST, CSV."
    )
