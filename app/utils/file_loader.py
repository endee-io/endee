import io
from typing import List

from PyPDF2 import PdfReader


def load_text_from_file(filename: str, file_bytes: bytes) -> str:
    if filename.lower().endswith(".pdf"):
        with io.BytesIO(file_bytes) as stream:
            reader = PdfReader(stream)
            pages = []
            for page in reader.pages:
                text = page.extract_text()
                if text:
                    pages.append(text)
            return "\n\n".join(pages)

    return file_bytes.decode("utf-8", errors="replace")


def chunk_text(text: str, chunk_size: int = 500, overlap: int = 125) -> List[str]:
    words = text.split()
    if not words:
        return []

    chunks = []
    start = 0
    while start < len(words):
        end = min(start + chunk_size, len(words))
        chunk = " ".join(words[start:end]).strip()
        if chunk:
            chunks.append(chunk)
        if end == len(words):
            break
        start += chunk_size - overlap
    return chunks
