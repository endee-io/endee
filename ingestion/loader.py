"""
Document loader — reads .txt, .pdf, and .json files into a standard format.

Each loaded document is a dict:
  { "content": str, "metadata": { "source": str, "filename": str } }
"""

import os
import json
import logging
from typing import List, Dict
from pathlib import Path

logger = logging.getLogger(__name__)


def _load_txt(filepath: str) -> str:
    """Read plain-text file."""
    with open(filepath, "r", encoding="utf-8") as f:
        return f.read()


def _load_pdf(filepath: str) -> str:
    """Extract text from a PDF using PyPDF2."""
    try:
        from PyPDF2 import PdfReader
    except ImportError:
        raise ImportError("PyPDF2 is required for PDF loading. Run: pip install PyPDF2")

    reader = PdfReader(filepath)
    pages = [page.extract_text() or "" for page in reader.pages]
    return "\n".join(pages)


def _load_json(filepath: str) -> str:
    """
    Load JSON file.  Supports two formats:
      1. A list of objects with a "text" or "content" field
      2. A single object with a "text" or "content" field
    Falls back to dumping the whole JSON as string.
    """
    with open(filepath, "r", encoding="utf-8") as f:
        data = json.load(f)

    # List of records
    if isinstance(data, list):
        texts = []
        for item in data:
            if isinstance(item, dict):
                texts.append(item.get("text", item.get("content", json.dumps(item))))
            else:
                texts.append(str(item))
        return "\n\n".join(texts)

    # Single object
    if isinstance(data, dict):
        return data.get("text", data.get("content", json.dumps(data, indent=2)))

    return str(data)


# Mapping of extensions to loader functions
_LOADERS = {
    ".txt": _load_txt,
    ".md": _load_txt,
    ".pdf": _load_pdf,
    ".json": _load_json,
}


def load_documents(path: str) -> List[Dict]:
    """
    Load documents from a file or directory.

    Args:
        path: Path to a single file or a directory containing documents.

    Returns:
        List of dicts with 'content' and 'metadata' keys.
    """
    path = Path(path)
    documents: List[Dict] = []

    if path.is_file():
        files = [path]
    elif path.is_dir():
        files = sorted(path.iterdir())
    else:
        raise FileNotFoundError(f"Path not found: {path}")

    for filepath in files:
        ext = filepath.suffix.lower()
        loader = _LOADERS.get(ext)

        if loader is None:
            logger.warning(f"Skipping unsupported file type: {filepath.name}")
            continue

        try:
            content = loader(str(filepath))
            if content.strip():
                documents.append({
                    "content": content,
                    "metadata": {
                        "source": str(filepath),
                        "filename": filepath.name,
                    },
                })
                logger.info(f"Loaded {filepath.name} ({len(content)} chars)")
            else:
                logger.warning(f"Empty content in {filepath.name}, skipping")
        except Exception as e:
            logger.error(f"Failed to load {filepath.name}: {e}")

    logger.info(f"Loaded {len(documents)} document(s) from {path}")
    return documents
