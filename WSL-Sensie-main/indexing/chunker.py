"""
indexing/chunker.py
────────────────────
Splits raw Document text into smaller, overlapping chunks suitable for
embedding. Chunks preserve their parent document's metadata so retrieved
results can always be traced back to their source.

Two strategies are provided:
  - CharacterChunker  – simple character-window split (fast, default)
  - LineChunker       – groups lines into blocks (better for shell history)
"""

from __future__ import annotations

import hashlib
import uuid
from dataclasses import dataclass, field
from typing import Any

from config import CHUNK_SIZE, CHUNK_OVERLAP

Document = dict[str, Any]


# ── Chunk dataclass ────────────────────────────────────────────────────────────

@dataclass
class Chunk:
    """
    A single indexable unit of text.

    Attributes
    ----------
    id       : Stable content-hash ID used as the Endee vector ID.
    text     : The raw text content of this chunk.
    source   : Origin file/system (e.g. ~/.bashrc, bash_history).
    doc_type : Coarse type label (bash_history, config, script, log …).
    metadata : Arbitrary key/value pairs forwarded to Endee's ``meta`` field.
    """

    id:       str
    text:     str
    source:   str
    doc_type: str
    metadata: dict[str, Any] = field(default_factory=dict)

    # ── Helpers ─────────────────────────────────────────────────────────────

    @staticmethod
    def make_id(text: str, source: str, offset: int) -> str:
        """
        Derive a stable, deterministic ID from the chunk's content and position.
        Using a content-hash means re-indexing the same text will produce the
        same ID, enabling idempotent upserts.
        """
        digest = hashlib.sha256(f"{source}:{offset}:{text}".encode()).hexdigest()[:16]
        return f"chunk_{digest}"

    def to_endee_item(self, vector: list[float]) -> dict[str, Any]:
        """
        Produce the dict expected by ``index.upsert()``.

        Schema
        ------
        {
          "id":     str,
          "vector": list[float],
          "meta":   {
            "text":     str,
            "source":   str,
            "doc_type": str,
            **metadata
          }
        }
        """
        return {
            "id":     self.id,
            "vector": vector,
            "meta": {
                "text":     self.text,
                "source":   self.source,
                "doc_type": self.doc_type,
                **self.metadata,
            },
        }


# ── Chunkers ───────────────────────────────────────────────────────────────────

class CharacterChunker:
    """
    Splits text into overlapping character windows.
    Works well for continuous prose, config files, and logs.
    """

    def __init__(
        self,
        chunk_size: int    = CHUNK_SIZE,
        chunk_overlap: int = CHUNK_OVERLAP,
    ) -> None:
        if chunk_overlap >= chunk_size:
            raise ValueError("chunk_overlap must be less than chunk_size")
        self.chunk_size    = chunk_size
        self.chunk_overlap = chunk_overlap

    def chunk(self, doc: Document) -> list[Chunk]:
        text     = doc.get("content", "").strip()
        source   = doc.get("source", "unknown")
        doc_type = doc.get("type",   "unknown")
        meta     = doc.get("metadata", {})

        if not text:
            return []

        chunks: list[Chunk] = []
        step   = self.chunk_size - self.chunk_overlap
        offset = 0

        while offset < len(text):
            end      = offset + self.chunk_size
            fragment = text[offset:end].strip()

            if fragment:
                chunk_id = Chunk.make_id(fragment, source, offset)
                chunks.append(
                    Chunk(
                        id=chunk_id,
                        text=fragment,
                        source=source,
                        doc_type=doc_type,
                        metadata={
                            **meta,
                            "chunk_offset": offset,
                            "chunk_length": len(fragment),
                        },
                    )
                )

            offset += step

        return chunks


class LineChunker:
    """
    Groups lines into fixed-size blocks with optional overlap.
    Well-suited for shell history where each line is one command.
    """

    def __init__(
        self,
        lines_per_chunk: int = 20,
        overlap_lines:   int = 3,
    ) -> None:
        self.lines_per_chunk = lines_per_chunk
        self.overlap_lines   = overlap_lines

    def chunk(self, doc: Document) -> list[Chunk]:
        text     = doc.get("content", "").strip()
        source   = doc.get("source", "unknown")
        doc_type = doc.get("type",   "unknown")
        meta     = doc.get("metadata", {})

        if not text:
            return []

        lines  = [l for l in text.splitlines() if l.strip()]
        chunks: list[Chunk] = []
        step   = max(1, self.lines_per_chunk - self.overlap_lines)

        for i in range(0, len(lines), step):
            block    = "\n".join(lines[i : i + self.lines_per_chunk])
            chunk_id = Chunk.make_id(block, source, i)
            chunks.append(
                Chunk(
                    id=chunk_id,
                    text=block,
                    source=source,
                    doc_type=doc_type,
                    metadata={**meta, "line_start": i},
                )
            )

        return chunks


# ── Convenience function ───────────────────────────────────────────────────────

def chunk_documents(
    docs: list[Document],
    strategy: str = "character",
    chunk_size: int    = CHUNK_SIZE,
    chunk_overlap: int = CHUNK_OVERLAP,
) -> list[Chunk]:
    """
    Chunk a list of Documents using the chosen strategy.

    Parameters
    ----------
    docs:
        Raw documents from any collector.
    strategy:
        ``"character"`` (default) or ``"line"``.
    chunk_size / chunk_overlap:
        Passed to CharacterChunker; ignored for LineChunker.

    Returns
    -------
    list[Chunk]
    """
    if strategy == "line":
        chunker = LineChunker()
    else:
        chunker = CharacterChunker(chunk_size=chunk_size, chunk_overlap=chunk_overlap)

    all_chunks: list[Chunk] = []
    for doc in docs:
        # Shell-history docs benefit from the line chunker
        if doc.get("type") in {"bash_history", "zsh_history", "powershell_history"}:
            all_chunks.extend(LineChunker().chunk(doc))
        else:
            all_chunks.extend(chunker.chunk(doc))

    # Deduplicate by chunk ID
    seen: set[str]       = set()
    unique: list[Chunk]  = []
    for c in all_chunks:
        if c.id not in seen:
            seen.add(c.id)
            unique.append(c)

    print(f"[chunker] ✓  {len(docs)} documents → {len(unique)} unique chunks")
    return unique
