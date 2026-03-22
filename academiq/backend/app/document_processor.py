import fitz  # PyMuPDF
import hashlib
import re


class DocumentProcessor:
    """
    Extracts text from PDFs or raw strings and splits into
    overlapping sentence-aware chunks for Endee ingestion.
    """

    def __init__(self, chunk_size: int = 450, overlap: int = 60):
        self.chunk_size = chunk_size
        self.overlap = overlap

    def extract_from_pdf(self, file_bytes: bytes) -> str:
        """Extract all text from a PDF byte stream using PyMuPDF."""
        doc = fitz.open(stream=file_bytes, filetype="pdf")
        pages = [page.get_text("text") for page in doc]
        doc.close()
        full_text = "\n\n".join(pages)
        return self._clean_text(full_text)

    def _clean_text(self, text: str) -> str:
        """Remove excess whitespace and non-printable characters."""
        text = re.sub(r'\s+', ' ', text)
        text = re.sub(r'[^\x20-\x7E\n]', '', text)
        return text.strip()

    def chunk_text(self, text: str, doc_name: str) -> list[dict]:
        """
        Split text into overlapping chunks using sentence boundaries.
        Returns list of chunk dicts ready for embedding + Endee insertion.
        """
        # Split on sentence boundaries
        sentences = re.split(r'(?<=[.!?])\s+', text)

        chunks = []
        current = ""
        chunk_idx = 0

        for sentence in sentences:
            if len(current) + len(sentence) <= self.chunk_size:
                current += " " + sentence if current else sentence
            else:
                if current.strip():
                    chunk_id = self._make_id(doc_name, chunk_idx)
                    chunks.append({
                        "id": chunk_id,
                        "text": current.strip(),
                        "doc_name": doc_name,
                        "chunk_index": chunk_idx
                    })
                    chunk_idx += 1
                # Start new chunk with overlap from previous
                words = current.split()
                overlap_text = " ".join(words[-self.overlap:]) if len(words) > self.overlap else current
                current = overlap_text + " " + sentence

        # Flush the last chunk
        if current.strip():
            chunks.append({
                "id": self._make_id(doc_name, chunk_idx),
                "text": current.strip(),
                "doc_name": doc_name,
                "chunk_index": chunk_idx
            })

        return chunks

    def _make_id(self, doc_name: str, idx: int) -> str:
        """Create a deterministic, Endee-safe vector ID."""
        base = f"{doc_name}_{idx}"
        return hashlib.md5(base.encode()).hexdigest()[:16] + f"_{idx}"

    def process_pdf(self, file_bytes: bytes, doc_name: str) -> list[dict]:
        """Full pipeline: PDF bytes → cleaned text → chunks."""
        text = self.extract_from_pdf(file_bytes)
        return self.chunk_text(text, doc_name)

    def process_text(self, text: str, doc_name: str) -> list[dict]:
        """Full pipeline: raw text string → chunks."""
        cleaned = self._clean_text(text)
        return self.chunk_text(cleaned, doc_name)
