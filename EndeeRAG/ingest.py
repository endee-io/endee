"""
Ingestion Module: PDF → Parse → Chunk → Embed → Store in Endee
Handles the complete document ingestion pipeline.
"""
import hashlib
import re
import sys
import time
import json
import logging
from pathlib import Path
from typing import List, Dict, Any, Optional, Tuple

import fitz  # PyMuPDF
import tiktoken
from sentence_transformers import SentenceTransformer
from endee_model import SparseModel
from tqdm import tqdm

from config import (
    ENDEE_INDEX_NAME,
    ENDEE_DENSE_DIM, ENDEE_SPACE_TYPE, ENDEE_SPARSE_MODEL,
    DENSE_MODEL_NAME, SPARSE_MODEL_NAME,
    CHUNK_SIZE, CHUNK_OVERLAP, MAX_CHUNKS_PER_DOC,
    UPLOAD_DIR, CHUNKS_DIR,
)
from encryption import encryptor
from endee_client import get_endee_client

# Configure logging (avoids charmap issues from print() on Windows)
logger = logging.getLogger("ingest")
if not logger.handlers:
    handler = logging.StreamHandler(
        open(sys.stdout.fileno(), mode="w", encoding="utf-8", closefd=False)
    )
    handler.setFormatter(logging.Formatter("%(message)s"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)


class DocumentIngestor:
    """End-to-end document ingestion pipeline."""

    # ─── Text Cleaning Utilities ──────────────────────────────────────

    @staticmethod
    def _clean_text(text: str) -> str:
        """Sanitize text to be safe for UTF-8 encoding, embedding, and console output.

        Fixes the 'charmap codec can't encode characters' error on Windows by
        round-tripping through UTF-8 and stripping control characters.
        """
        if not text:
            return ""
        # Round-trip through UTF-8 to drop anything that can't be represented
        text = text.encode("utf-8", errors="ignore").decode("utf-8")
        # Replace common problematic characters
        replacements = {
            "\u2018": "'", "\u2019": "'",   # Smart single quotes
            "\u201c": '"', "\u201d": '"',   # Smart double quotes
            "\u2013": "-", "\u2014": "--",  # En-dash, em-dash
            "\u2026": "...",                 # Ellipsis
            "\u00a0": " ",                   # Non-breaking space
            "\ufeff": "",                     # BOM
            "\u200b": "",                     # Zero-width space
        }
        for original, replacement in replacements.items():
            text = text.replace(original, replacement)
        # Strip remaining control characters (keep newlines/tabs)
        text = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]", "", text)
        return text.strip()

    @staticmethod
    def _is_valid_chunk(text: str, min_chars: int = 10) -> bool:
        """Return True if chunk has enough meaningful content to embed."""
        if not text or len(text.strip()) < min_chars:
            return False
        # Reject chunks that are only whitespace / punctuation
        if not re.search(r"[a-zA-Z0-9]", text):
            return False
        return True

    def __init__(self):
        logger.info("[Ingest] Initializing models...")
        self.dense_model = SentenceTransformer(DENSE_MODEL_NAME)
        self.sparse_model = SparseModel(model_name=SPARSE_MODEL_NAME)
        self.tokenizer = tiktoken.get_encoding("cl100k_base")

        # Initialize Endee client (Cloud / Local / Fallback)
        logger.info("[Ingest] Connecting to Endee...")
        self.client, self.using_local = get_endee_client()

        self._ensure_index()
        self.index = self.client.get_index(ENDEE_INDEX_NAME)
        logger.info("[Ingest] Ready.")

    def _ensure_index(self):
        """Create the hybrid index if it doesn't exist."""
        try:
            self.client.get_index(ENDEE_INDEX_NAME)
            logger.info(f"[Ingest] Index '{ENDEE_INDEX_NAME}' already exists.")
        except Exception:
            logger.info(f"[Ingest] Creating hybrid index '{ENDEE_INDEX_NAME}'...")
            kwargs = {
                "name": ENDEE_INDEX_NAME,
                "dimension": ENDEE_DENSE_DIM,
                "space_type": ENDEE_SPACE_TYPE,
            }
            if not self.using_local:
                # Only pass Endee-specific params for real Endee server
                from endee import Precision
                kwargs["sparse_model"] = ENDEE_SPARSE_MODEL
                kwargs["precision"] = Precision.INT8
            self.client.create_index(**kwargs)
            logger.info("[Ingest] Index created successfully.")

    # ─── PDF Parsing ─────────────────────────────────────────────────────

    def parse_pdf(self, pdf_path: str) -> Dict[str, Any]:
        """Extract text and metadata from a PDF file.

        Each page is extracted individually; failures on single pages are
        logged and skipped so the rest of the document is still ingested.
        """
        path = Path(pdf_path)
        doc = fitz.open(str(path))

        pages = []
        full_text = ""
        skipped_pages = []

        for page_num in range(len(doc)):
            try:
                page = doc[page_num]
                raw_text = page.get_text("text")
                # ── Critical fix: sanitize text to prevent charmap errors ──
                text = self._clean_text(raw_text)

                if not text:
                    skipped_pages.append(page_num + 1)
                    logger.warning(f"[Ingest] Page {page_num + 1}: empty after cleaning, skipped.")
                    continue

                pages.append({
                    "page_number": page_num + 1,
                    "text": text,
                    "char_count": len(text),
                })
                full_text += text + "\n\n"
            except Exception as e:
                skipped_pages.append(page_num + 1)
                logger.warning(f"[Ingest] Page {page_num + 1} failed: {e} — skipped.")
                continue

        if skipped_pages:
            logger.info(f"[Ingest] Skipped pages: {skipped_pages}")

        metadata = {
            "filename": path.name,
            "file_hash": hashlib.md5(path.read_bytes()).hexdigest(),
            "total_pages": len(doc),
            "parsed_pages": len(pages),
            "skipped_pages": skipped_pages,
            "total_chars": len(full_text),
        }

        # Try to get PDF metadata
        try:
            pdf_meta = doc.metadata
            if pdf_meta:
                metadata["title"] = self._clean_text(pdf_meta.get("title", "")) or path.stem
                metadata["author"] = self._clean_text(pdf_meta.get("author", "")) or "Unknown"
                metadata["subject"] = self._clean_text(pdf_meta.get("subject", ""))
        except Exception:
            metadata["title"] = path.stem
            metadata["author"] = "Unknown"
            metadata["subject"] = ""

        doc.close()
        return {"pages": pages, "full_text": full_text, "metadata": metadata}

    # ─── Chunking ────────────────────────────────────────────────────────

    def chunk_text(self, text: str, chunk_size: int = CHUNK_SIZE,
                   overlap: int = CHUNK_OVERLAP) -> List[Dict[str, Any]]:
        """Split text into overlapping token-based chunks.

        Chunks that are empty or contain only whitespace/punctuation
        after decoding are automatically dropped.
        """
        # Clean the text before tokenizing
        text = self._clean_text(text)
        if not text:
            return []

        tokens = self.tokenizer.encode(text)
        chunks = []
        start = 0

        while start < len(tokens) and len(chunks) < MAX_CHUNKS_PER_DOC:
            end = min(start + chunk_size, len(tokens))
            chunk_tokens = tokens[start:end]
            chunk_text = self.tokenizer.decode(chunk_tokens)
            # Clean decoded chunk and validate
            chunk_text = self._clean_text(chunk_text)

            if self._is_valid_chunk(chunk_text):
                chunks.append({
                    "text": chunk_text,
                    "token_count": len(chunk_tokens),
                    "start_token": start,
                    "end_token": end,
                    "chunk_index": len(chunks),
                })

            if end >= len(tokens):
                break
            start += chunk_size - overlap

        return chunks

    def chunk_by_pages(self, pages: List[Dict], chunk_size: int = CHUNK_SIZE,
                       overlap: int = CHUNK_OVERLAP) -> List[Dict[str, Any]]:
        """Chunk text while preserving page number information."""
        all_chunks = []
        current_text = ""
        current_pages = []

        for page in pages:
            current_text += page["text"] + "\n\n"
            current_pages.append(page["page_number"])

            tokens = self.tokenizer.encode(current_text)
            if len(tokens) >= chunk_size * 2:
                # Chunk the accumulated text
                chunks = self.chunk_text(current_text, chunk_size, overlap)
                for chunk in chunks:
                    chunk["source_pages"] = current_pages.copy()
                    all_chunks.append(chunk)
                current_text = ""
                current_pages = []

        # Handle remaining text
        if current_text.strip():
            chunks = self.chunk_text(current_text, chunk_size, overlap)
            for chunk in chunks:
                chunk["source_pages"] = current_pages.copy()
                all_chunks.append(chunk)

        # Re-index chunks
        for i, chunk in enumerate(all_chunks):
            chunk["chunk_index"] = i

        return all_chunks

    # ─── Embedding & Storage ─────────────────────────────────────────────

    def embed_and_store(self, chunks: List[Dict], doc_metadata: Dict,
                        batch_size: int = 100, encrypt: bool = True) -> Dict[str, Any]:
        """Embed chunks and store in Endee with both dense and sparse vectors."""
        stats = {
            "total_chunks": len(chunks),
            "stored": 0,
            "skipped": 0,
            "failed": 0,
            "latency_embed_ms": 0,
            "latency_upsert_ms": 0,
        }

        if not chunks:
            logger.warning("[Ingest] No chunks to embed — skipping storage.")
            return stats

        doc_id_prefix = doc_metadata.get("file_hash", "doc")[:8]

        for batch_start in tqdm(range(0, len(chunks), batch_size), desc="Embedding & storing"):
            batch = chunks[batch_start:batch_start + batch_size]

            # Filter to only valid texts (defensive — chunks should already be clean)
            valid_batch = []
            for c in batch:
                clean = self._clean_text(c["text"])
                if self._is_valid_chunk(clean):
                    c["text"] = clean  # ensure stored text is the cleaned version
                    valid_batch.append(c)
                else:
                    stats["skipped"] += 1

            if not valid_batch:
                continue

            texts = [c["text"] for c in valid_batch]

            try:
                # Dense embeddings
                t0 = time.time()
                dense_vecs = self.dense_model.encode(texts)
                stats["latency_embed_ms"] += (time.time() - t0) * 1000

                # Sparse embeddings
                sparse_vecs = list(self.sparse_model.embed(texts, batch_size=batch_size))
            except Exception as e:
                logger.error(f"[Ingest] Embedding batch failed: {e} — skipping {len(texts)} chunks.")
                stats["failed"] += len(texts)
                continue

            # Prepare upsert points
            points = []
            for i, (chunk, dense_vec, sparse_vec) in enumerate(zip(valid_batch, dense_vecs, sparse_vecs)):
                try:
                    if sparse_vec is None or not sparse_vec.indices.tolist():
                        stats["skipped"] += 1
                        continue

                    chunk_id = f"{doc_id_prefix}_chunk_{batch_start + i}"

                    # Build metadata
                    meta = {
                        "text": chunk["text"],
                        "chunk_index": chunk["chunk_index"],
                        "token_count": chunk["token_count"],
                        "filename": doc_metadata.get("filename", "unknown"),
                        "title": doc_metadata.get("title", "Untitled"),
                        "source_pages": str(chunk.get("source_pages", [])),
                    }

                    # Encrypt metadata if enabled
                    if encrypt and encryptor.enabled:
                        meta = encryptor.encrypt_metadata(meta)

                    # Build filter fields (for Endee filtering)
                    filter_fields = {
                        "doc_hash": doc_metadata.get("file_hash", "unknown"),
                        "filename": doc_metadata.get("filename", "unknown"),
                    }

                    points.append({
                        "id": chunk_id,
                        "vector": dense_vec.tolist(),
                        "sparse_indices": sparse_vec.indices.tolist(),
                        "sparse_values": sparse_vec.values.tolist(),
                        "meta": meta,
                        "filter": filter_fields,
                    })
                except Exception as e:
                    logger.warning(f"[Ingest] Chunk {batch_start + i} prep failed: {e}")
                    stats["failed"] += 1
                    continue

            # Upsert to Endee
            if points:
                try:
                    t0 = time.time()
                    # Endee allows max 1000 per upsert
                    for upsert_start in range(0, len(points), 1000):
                        upsert_batch = points[upsert_start:upsert_start + 1000]
                        self.index.upsert(upsert_batch)
                    stats["latency_upsert_ms"] += (time.time() - t0) * 1000
                    stats["stored"] += len(points)
                except Exception as e:
                    logger.error(f"[Ingest] Upsert failed: {e}")
                    stats["failed"] += len(points)

        return stats

    # ─── Full Pipeline ───────────────────────────────────────────────────

    def ingest_pdf(self, pdf_path: str, encrypt: bool = True,
                    original_filename: str = None) -> Dict[str, Any]:
        """Complete ingestion pipeline: PDF → chunks → embeddings → Endee."""
        logger.info(f"\n[Ingest] Processing: {original_filename or pdf_path}")
        total_start = time.time()

        # Step 1: Parse PDF
        try:
            t0 = time.time()
            parsed = self.parse_pdf(pdf_path)
            parse_time = (time.time() - t0) * 1000

            # Override filename with the real uploaded name if provided
            if original_filename:
                parsed["metadata"]["filename"] = original_filename
                if not parsed["metadata"].get("title") or parsed["metadata"]["title"] == Path(pdf_path).stem:
                    parsed["metadata"]["title"] = Path(original_filename).stem

            logger.info(
                f"  +-- Parsed: {parsed['metadata']['total_pages']} pages "
                f"({parsed['metadata'].get('parsed_pages', '?')} usable), "
                f"{parsed['metadata']['total_chars']} chars ({parse_time:.0f}ms)"
            )
        except Exception as e:
            raise RuntimeError(f"PDF parsing failed: {e}") from e

        if not parsed["pages"]:
            raise RuntimeError("No readable pages found in the PDF.")

        # Step 2: Chunk
        try:
            t0 = time.time()
            chunks = self.chunk_by_pages(parsed["pages"])
            chunk_time = (time.time() - t0) * 1000
            logger.info(f"  +-- Chunked: {len(chunks)} chunks ({chunk_time:.0f}ms)")
        except Exception as e:
            raise RuntimeError(f"Chunking failed: {e}") from e

        if not chunks:
            raise RuntimeError("No valid chunks produced from the PDF.")

        # Step 3: Embed & Store
        try:
            storage_stats = self.embed_and_store(chunks, parsed["metadata"], encrypt=encrypt)
            logger.info(f"  +-- Stored: {storage_stats['stored']}/{storage_stats['total_chunks']} chunks")
            logger.info(f"  +-- Embed time: {storage_stats['latency_embed_ms']:.0f}ms")
            logger.info(f"  +-- Upsert time: {storage_stats['latency_upsert_ms']:.0f}ms")
        except Exception as e:
            raise RuntimeError(f"Embedding/storage failed: {e}") from e

        total_time = (time.time() - total_start) * 1000
        logger.info(f"  \\-- Total: {total_time:.0f}ms")

        # Save chunk data for reference
        try:
            chunks_file = CHUNKS_DIR / f"{parsed['metadata']['file_hash'][:8]}_chunks.json"
            with open(chunks_file, "w", encoding="utf-8") as f:
                json.dump({
                    "metadata": parsed["metadata"],
                    "chunks": [{"index": c["chunk_index"], "token_count": c["token_count"],
                                "pages": c.get("source_pages", [])} for c in chunks],
                }, f, indent=2, ensure_ascii=False)
        except Exception as e:
            logger.warning(f"[Ingest] Could not save chunk metadata: {e}")

        return {
            "metadata": parsed["metadata"],
            "chunks_count": len(chunks),
            "stored_count": storage_stats["stored"],
            "parse_time_ms": parse_time,
            "chunk_time_ms": chunk_time,
            "embed_time_ms": storage_stats["latency_embed_ms"],
            "upsert_time_ms": storage_stats["latency_upsert_ms"],
            "total_time_ms": total_time,
            "encrypted": encrypt and encryptor.enabled,
        }

    def ingest_text(self, text: str, title: str = "Pasted Text",
                    source: str = "user_input", encrypt: bool = True) -> Dict[str, Any]:
        """Ingest raw text (for non-PDF inputs)."""
        file_hash = hashlib.md5(text.encode()).hexdigest()
        metadata = {
            "filename": source,
            "file_hash": file_hash,
            "total_pages": 1,
            "total_chars": len(text),
            "title": title,
        }

        chunks = self.chunk_text(text)
        for chunk in chunks:
            chunk["source_pages"] = [1]

        storage_stats = self.embed_and_store(chunks, metadata, encrypt=encrypt)

        return {
            "metadata": metadata,
            "chunks_count": len(chunks),
            "stored_count": storage_stats["stored"],
            "encrypted": encrypt and encryptor.enabled,
        }

    def delete_document(self, doc_hash: str):
        """Delete all chunks of a document from Endee."""
        # Endee Python SDK doesn't support filter-based deletion
        # We'd need to track chunk IDs and delete individually
        print(f"[Ingest] Document deletion for hash {doc_hash} - tracking chunk IDs required.")


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print("Usage: python ingest.py <pdf_path>")
        sys.exit(1)

    ingestor = DocumentIngestor()
    try:
        result = ingestor.ingest_pdf(sys.argv[1])
        logger.info(f"\nIngestion complete: {json.dumps(result, indent=2, ensure_ascii=False)}")
    except Exception as e:
        logger.error(f"\nIngestion failed: {e}")
        sys.exit(1)
