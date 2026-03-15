"""
rag_engine.py
-------------
Core RAG logic:
  1. Stores document chunk embeddings in Endee (vector DB).
  2. Retrieves the most relevant chunks for a user query.
  3. Generates an answer via OpenAI, Ollama, or a retrieval-only fallback.

Endee is used as the sole vector store — no secondary database required.
"""

from __future__ import annotations

import json
import logging
import os
from typing import Any, Dict, List, Optional

from endee import Endee, Precision

from embedder import EmbeddingService, VECTOR_DIM

logger = logging.getLogger(__name__)

INDEX_NAME    = "documind_knowledge_base"
META_FILE     = "documents_meta.json"   # lightweight JSON sidecar for doc metadata
UPSERT_BATCH  = 100                     # vectors per Endee upsert call


class RAGEngine:
    """
    Orchestrates embedding, Endee vector storage, retrieval, and generation.

    Endee stores every document chunk as a vector with:
      • id      : "<doc_id>_chunk_<n>"
      • vector  : 384-dim cosine embedding (all-MiniLM-L6-v2)
      • meta    : {"text", "filename", "doc_id", "chunk_index"}
      • filter  : {"doc_id": "<doc_id>"}  ← enables per-document search
    """

    # ------------------------------------------------------------------ #
    #  Initialisation                                                      #
    # ------------------------------------------------------------------ #

    def __init__(self) -> None:
        auth_token = os.getenv("ENDEE_AUTH_TOKEN", "")
        base_url   = os.getenv("ENDEE_BASE_URL", "")

        self.client = Endee(auth_token) if auth_token else Endee()
        if base_url:
            self.client.set_base_url(base_url)

        self.embedder       = EmbeddingService()
        self.documents_meta = self._load_meta()
        self._ensure_index()

    # ------------------------------------------------------------------ #
    #  Index management                                                    #
    # ------------------------------------------------------------------ #

    def _ensure_index(self) -> None:
        """Create the Endee index if it does not exist yet."""
        try:
            existing = {
                (idx["name"] if isinstance(idx, dict) else str(idx))
                for idx in self.client.list_indexes()
            }
            if INDEX_NAME not in existing:
                self.client.create_index(
                    name=INDEX_NAME,
                    dimension=VECTOR_DIM,
                    space_type="cosine",
                    precision=Precision.INT8,
                )
                logger.info("Created Endee index: %s (dim=%d)", INDEX_NAME, VECTOR_DIM)
            else:
                logger.info("Using existing Endee index: %s", INDEX_NAME)
        except Exception as exc:
            logger.warning("Index setup warning (server may not be ready): %s", exc)

    # ------------------------------------------------------------------ #
    #  Document metadata sidecar                                           #
    # ------------------------------------------------------------------ #

    def _load_meta(self) -> Dict[str, Any]:
        if os.path.exists(META_FILE):
            with open(META_FILE, "r") as fh:
                return json.load(fh)
        return {}

    def _save_meta(self) -> None:
        with open(META_FILE, "w") as fh:
            json.dump(self.documents_meta, fh, indent=2)

    # ------------------------------------------------------------------ #
    #  Ingest                                                              #
    # ------------------------------------------------------------------ #

    def add_document(self, chunks: List[Dict], doc_id: str, filename: str) -> None:
        """
        Embed *chunks* and upsert them into Endee.

        Each Endee record:
          id     = "<doc_id>_chunk_<i>"
          vector = 384-dim float list
          meta   = {"text", "filename", "doc_id", "chunk_index"}
          filter = {"doc_id": "<doc_id>"}
        """
        texts = [c["text"] for c in chunks]
        embeddings = self.embedder.embed_batch(texts)

        vectors = [
            {
                "id": f"{doc_id}_chunk_{i}",
                "vector": emb,
                "meta": {
                    "text":        chunk["text"],
                    "filename":    filename,
                    "doc_id":      doc_id,
                    "chunk_index": i,
                },
                "filter": {"doc_id": doc_id},
            }
            for i, (chunk, emb) in enumerate(zip(chunks, embeddings))
        ]

        index = self.client.get_index(name=INDEX_NAME)
        for start in range(0, len(vectors), UPSERT_BATCH):
            index.upsert(vectors[start : start + UPSERT_BATCH])

        self.documents_meta[doc_id] = {
            "doc_id":       doc_id,
            "filename":     filename,
            "total_chunks": len(chunks),
        }
        self._save_meta()
        logger.info("Ingested %d chunks for '%s' (doc_id=%s)", len(chunks), filename, doc_id)

    # ------------------------------------------------------------------ #
    #  Retrieval                                                           #
    # ------------------------------------------------------------------ #

    def retrieve(
        self,
        query: str,
        top_k: int = 5,
        doc_id: Optional[str] = None,
    ) -> List[Dict]:
        """
        Embed *query* and search Endee for the *top_k* most similar chunks.
        If *doc_id* is given, results are filtered to that document only.
        """
        query_vec = self.embedder.embed(query)
        index     = self.client.get_index(name=INDEX_NAME)

        params: Dict[str, Any] = {"vector": query_vec, "top_k": top_k}
        if doc_id:
            # Endee payload filter: restrict search to a single document
            params["filter"] = [{"doc_id": {"$eq": doc_id}}]

        return index.query(**params)

    # ------------------------------------------------------------------ #
    #  Answer generation                                                   #
    # ------------------------------------------------------------------ #

    def answer(
        self,
        question: str,
        top_k: int = 5,
        doc_id: Optional[str] = None,
    ) -> Dict[str, Any]:
        """Full RAG pipeline: retrieve → generate → return."""
        results = self.retrieve(question, top_k=top_k, doc_id=doc_id)

        if not results:
            return {
                "answer":   "I couldn't find relevant information to answer your question. "
                            "Please upload relevant documents first.",
                "sources":  [],
                "question": question,
            }

        sources = [
            {
                "text":        r.get("meta", {}).get("text", ""),
                "filename":    r.get("meta", {}).get("filename", ""),
                "chunk_index": r.get("meta", {}).get("chunk_index", 0),
                "similarity":  round(float(r.get("similarity", 0)), 4),
            }
            for r in results
        ]

        context = "\n\n---\n\n".join(s["text"] for s in sources)
        generated = self._generate(question, context)

        return {"answer": generated, "sources": sources, "question": question}

    def _generate(self, question: str, context: str) -> str:
        """Try OpenAI → Ollama → retrieval-only fallback."""
        if os.getenv("OPENAI_API_KEY"):
            return self._openai_generate(question, context)
        if os.getenv("OLLAMA_BASE_URL"):
            return self._ollama_generate(question, context)
        return self._fallback_generate(context)

    def _openai_generate(self, question: str, context: str) -> str:
        try:
            from openai import OpenAI

            client = OpenAI(api_key=os.environ["OPENAI_API_KEY"])
            prompt = (
                "You are a helpful assistant. Answer the question ONLY using the "
                "provided context. If the context is insufficient, say so clearly.\n\n"
                f"Context:\n{context}\n\n"
                f"Question: {question}\n\nAnswer:"
            )
            resp = client.chat.completions.create(
                model=os.getenv("OPENAI_MODEL", "gpt-3.5-turbo"),
                messages=[{"role": "user", "content": prompt}],
                max_tokens=512,
                temperature=0.1,
            )
            return resp.choices[0].message.content.strip()
        except Exception as exc:
            logger.error("OpenAI error: %s", exc)
            return self._fallback_generate(context)

    def _ollama_generate(self, question: str, context: str) -> str:
        try:
            import requests

            url = f"{os.environ['OLLAMA_BASE_URL'].rstrip('/')}/api/generate"
            payload = {
                "model":  os.getenv("OLLAMA_MODEL", "llama3"),
                "prompt": f"Context:\n{context}\n\nQuestion: {question}\nAnswer:",
                "stream": False,
            }
            resp = requests.post(url, json=payload, timeout=120)
            resp.raise_for_status()
            return resp.json().get("response", "").strip()
        except Exception as exc:
            logger.error("Ollama error: %s", exc)
            return self._fallback_generate(context)

    def _fallback_generate(self, context: str) -> str:
        """No LLM configured — return the retrieved context directly."""
        preview = context[:2000] + ("…" if len(context) > 2000 else "")
        return (
            "**[Retrieval-only mode — no LLM configured]**\n\n"
            "Most relevant passages from your documents:\n\n"
            + preview
        )

    # ------------------------------------------------------------------ #
    #  Document listing / deletion                                         #
    # ------------------------------------------------------------------ #

    def list_documents(self) -> List[Dict]:
        return list(self.documents_meta.values())

    def delete_document(self, doc_id: str) -> None:
        if doc_id not in self.documents_meta:
            raise KeyError(f"Document '{doc_id}' not found.")

        total = self.documents_meta[doc_id].get("total_chunks", 0)
        index = self.client.get_index(name=INDEX_NAME)

        deleted = 0
        for i in range(total):
            try:
                index.delete_vector(f"{doc_id}_chunk_{i}")
                deleted += 1
            except Exception:
                pass  # chunk may already be gone

        del self.documents_meta[doc_id]
        self._save_meta()
        logger.info("Deleted doc_id=%s (%d/%d chunks removed)", doc_id, deleted, total)
