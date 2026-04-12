"""
Endee Client Wrapper with Local Fallback.
Tries Endee Cloud/Local → falls back to in-memory NumPy vector store.
This ensures the demo always works, even without Docker.
"""
import os
import json
import time
import numpy as np
from pathlib import Path
from typing import List, Dict, Any, Optional

from config import (
    ENDEE_URL, ENDEE_AUTH_TOKEN, ENDEE_INDEX_NAME,
    ENDEE_DENSE_DIM, ENDEE_SPACE_TYPE, ENDEE_SPARSE_MODEL,
    DATA_DIR,
)


# ─── Local Fallback Vector Store ─────────────────────────────────────────

class LocalIndex:
    """In-memory vector store that mimics the Endee Index API."""

    def __init__(self, name: str, dimension: int, persist_path: str = None):
        self.name = name
        self.dimension = dimension
        self.vectors = {}       # id -> np.array (dense)
        self.sparse_data = {}   # id -> (indices, values)
        self.metadata = {}      # id -> dict
        self.filters = {}       # id -> dict
        self._persist_path = persist_path or str(DATA_DIR / f"{name}_local.json")
        self._load()

    def _load(self):
        """Load persisted data if available."""
        if Path(self._persist_path).exists():
            try:
                with open(self._persist_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                for item in data.get("vectors", []):
                    vid = item["id"]
                    self.vectors[vid] = np.array(item["vector"], dtype=np.float32)
                    self.sparse_data[vid] = (
                        item.get("sparse_indices", []),
                        item.get("sparse_values", []),
                    )
                    self.metadata[vid] = item.get("meta", {})
                    self.filters[vid] = item.get("filter", {})
                print(f"[LocalIndex] Loaded {len(self.vectors)} vectors from {self._persist_path}")
            except Exception as e:
                print(f"[LocalIndex] Could not load: {e}")

    def _save(self):
        """Persist data to disk."""
        data = {"vectors": []}
        for vid in self.vectors:
            data["vectors"].append({
                "id": vid,
                "vector": self.vectors[vid].tolist(),
                "sparse_indices": self.sparse_data.get(vid, ([], []))[0],
                "sparse_values": self.sparse_data.get(vid, ([], []))[1],
                "meta": self.metadata.get(vid, {}),
                "filter": self.filters.get(vid, {}),
            })
        with open(self._persist_path, "w", encoding="utf-8") as f:
            json.dump(data, f)

    def upsert(self, points: List[Dict]):
        """Upsert vectors (mimics Endee API)."""
        for p in points:
            vid = p["id"]
            self.vectors[vid] = np.array(p["vector"], dtype=np.float32)
            self.sparse_data[vid] = (
                p.get("sparse_indices", []),
                p.get("sparse_values", []),
            )
            self.metadata[vid] = p.get("meta", {})
            self.filters[vid] = p.get("filter", {})
        self._save()

    def query(self, vector=None, sparse_indices=None, sparse_values=None,
              top_k=5, ef=128, include_vectors=False, filter=None, **kwargs):
        """Query vectors (mimics Endee API with cosine similarity + BM25 fusion)."""
        if not self.vectors:
            return []

        scores = {}

        # Dense scoring (cosine similarity)
        if vector is not None:
            q_vec = np.array(vector, dtype=np.float32)
            q_norm = np.linalg.norm(q_vec)
            if q_norm > 0:
                q_vec = q_vec / q_norm

            for vid, v in self.vectors.items():
                v_norm = np.linalg.norm(v)
                if v_norm > 0:
                    sim = float(np.dot(q_vec, v / v_norm))
                else:
                    sim = 0.0
                scores[vid] = scores.get(vid, 0.0) + sim

        # Sparse scoring (BM25-like dot product)
        if sparse_indices is not None and sparse_values is not None:
            q_sparse = dict(zip(sparse_indices, sparse_values))
            for vid, (s_idx, s_val) in self.sparse_data.items():
                doc_sparse = dict(zip(s_idx, s_val))
                dot = sum(q_sparse.get(idx, 0) * doc_sparse.get(idx, 0)
                          for idx in set(q_sparse) & set(doc_sparse))
                if dot > 0:
                    # RRF-like fusion: combine dense and sparse
                    scores[vid] = scores.get(vid, 0.0) + dot * 0.3  # weighted

        # Apply filters
        if filter:
            filtered_ids = set(scores.keys())
            for f_cond in filter:
                for field, op_dict in f_cond.items():
                    if isinstance(op_dict, dict):
                        for op, val in op_dict.items():
                            ids_to_remove = set()
                            for vid in filtered_ids:
                                vf = self.filters.get(vid, {})
                                if op == "$eq" and vf.get(field) != val:
                                    ids_to_remove.add(vid)
                                elif op == "$in" and vf.get(field) not in val:
                                    ids_to_remove.add(vid)
                                elif op == "$range":
                                    fval = vf.get(field, 0)
                                    if not (val[0] <= fval <= val[1]):
                                        ids_to_remove.add(vid)
                            filtered_ids -= ids_to_remove
            scores = {k: v for k, v in scores.items() if k in filtered_ids}

        # Sort and return top_k
        sorted_ids = sorted(scores.items(), key=lambda x: x[1], reverse=True)[:top_k]

        results = []
        for vid, sim in sorted_ids:
            item = {
                "id": vid,
                "similarity": sim,
                "meta": self.metadata.get(vid, {}),
            }
            if include_vectors:
                item["vector"] = self.vectors[vid].tolist()
            results.append(item)

        return results

    def get_vector(self, vid: str):
        if vid in self.vectors:
            return {
                "id": vid,
                "vector": self.vectors[vid].tolist(),
                "meta": self.metadata.get(vid, {}),
            }
        return None

    def delete_vector(self, vid: str):
        self.vectors.pop(vid, None)
        self.sparse_data.pop(vid, None)
        self.metadata.pop(vid, None)
        self.filters.pop(vid, None)
        self._save()


class LocalEndee:
    """Mimics the Endee client API using local storage."""

    def __init__(self):
        self._indexes = {}
        print("[EndeeClient] Using LOCAL fallback vector store (no Endee server)")

    def create_index(self, name, dimension, space_type="cosine",
                     sparse_model=None, precision=None, **kwargs):
        self._indexes[name] = LocalIndex(name, dimension)
        return self._indexes[name]

    def get_index(self, name):
        if name not in self._indexes:
            # Try to load from disk
            idx = LocalIndex(name, ENDEE_DENSE_DIM)
            self._indexes[name] = idx
        return self._indexes[name]

    def delete_index(self, name):
        if name in self._indexes:
            del self._indexes[name]
        persist = DATA_DIR / f"{name}_local.json"
        if persist.exists():
            persist.unlink()


# ─── Client Factory ──────────────────────────────────────────────────────

_client_instance = None
_using_local = False


def get_endee_client():
    """
    Get Endee client. Tries:
    1. Endee Cloud (if ENDEE_AUTH_TOKEN set)
    2. Local Endee server (if running on ENDEE_URL)
    3. Fallback to local in-memory store
    """
    global _client_instance, _using_local

    if _client_instance is not None:
        return _client_instance, _using_local

    # Try Endee Cloud
    if ENDEE_AUTH_TOKEN:
        try:
            from endee import Endee
            client = Endee(ENDEE_AUTH_TOKEN)
            # Test connection by listing indexes
            client.list_indexes()
            _client_instance = client
            _using_local = False
            print("[EndeeClient] Connected to Endee Cloud [OK]")
            return _client_instance, _using_local
        except Exception as e:
            print(f"[EndeeClient] Endee Cloud failed: {e}")

    # Try Local Endee server
    try:
        from endee import Endee
        client = Endee()
        if ENDEE_AUTH_TOKEN:
            client.set_base_url(f"{ENDEE_URL}/api/v1")
        # Test with a quick request
        client.list_indexes()
        _client_instance = client
        _using_local = False
        print(f"[EndeeClient] Connected to local Endee at {ENDEE_URL} [OK]")
        return _client_instance, _using_local
    except Exception as e:
        print(f"[EndeeClient] Local Endee failed: {e}")

    # Fallback to local
    print("[EndeeClient] [WARN] No Endee server available. Using local fallback.")
    _client_instance = LocalEndee()
    _using_local = True
    return _client_instance, _using_local


def reset_client():
    """Reset the cached client (useful for reconnection)."""
    global _client_instance, _using_local
    _client_instance = None
    _using_local = False
