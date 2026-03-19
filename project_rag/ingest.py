"""
Standalone batch document ingestor — CLI tool.
Supports .txt and .pdf files.
Usage: python ingest.py --dir ./data --chunk-size 500
"""

import argparse
import io
import json
import os
import time

import numpy as np
import requests
from sentence_transformers import SentenceTransformer

ENDEE_URL     = os.getenv("ENDEE_URL",   "http://localhost:8080")
INDEX_NAME    = os.getenv("INDEX_NAME",  "LEGALDOC")
EMBEDDING_DIM = 384

def create_index():
    url = f"{ENDEE_URL}/api/v1/index/create"
    payload = {
        "index_name": INDEX_NAME, "dim": EMBEDDING_DIM,
        "space_type": "cosine", "M": 16, "ef_con": 200, "precision": "float32",
    }
    r = requests.post(url, json=payload, timeout=10)
    if r.ok:
        print(f"✓ Index '{INDEX_NAME}' ready")
    elif "already exists" in r.text.lower():
        print(f"✓ Index '{INDEX_NAME}' already exists")
    else:
        print(f"✗ Index creation failed: {r.text}")

def read_file(path: str) -> str:
    if path.endswith(".pdf"):
        try:
            from pypdf import PdfReader
            reader = PdfReader(path)
            return "\n".join(p.extract_text() or "" for p in reader.pages)
        except ImportError:
            print("  ✗ pypdf not installed. Run: pip install pypdf"); return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()

def chunk_text(text: str, chunk_size: int = 500):
    words = text.split()
    return [" ".join(words[i:i+chunk_size]) for i in range(0, len(words), chunk_size) if words[i:i+chunk_size]]

def ingest_file(path: str, model, chunk_size: int = 500):
    print(f"\n📄 {os.path.basename(path)}")
    text = read_file(path)
    if not text.strip():
        print("  ✗ Empty or unreadable"); return 0

    chunks = chunk_text(text, chunk_size)
    print(f"  → {len(chunks)} chunks")

    embeddings = model.encode(chunks, normalize_embeddings=True, show_progress_bar=True)
    doc_name   = os.path.basename(path)
    seed       = f"{doc_name}-{int(time.time())}"

    vectors = [
        {
            "id":     f"{seed}-{i}",
            "vector": emb.astype(np.float32).tolist(),
            "meta":   json.dumps({"doc": doc_name, "text": ch}),
        }
        for i, (ch, emb) in enumerate(zip(chunks, embeddings))
    ]

    r = requests.post(
        f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/vector/insert",
        json=vectors, timeout=60,
        headers={"Content-Type": "application/json"},
    )
    if r.ok:
        print(f"  ✓ Stored {len(vectors)} vectors")
        return len(vectors)
    print(f"  ✗ Insert failed: {r.status_code} – {r.text[:200]}")
    return 0

def main():
    parser = argparse.ArgumentParser(description="Batch ingestor for Legal RAG")
    parser.add_argument("--dir",        default="./data",  help="Directory with .txt/.pdf files")
    parser.add_argument("--file",                          help="Single file to ingest")
    parser.add_argument("--chunk-size", type=int, default=500)
    parser.add_argument("--no-create",  action="store_true", help="Skip index creation")
    args = parser.parse_args()

    model = SentenceTransformer("all-MiniLM-L6-v2")

    if not args.no_create:
        create_index()

    total = 0
    if args.file:
        total += ingest_file(args.file, model, args.chunk_size)
    elif os.path.isdir(args.dir):
        files = [f for f in os.listdir(args.dir) if f.endswith((".txt", ".pdf"))]
        print(f"\nFound {len(files)} files in {args.dir}")
        for fname in files:
            total += ingest_file(os.path.join(args.dir, fname), model, args.chunk_size)
    else:
        print(f"✗ Directory not found: {args.dir}")

    print(f"\n✅ Total vectors stored: {total}")

if __name__ == "__main__":
    main()