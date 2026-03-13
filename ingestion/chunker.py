"""
Document chunker — splits documents into overlapping text chunks.

Each chunk is a dict:
  { "text": str, "chunk_id": str, "metadata": dict }
"""

import hashlib
import logging
from typing import List, Dict

logger = logging.getLogger(__name__)


def _split_text(text: str, chunk_size: int, overlap: int) -> List[str]:
    """
    Split text into chunks of approximately `chunk_size` characters
    with `overlap` characters shared between consecutive chunks.
    Splits on sentence boundaries when possible.
    """
    if len(text) <= chunk_size:
        return [text]

    chunks: List[str] = []
    start = 0

    while start < len(text):
        end = start + chunk_size

        # If we haven't reached the end, try to break at a sentence boundary
        if end < len(text):
            # Look for the last period, question mark, or newline before the end
            for sep in [". ", "? ", "! ", "\n\n", "\n"]:
                last_sep = text[start:end].rfind(sep)
                if last_sep != -1 and last_sep > chunk_size * 0.3:
                    end = start + last_sep + len(sep)
                    break

        chunk = text[start:end].strip()
        if chunk:
            chunks.append(chunk)

        # Move start forward, accounting for overlap
        start = end - overlap if end < len(text) else len(text)

    return chunks


def _generate_chunk_id(source: str, index: int) -> str:
    """Create a deterministic ID for a chunk based on source and position."""
    raw = f"{source}::chunk_{index}"
    return hashlib.md5(raw.encode()).hexdigest()


def chunk_documents(
    documents: List[Dict],
    chunk_size: int = 500,
    overlap: int = 50,
) -> List[Dict]:
    """
    Split loaded documents into overlapping chunks.

    Args:
        documents:  Output of load_documents().
        chunk_size: Target number of characters per chunk.
        overlap:    Overlap between consecutive chunks.

    Returns:
        List of chunk dicts with 'text', 'chunk_id', and 'metadata'.
    """
    all_chunks: List[Dict] = []

    for doc in documents:
        text = doc["content"]
        metadata = doc.get("metadata", {})
        source = metadata.get("source", "unknown")

        pieces = _split_text(text, chunk_size, overlap)

        for i, piece in enumerate(pieces):
            chunk_id = _generate_chunk_id(source, i)
            all_chunks.append({
                "text": piece,
                "chunk_id": chunk_id,
                "metadata": {
                    **metadata,
                    "chunk_index": i,
                    "total_chunks": len(pieces),
                },
            })

        logger.info(f"Chunked '{source}' → {len(pieces)} chunk(s)")

    logger.info(f"Total chunks created: {len(all_chunks)}")
    return all_chunks
