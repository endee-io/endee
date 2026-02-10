"""
Endee Vector Database Client
Handles all interactions with the Endee vector database
"""

import httpx
import logging
from typing import List, Dict, Any, Optional
import asyncio

logger = logging.getLogger(__name__)

class EndeeClient:
    """Client for interacting with Endee vector database"""
    
    def __init__(self, base_url: str, auth_token: Optional[str] = None):
        self.base_url = base_url.rstrip('/')
        self.auth_token = auth_token
        self.headers = {}
        
        if auth_token:
            self.headers["Authorization"] = auth_token
        
        self.timeout = httpx.Timeout(30.0, connect=10.0)
    
    async def health_check(self) -> bool:
        """Check if Endee is accessible"""
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                response = await client.get(
                    f"{self.base_url}/api/v1/health",
                    headers=self.headers
                )
                return response.status_code == 200
        except Exception as e:
            logger.error(f"Endee health check failed: {e}")
            return False
    
    async def create_index(
        self,
        index_name: str,
        dimension: int,
        metric: str = "cosine",
        quant: str = "int8"
    ) -> Dict[str, Any]:
        """
        Create a new index in Endee
        
        Args:
            index_name: Name of the index
            dimension: Vector dimension (e.g., 384 for all-MiniLM-L6-v2)
            metric: Distance metric (cosine, euclidean, dot)
            quant: Quantization level (float32, int8, binary)
        """
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                payload = {
                    "index_name": index_name,
                    "dim": dimension,
                    "space_type": metric,
                    "quant_bit": int(quant.replace('int', '').replace('binary', '1').replace('float', '32'))
                }
                
                response = await client.post(
                    f"{self.base_url}/api/v1/index/create",
                    json=payload,
                    headers=self.headers
                )
                
                if response.status_code == 200:
                    logger.info(f"Index '{index_name}' created successfully")
                    return response.json()
                elif response.status_code == 400 and "already exists" in response.text.lower():
                    logger.info(f"Index '{index_name}' already exists")
                    return {"status": "exists", "name": index_name}
                else:
                    response.raise_for_status()
                    
        except httpx.HTTPStatusError as e:
            logger.error(f"Failed to create index: {e.response.text}")
            raise
        except Exception as e:
            logger.error(f"Error creating index: {e}")
            raise
    
    async def insert_vectors(
        self,
        index_name: str,
        vectors: List[List[float]],
        ids: Optional[List[str]] = None,
        metadata: Optional[List[Dict[str, Any]]] = None
    ) -> Dict[str, Any]:
        """
        Insert vectors into Endee index
        
        Args:
            index_name: Target index name
            vectors: List of embedding vectors
            ids: Optional custom IDs for vectors
            metadata: Optional metadata for each vector
        """
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                # Build array of vector objects
                vector_objects = []
                for i, vector in enumerate(vectors):
                    vec_obj = {"vector": vector}
                    if ids and i < len(ids):
                        vec_obj["id"] = ids[i]
                    if metadata and i < len(metadata):
                        vec_obj["metadata"] = metadata[i]
                    vector_objects.append(vec_obj)
                
                payload = vector_objects
                
                response = await client.post(
                    f"{self.base_url}/api/v1/index/{index_name}/vector/insert",
                    json=payload,
                    headers=self.headers
                )
                
                response.raise_for_status()
                result = response.json()
                logger.info(f"Inserted {len(vectors)} vectors into '{index_name}'")
                return result
                
        except Exception as e:
            logger.error(f"Error inserting vectors: {e}")
            raise
    
    async def search(
        self,
        index_name: str,
        query_vector: List[float],
        top_k: int = 10,
        filter_metadata: Optional[Dict[str, Any]] = None
    ) -> List[Dict[str, Any]]:
        """
        Search for similar vectors in Endee
        
        Args:
            index_name: Index to search
            query_vector: Query embedding vector
            top_k: Number of results to return
            filter_metadata: Optional metadata filters
        """
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                payload = {
                    "vector": query_vector,
                    "k": top_k
                }
                
                if filter_metadata:
                    payload["filter"] = filter_metadata
                
                response = await client.post(
                    f"{self.base_url}/api/v1/index/{index_name}/search",
                    json=payload,
                    headers=self.headers
                )
                
                response.raise_for_status()
                return response.json()
                
        except Exception as e:
            logger.error(f"Error searching vectors: {e}")
            raise
    
    async def batch_search(
        self,
        index_name: str,
        query_vectors: List[List[float]],
        top_k: int = 10
    ) -> List[List[Dict[str, Any]]]:
        """
        Batch search for multiple query vectors
        """
        try:
            # For batch search, we call search multiple times
            # Endee doesn't have a dedicated batch search endpoint
            results = []
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                for query_vector in query_vectors:
                    payload = {
                        "vector": query_vector,
                        "k": top_k
                    }
                    
                    response = await client.post(
                        f"{self.base_url}/api/v1/index/{index_name}/search",
                        json=payload,
                        headers=self.headers
                    )
                    
                    response.raise_for_status()
                    results.append(response.json())
                
                return results
                
        except Exception as e:
            logger.error(f"Error in batch search: {e}")
            raise
    
    async def get_vector(
        self,
        index_name: str,
        vector_id: str
    ) -> Optional[Dict[str, Any]]:
        """
        Retrieve a specific vector by ID
        """
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                response = await client.get(
                    f"{self.base_url}/vector/{index_name}/{vector_id}",
                    headers=self.headers
                )
                
                if response.status_code == 404:
                    return None
                
                response.raise_for_status()
                return response.json()
                
        except Exception as e:
            logger.error(f"Error retrieving vector: {e}")
            raise
    
    async def delete_vectors(
        self,
        index_name: str,
        vector_ids: List[str]
    ) -> Dict[str, Any]:
        """
        Delete vectors from index
        """
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                payload = {
                    "index": index_name,
                    "ids": vector_ids
                }
                
                response = await client.post(
                    f"{self.base_url}/delete",
                    json=payload,
                    headers=self.headers
                )
                
                response.raise_for_status()
                logger.info(f"Deleted {len(vector_ids)} vectors from '{index_name}'")
                return response.json()
                
        except Exception as e:
            logger.error(f"Error deleting vectors: {e}")
            raise
    
    async def get_index_stats(self, index_name: str) -> Dict[str, Any]:
        """
        Get statistics about an index
        """
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                response = await client.get(
                    f"{self.base_url}/stats/{index_name}",
                    headers=self.headers
                )
                
                response.raise_for_status()
                return response.json()
                
        except Exception as e:
            logger.error(f"Error retrieving index stats: {e}")
            raise
    
    async def list_indices(self) -> List[str]:
        """
        List all available indices
        """
        try:
            async with httpx.AsyncClient(timeout=self.timeout) as client:
                response = await client.get(
                    f"{self.base_url}/indices",
                    headers=self.headers
                )
                
                response.raise_for_status()
                return response.json().get("indices", [])
                
        except Exception as e:
            logger.error(f"Error listing indices: {e}")
            raise
