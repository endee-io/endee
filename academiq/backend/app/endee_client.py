from endee import Endee, Precision


class EndeeManager:
    """
    Manages all interactions with the Endee vector database using the official Python SDK.
    Creates and maintains the 'academiq_docs' index for storing document embeddings.
    """

    INDEX_NAME = "academiq_docs"
    DIMENSION = 384

    def __init__(self, base_url: str, auth_token: str = ""):
        """Initialize using official Endee Python SDK."""
        self._Precision = Precision
        self.client = Endee(auth_token if auth_token else "")
        self.client.set_base_url(base_url)
        self._ensure_index_exists()

    def _ensure_index_exists(self):
        """Create the academiq_docs index if it doesn't already exist."""
        try:
            existing = self.client.list_indexes()
            names = [idx if isinstance(idx, str) else idx.get("name", "") for idx in (existing or [])]
            if self.INDEX_NAME not in names:
                self.client.create_index(
                    name=self.INDEX_NAME,
                    dimension=self.DIMENSION,
                    space_type="cosine",
                    precision=self._Precision.INT8
                )
                print(f"[Endee] Created index '{self.INDEX_NAME}'")
            else:
                print(f"[Endee] Index '{self.INDEX_NAME}' already exists")
        except Exception as e:
            print(f"[Endee] Warning during index init: {e}")

    def upsert_chunks(self, chunks: list[dict]) -> int:
        """
        Insert document chunks into Endee.
        Each chunk dict must have: id, vector (list[float]), text, doc_name.
        Returns number of chunks inserted.
        """
        index = self.client.get_index(name=self.INDEX_NAME)
        vectors = [
            {
                "id": chunk["id"],
                "vector": chunk["vector"],
                "meta": {
                    "text": chunk["text"],
                    "doc_name": chunk["doc_name"],
                    "chunk_index": chunk.get("chunk_index", 0)
                },
                "filter": {
                    "doc_name": chunk["doc_name"][:50]  # for filtered queries
                }
            }
            for chunk in chunks
        ]
        index.upsert(vectors)
        return len(vectors)

    def search(self, query_vector: list[float], top_k: int = 5, doc_name_filter: str = None) -> list[dict]:
        """
        Semantic search: returns top_k most similar chunks.
        Optionally filter by doc_name using Endee's filter parameter.
        Each result: {"id", "score", "text", "doc_name", "chunk_index"}
        """
        index = self.client.get_index(name=self.INDEX_NAME)

        kwargs = dict(vector=query_vector, top_k=top_k, ef=128)
        if doc_name_filter:
            kwargs["filter"] = [{"doc_name": {"$eq": doc_name_filter[:50]}}]

        raw = index.query(**kwargs)
        return [
            {
                "id": r["id"],
                "score": round(float(r.get("similarity", 0)), 4),
                "text": r.get("meta", {}).get("text", ""),
                "doc_name": r.get("meta", {}).get("doc_name", "unknown"),
                "chunk_index": r.get("meta", {}).get("chunk_index", 0)
            }
            for r in (raw or [])
        ]

    def get_vector(self, vector_id: str) -> dict:
        """Retrieve a stored vector and its metadata by ID."""
        try:
            index = self.client.get_index(name=self.INDEX_NAME)
            result = index.get_vector(vector_id)
            return result or {}
        except Exception as e:
            return {"error": str(e)}

    def describe_index(self) -> dict:
        """Returns index stats from Endee."""
        try:
            index = self.client.get_index(name=self.INDEX_NAME)
            return index.describe() or {}
        except Exception as e:
            return {"error": str(e)}

    def list_all_indexes(self) -> list[dict]:
        """List all indexes in this Endee instance."""
        try:
            return self.client.list_indexes() or []
        except Exception as e:
            return [{"error": str(e)}]

    def is_healthy(self) -> bool:
        """Ping Endee by listing indexes."""
        try:
            self.client.list_indexes()
            return True
        except Exception:
            return False
