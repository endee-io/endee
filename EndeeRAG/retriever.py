"""
Retriever Module: Hybrid Search (Dense + Sparse + RRF) with Metadata Filtering
Supports dense-only, sparse-only, and hybrid search modes.
"""
import sys
import time
import logging
from typing import List, Dict, Any, Optional, Tuple

from sentence_transformers import SentenceTransformer
from endee_model import SparseModel

from config import (
    ENDEE_INDEX_NAME,
    DENSE_MODEL_NAME, SPARSE_MODEL_NAME,
    DEFAULT_TOP_K, DEFAULT_EF,
)
from encryption import encryptor
from endee_client import get_endee_client

# UTF-8 safe logging (same pattern as ingest.py)
logger = logging.getLogger("retriever")
if not logger.handlers:
    _handler = logging.StreamHandler(
        open(sys.stdout.fileno(), mode="w", encoding="utf-8", closefd=False)
    )
    _handler.setFormatter(logging.Formatter("%(message)s"))
    logger.addHandler(_handler)
    logger.setLevel(logging.INFO)


class HybridRetriever:
    """
    Retrieval engine supporting:
    - Dense search (semantic similarity via sentence-transformers)
    - Sparse search (BM25 keyword matching via endee-model)
    - Hybrid search (Dense + Sparse fused by Endee's server-side RRF)
    - Metadata filtering ($eq, $in, $range operators)
    """

    def __init__(self):
        logger.info("[Retriever] Initializing models...")
        self.dense_model = SentenceTransformer(DENSE_MODEL_NAME)
        self.sparse_model = SparseModel(model_name=SPARSE_MODEL_NAME)

        # Connect to Endee (Cloud / Local / Fallback)
        self.client, self.using_local = get_endee_client()
        self.index = self.client.get_index(ENDEE_INDEX_NAME)
        logger.info("[Retriever] Ready.")

    def _embed_query_dense(self, query: str) -> List[float]:
        """Generate dense embedding for a query."""
        return self.dense_model.encode(query).tolist()

    def _embed_query_sparse(self, query: str) -> Tuple[List[int], List[float]]:
        """Generate sparse BM25 embedding for a query."""
        sparse_vec = next(self.sparse_model.query_embed(query))
        return sparse_vec.indices.tolist(), sparse_vec.values.tolist()

    def _process_results(self, results: List[Dict], decrypt: bool = True) -> List[Dict[str, Any]]:
        """Process raw Endee results: decrypt metadata, format output."""
        processed = []
        for item in results:
            try:
                meta = item.get("meta", {})

                # Decrypt metadata if needed
                if decrypt and encryptor.enabled:
                    meta = encryptor.decrypt_metadata(meta)

                # Clean text to prevent encoding issues downstream
                text = meta.get("text", "")
                if text:
                    text = text.encode("utf-8", errors="ignore").decode("utf-8")

                processed.append({
                    "id": item.get("id", "unknown"),
                    "similarity": round(float(item.get("similarity", 0.0)), 4),
                    "text": text,
                    "chunk_index": meta.get("chunk_index", -1),
                    "token_count": meta.get("token_count", 0),
                    "filename": meta.get("filename", "unknown"),
                    "title": meta.get("title", "Untitled"),
                    "source_pages": meta.get("source_pages", "[]"),
                    "meta": meta,
                })
            except Exception as e:
                logger.warning(f"[Retriever] Skipped malformed result: {e}")
                continue

        return processed

    # ─── Search Modes ────────────────────────────────────────────────────

    def search_dense(self, query: str, top_k: int = DEFAULT_TOP_K,
                     ef: int = DEFAULT_EF, filters: Optional[List[Dict]] = None,
                     decrypt: bool = True) -> Dict[str, Any]:
        """Dense-only semantic search."""
        t0 = time.time()

        query_vec = self._embed_query_dense(query)

        kwargs = {
            "vector": query_vec,
            "top_k": top_k,
            "ef": ef,
            "include_vectors": False,
        }
        if filters:
            kwargs["filter"] = filters

        results = self.index.query(**kwargs)
        latency = (time.time() - t0) * 1000

        return {
            "mode": "dense",
            "query": query,
            "results": self._process_results(results, decrypt=decrypt),
            "latency_ms": latency,
            "top_k": top_k,
        }

    def search_sparse(self, query: str, top_k: int = DEFAULT_TOP_K,
                      filters: Optional[List[Dict]] = None,
                      decrypt: bool = True) -> Dict[str, Any]:
        """Sparse-only BM25 keyword search."""
        t0 = time.time()

        sparse_indices, sparse_values = self._embed_query_sparse(query)

        kwargs = {
            "sparse_indices": sparse_indices,
            "sparse_values": sparse_values,
            "top_k": top_k,
            "include_vectors": False,
        }
        if filters:
            kwargs["filter"] = filters

        results = self.index.query(**kwargs)
        latency = (time.time() - t0) * 1000

        return {
            "mode": "sparse",
            "query": query,
            "results": self._process_results(results, decrypt=decrypt),
            "latency_ms": latency,
            "top_k": top_k,
        }

    def search_hybrid(self, query: str, top_k: int = DEFAULT_TOP_K,
                      ef: int = DEFAULT_EF, filters: Optional[List[Dict]] = None,
                      decrypt: bool = True) -> Dict[str, Any]:
        """
        Hybrid search: Dense + Sparse combined via Endee's server-side RRF fusion.
        This is the recommended search mode for RAG applications.
        """
        t0 = time.time()

        # Generate both embeddings
        query_vec = self._embed_query_dense(query)
        sparse_indices, sparse_values = self._embed_query_sparse(query)

        kwargs = {
            "vector": query_vec,
            "sparse_indices": sparse_indices,
            "sparse_values": sparse_values,
            "top_k": top_k,
            "ef": ef,
            "include_vectors": False,
        }
        if filters:
            kwargs["filter"] = filters

        results = self.index.query(**kwargs)
        latency = (time.time() - t0) * 1000

        return {
            "mode": "hybrid",
            "query": query,
            "results": self._process_results(results, decrypt=decrypt),
            "latency_ms": latency,
            "top_k": top_k,
        }

    # ─── Convenience Methods ─────────────────────────────────────────────

    def search(self, query: str, mode: str = "hybrid", top_k: int = DEFAULT_TOP_K,
               filters: Optional[List[Dict]] = None, decrypt: bool = True) -> Dict[str, Any]:
        """Unified search interface that routes to the appropriate mode."""
        if mode == "dense":
            return self.search_dense(query, top_k=top_k, filters=filters, decrypt=decrypt)
        elif mode == "sparse":
            return self.search_sparse(query, top_k=top_k, filters=filters, decrypt=decrypt)
        elif mode == "hybrid":
            return self.search_hybrid(query, top_k=top_k, filters=filters, decrypt=decrypt)
        else:
            raise ValueError(f"Unknown search mode: {mode}. Use 'dense', 'sparse', or 'hybrid'.")

    def search_with_filter_by_document(self, query: str, filename: str,
                                        top_k: int = DEFAULT_TOP_K,
                                        mode: str = "hybrid") -> Dict[str, Any]:
        """Search within a specific document using Endee's $eq filter."""
        filters = [{"filename": {"$eq": filename}}]
        return self.search(query, mode=mode, top_k=top_k, filters=filters)

    def search_multi_document(self, query: str, filenames: List[str],
                               top_k: int = DEFAULT_TOP_K,
                               mode: str = "hybrid") -> Dict[str, Any]:
        """Search across multiple specific documents using Endee's $in filter."""
        filters = [{"filename": {"$in": filenames}}]
        return self.search(query, mode=mode, top_k=top_k, filters=filters)

    def compare_search_modes(self, query: str, top_k: int = DEFAULT_TOP_K) -> Dict[str, Any]:
        """Run all three search modes and compare results for benchmarking."""
        dense_result = self.search_dense(query, top_k=top_k)
        sparse_result = self.search_sparse(query, top_k=top_k)
        hybrid_result = self.search_hybrid(query, top_k=top_k)

        return {
            "query": query,
            "dense": dense_result,
            "sparse": sparse_result,
            "hybrid": hybrid_result,
            "comparison": {
                "dense_latency_ms": dense_result["latency_ms"],
                "sparse_latency_ms": sparse_result["latency_ms"],
                "hybrid_latency_ms": hybrid_result["latency_ms"],
                "dense_top_ids": [r["id"] for r in dense_result["results"]],
                "sparse_top_ids": [r["id"] for r in sparse_result["results"]],
                "hybrid_top_ids": [r["id"] for r in hybrid_result["results"]],
            }
        }


if __name__ == "__main__":
    retriever = HybridRetriever()

    query = "What is machine learning?"
    print(f"\n🔍 Query: {query}")

    # Test all modes
    for mode in ["dense", "sparse", "hybrid"]:
        result = retriever.search(query, mode=mode)
        print(f"\n--- {mode.upper()} Search ({result['latency_ms']:.0f}ms) ---")
        for r in result["results"]:
            print(f"  [{r['similarity']:.4f}] {r['text'][:100]}...")
