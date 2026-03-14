"""
Endee Database Client

A simple wrapper around the Endee REST API.
Handles all communication with the vector database.
"""

import json
import math
import requests
from typing import Callable, Optional
from config import config

try:
    import msgpack  # type: ignore[reportMissingImports]
except Exception:
    msgpack = None


class EndeeError(Exception):
    """Custom exception for Endee API errors."""
    pass


class EndeeClient:
    """
    Client for interacting with the Endee vector database.
    
    Basic usage:
        client = EndeeClient()
        client.create_index("my-index", dimensions=1536)
        client.insert_vectors("my-index", vectors=[...], ids=[...], metadata=[...])
        results = client.search("my-index", query_vector=[...], top_k=10)
    """
    
    def __init__(self, base_url: Optional[str] = None, api_key: Optional[str] = None):
        """
        Initialize the Endee client.
        
        Args:
            base_url: Endee server URL (default: from config)
            api_key: API key for authentication (default: from config)
        """
        self.base_url = (base_url or config.ENDEE_URL).rstrip("/")
        self.api_key = api_key or config.ENDEE_API_KEY
        
        self.session = requests.Session()
        if self.api_key:
            self.session.headers["Authorization"] = f"Bearer {self.api_key}"
        self.session.headers["Content-Type"] = "application/json"
    
    def _request(self, method: str, endpoint: str, **kwargs) -> dict:
        """Make an HTTP request to the Endee API."""
        url = f"{self.base_url}{endpoint}"
        timeout = kwargs.pop("timeout", 60)
        
        try:
            response = self.session.request(method, url, timeout=timeout, **kwargs)
            response.raise_for_status()

            if not response.text:
                return {}

            content_type = response.headers.get("Content-Type", "")
            if "application/json" in content_type:
                return response.json()

            text = response.text.strip()
            if text.startswith("{") or text.startswith("["):
                try:
                    return json.loads(text)
                except Exception:
                    pass

            return {"message": response.text}
        except requests.exceptions.ConnectionError:
            raise EndeeError(f"Cannot connect to Endee at {self.base_url}. Is the server running?")
        except requests.exceptions.HTTPError as e:
            error_msg = response.text if "response" in locals() and response is not None else str(e)
            raise EndeeError(f"Endee API error: {error_msg}")
        except Exception as e:
            raise EndeeError(f"Request failed: {e}")
    
    # ==================== Health Check ====================
    
    def health_check(self) -> bool:
        """Check if the Endee server is running."""
        try:
            self._request("GET", "/api/v1/health")
            return True
        except EndeeError:
            return False
    
    # ==================== Index Management ====================
    
    def create_index(
        self,
        name: str,
        dimensions: int,
        metric: str = "cosine",
        quantization: str = "int8",
        max_elements: int = 100000,
    ) -> dict:
        """
        Create a new vector index.
        
        Args:
            name: Unique name for the index
            dimensions: Vector dimensions (must match your embedding model)
            metric: Distance metric - "cosine", "l2", or "ip" (inner product)
            quantization: Quantization level - "fp32", "fp16", "int16", "int8", "binary"
            max_elements: Initial capacity (auto-expands)
        
        Returns:
            API response dict
        """
        payload = {
            "index_name": name,
            "dim": dimensions,
            "space_type": metric,
            "precision": quantization,
        }
        
        return self._request("POST", "/api/v1/index/create", json=payload)
    
    def delete_index(self, name: str) -> dict:
        """Delete an index."""
        return self._request("DELETE", f"/api/v1/index/{name}/delete")
    
    def list_indices(self) -> list:
        """List all indices."""
        response = self._request("GET", "/api/v1/index/list")
        return response.get("indexes", [])
    
    def get_index_info(self, name: str) -> dict:
        """Get information about an index."""
        return self._request("GET", f"/api/v1/index/{name}/info")
    
    def index_exists(self, name: str) -> bool:
        """Check if an index exists."""
        try:
            self.get_index_info(name)
            return True
        except EndeeError:
            return False
    
    # ==================== Vector Operations ====================
    
    def insert_vectors(
        self,
        index_name: str,
        vectors: list[list[float]],
        ids: list[str],
        metadata: Optional[list[dict]] = None,
    ) -> dict:
        """
        Insert vectors into an index.
        
        Args:
            index_name: Name of the target index
            vectors: List of embedding vectors
            ids: List of unique IDs for each vector
            metadata: Optional list of metadata dicts for filtering
        
        Returns:
            API response dict
        """
        payload = []
        for i, vector in enumerate(vectors):
            norm = math.sqrt(sum(float(v) * float(v) for v in vector)) if vector else 0.0
            item = {
                "id": ids[i],
                "vector": vector,
                "norm": norm,
            }
            if metadata and i < len(metadata) and metadata[i]:
                item["filter"] = json.dumps(metadata[i])
            payload.append(item)
        
        return self._request(
            "POST",
            f"/api/v1/index/{index_name}/vector/insert",
            json=payload
        )
    
    def delete_vectors(self, index_name: str, ids: list[str]) -> dict:
        """Delete vectors by ID."""
        deleted = 0
        for vector_id in ids:
            self._request("DELETE", f"/api/v1/index/{index_name}/vector/{vector_id}/delete")
            deleted += 1
        return {"deleted": deleted}
    
    def get_vector(self, index_name: str, vector_id: str) -> dict:
        """Get a specific vector by ID."""
        try:
            response = self.session.post(
                f"{self.base_url}/api/v1/index/{index_name}/vector/get",
                json={"id": vector_id},
                headers={"Content-Type": "application/json", **self.session.headers},
                timeout=60,
            )
            response.raise_for_status()

            if "application/msgpack" in response.headers.get("Content-Type", ""):
                if msgpack is None:
                    raise EndeeError("msgpack package is required to decode vector responses")
                return msgpack.unpackb(response.content, raw=False)

            return response.json() if response.text else {}
        except Exception as e:
            raise EndeeError(f"Failed to get vector: {e}")
    
    # ==================== Search ====================
    
    def search(
        self,
        index_name: str,
        query_vector: list[float],
        top_k: int = 10,
        filter_conditions: Optional[dict] = None,
        ef_search: Optional[int] = None,
    ) -> list[dict]:
        """
        Search for similar vectors.
        
        Args:
            index_name: Name of the index to search
            query_vector: The query embedding vector
            top_k: Number of results to return
            filter_conditions: Optional metadata filters
            ef_search: Search quality parameter (higher = better but slower)
        
        Returns:
            List of results with id, score, and metadata
        """
        payload = {
            "vector": query_vector,
            "k": top_k,
        }
        
        if filter_conditions:
            payload["filter"] = self._encode_filter(filter_conditions)
        
        if ef_search:
            payload["ef"] = ef_search

        try:
            response = self.session.post(
                f"{self.base_url}/api/v1/index/{index_name}/search",
                json=payload,
                headers={"Content-Type": "application/json", **self.session.headers},
                timeout=60,
            )
            response.raise_for_status()

            if "application/msgpack" in response.headers.get("Content-Type", ""):
                if msgpack is None:
                    raise EndeeError("msgpack package is required to decode search responses")
                decoded = msgpack.unpackb(response.content, raw=False)
                return self._normalize_search_results(decoded)

            data = response.json() if response.text else {}
            return self._normalize_search_results(data)
        except Exception as e:
            raise EndeeError(f"Search failed: {e}")
    
    def hybrid_search(
        self,
        index_name: str,
        query_vector: Optional[list[float]] = None,
        sparse_vector: Optional[dict[str, float]] = None,
        top_k: int = 10,
        alpha: float = 0.5,
        filter_conditions: Optional[dict] = None,
    ) -> list[dict]:
        """
        Hybrid search combining dense and sparse vectors.
        
        Args:
            index_name: Name of the index
            query_vector: Dense embedding vector
            sparse_vector: Sparse vector as {term: weight} dict
            top_k: Number of results
            alpha: Weight between dense (1.0) and sparse (0.0)
            filter_conditions: Optional filters
        
        Returns:
            List of results
        """
        payload: dict[str, object] = {"k": top_k}
        
        if query_vector:
            payload["vector"] = query_vector
        if sparse_vector:
            sparse_indices = []
            sparse_values = []
            for key, value in sparse_vector.items():
                try:
                    sparse_indices.append(int(key))
                    sparse_values.append(float(value))
                except Exception:
                    continue
            if sparse_indices and sparse_values:
                payload["sparse_indices"] = sparse_indices
                payload["sparse_values"] = sparse_values
        if filter_conditions:
            payload["filter"] = self._encode_filter(filter_conditions)

        return self.search(
            index_name=index_name,
            query_vector=query_vector or [],
            top_k=top_k,
            filter_conditions=filter_conditions,
        )
    
    # ==================== Batch Operations ====================
    
    def insert_batch(
        self,
        index_name: str,
        items: list[dict],
        batch_size: int = 100,
        on_progress: Optional[Callable[[int, int], None]] = None,
    ) -> int:
        """
        Insert items in batches.
        
        Args:
            index_name: Target index name
            items: List of dicts with 'id', 'vector', and optional 'metadata'
            batch_size: Number of items per batch
            on_progress: Optional callback(inserted, total)
        
        Returns:
            Total number of items inserted
        """
        total = len(items)
        inserted = 0
        
        for i in range(0, total, batch_size):
            batch = items[i:i + batch_size]
            
            vectors = [item["vector"] for item in batch]
            ids = [item["id"] for item in batch]
            metadata = [item.get("metadata", {}) for item in batch]
            
            self.insert_vectors(index_name, vectors, ids, metadata)
            
            inserted += len(batch)
            if on_progress:
                on_progress(inserted, total)
        
        return inserted

    def _encode_filter(self, filter_conditions: dict) -> str:
        """Convert simple dict-based filter conditions into Endee's array JSON format."""
        if isinstance(filter_conditions, str):
            return filter_conditions

        filters = []
        for field, condition in filter_conditions.items():
            if isinstance(condition, dict):
                filters.append({field: condition})
            else:
                filters.append({field: {"$eq": condition}})

        return json.dumps(filters)

    def _normalize_search_results(self, payload: object) -> list[dict]:
        """Normalize Endee search payloads (dict/list/msgpack variants) to a stable result list."""
        items: list[object]

        if isinstance(payload, dict):
            candidate = payload.get("results", [])
            if isinstance(candidate, list):
                items = candidate
            else:
                items = []
        elif isinstance(payload, list):
            items = payload
        else:
            items = []

        normalized: list[dict] = []
        for item in items:
            if isinstance(item, dict):
                parsed_metadata: dict = {}
                raw_filter = item.get("filter", "")
                if isinstance(raw_filter, str) and raw_filter:
                    try:
                        parsed_metadata = json.loads(raw_filter)
                    except Exception:
                        parsed_metadata = {"raw_filter": raw_filter}

                normalized.append(
                    {
                        "id": item.get("id", ""),
                        "score": item.get("similarity", item.get("score", 0.0)),
                        "metadata": parsed_metadata,
                    }
                )
                continue

            if isinstance(item, (list, tuple)) and len(item) >= 2:
                similarity = float(item[0]) if isinstance(item[0], (int, float)) else 0.0
                item_id = str(item[1]) if item[1] is not None else ""
                raw_filter = item[3] if len(item) > 3 else ""

                parsed_metadata: dict = {}
                if isinstance(raw_filter, str) and raw_filter:
                    try:
                        parsed_metadata = json.loads(raw_filter)
                    except Exception:
                        parsed_metadata = {"raw_filter": raw_filter}

                normalized.append(
                    {
                        "id": item_id,
                        "score": similarity,
                        "metadata": parsed_metadata,
                    }
                )

        return normalized


# Convenience function to get a client instance
def get_client() -> EndeeClient:
    """Get a configured Endee client instance."""
    return EndeeClient()
