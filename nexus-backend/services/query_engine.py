"""
Query Engine
Handles semantic queries on the knowledge graph
"""

import logging
from typing import List, Dict, Any
from services.endee_client import EndeeClient
from services.embedding_service import EmbeddingService

logger = logging.getLogger(__name__)

class QueryEngine:
    """
    Executes semantic queries on the knowledge graph
    Transforms natural language queries into graph exploration
    """
    
    def __init__(
        self,
        endee_client: EndeeClient,
        embedding_service: EmbeddingService,
        index_name: str = "nexus_knowledge"
    ):
        self.endee = endee_client
        self.embeddings = embedding_service
        self.index_name = index_name
    
    async def execute_query(
        self,
        query: str,
        top_k: int = 10,
        similarity_threshold: float = 0.7
    ) -> Dict[str, Any]:
        """
        Execute a semantic query on the knowledge graph
        
        Flow:
        1. Convert query to embedding
        2. Search Endee for similar vectors
        3. Retrieve related nodes and relationships
        4. Return structured graph response
        
        Args:
            query: Natural language query
            top_k: Number of results to return
            similarity_threshold: Minimum similarity score
        
        Returns:
            Dictionary with nodes, edges, and metadata
        """
        try:
            logger.info(f"Executing query: {query}")
            
            # Generate query embedding
            query_embedding = self.embeddings.encode(query)
            
            # Search Endee for similar vectors
            search_results = await self.endee.search(
                index_name=self.index_name,
                query_vector=query_embedding,
                top_k=top_k
            )
            
            # Filter by similarity threshold
            filtered_results = [
                result for result in search_results
                if result.get("distance", 0) >= similarity_threshold
            ]
            
            # Build nodes from results
            nodes = []
            node_ids = set()
            
            for result in filtered_results:
                node_id = result.get("id")
                metadata = result.get("metadata", {})
                similarity = result.get("distance", 0)
                
                node = {
                    "id": node_id,
                    "label": metadata.get("text", "")[:50] + "...",
                    "summary": metadata.get("text", ""),
                    "embedding_id": node_id,
                    "document_id": metadata.get("document_id", ""),
                    "metadata": {
                        **metadata,
                        "query_similarity": float(similarity)
                    }
                }
                
                nodes.append(node)
                node_ids.add(node_id)
            
            # Find edges between result nodes
            edges = await self._find_edges_between_nodes(node_ids)
            
            logger.info(f"Query returned {len(nodes)} nodes and {len(edges)} edges")
            
            return {
                "nodes": nodes,
                "edges": edges,
                "result_count": len(nodes)
            }
            
        except Exception as e:
            logger.error(f"Error executing query: {e}")
            raise
    
    async def _find_edges_between_nodes(
        self,
        node_ids: set,
        similarity_threshold: float = 0.7
    ) -> List[Dict[str, Any]]:
        """
        Find relationships (edges) between a set of nodes
        Uses Endee similarity search to discover connections
        """
        edges = []
        processed_pairs = set()
        
        try:
            # For each node, find its neighbors in the result set
            for node_id in list(node_ids)[:10]:  # Limit for performance
                # Get the vector for this node
                # Note: In production, we'd cache vectors
                results = await self.endee.search(
                    index_name=self.index_name,
                    query_vector=None,  # Would need to retrieve node's vector
                    top_k=5
                )
                
                for result in results:
                    target_id = result.get("id")
                    similarity = result.get("distance", 0)
                    
                    # Check if target is in our node set
                    if target_id not in node_ids or target_id == node_id:
                        continue
                    
                    # Check if already processed
                    pair = tuple(sorted([node_id, target_id]))
                    if pair in processed_pairs:
                        continue
                    
                    # Check similarity threshold
                    if similarity < similarity_threshold:
                        continue
                    
                    edges.append({
                        "source": node_id,
                        "target": target_id,
                        "similarity": float(similarity),
                        "relationship_type": "semantic_similarity"
                    })
                    
                    processed_pairs.add(pair)
            
        except Exception as e:
            logger.error(f"Error finding edges: {e}")
            # Return empty edges rather than failing
        
        return edges
    
    async def find_related_concepts(
        self,
        concept: str,
        top_k: int = 5
    ) -> List[Dict[str, Any]]:
        """
        Find concepts related to a given concept
        
        Args:
            concept: Concept to find relations for
            top_k: Number of related concepts to return
        
        Returns:
            List of related concepts with similarity scores
        """
        try:
            # Generate embedding for concept
            concept_embedding = self.embeddings.encode(concept)
            
            # Search for similar concepts
            results = await self.endee.search(
                index_name=self.index_name,
                query_vector=concept_embedding,
                top_k=top_k
            )
            
            related = []
            for result in results:
                metadata = result.get("metadata", {})
                related.append({
                    "concept": metadata.get("text", "")[:100],
                    "similarity": result.get("distance", 0),
                    "source": metadata.get("filename", "Unknown")
                })
            
            return related
            
        except Exception as e:
            logger.error(f"Error finding related concepts: {e}")
            raise
    
    async def get_knowledge_clusters(
        self,
        num_clusters: int = 5
    ) -> List[Dict[str, Any]]:
        """
        Identify knowledge clusters in the graph
        Uses simple heuristic clustering based on high-similarity connections
        
        This is a stretch feature - keep implementation simple
        """
        try:
            # In production, would use proper clustering algorithms
            # For now, return mock structure to demonstrate concept
            
            return [
                {
                    "cluster_id": f"cluster_{i}",
                    "topic": f"Topic {i}",
                    "node_count": 0,
                    "keywords": []
                }
                for i in range(num_clusters)
            ]
            
        except Exception as e:
            logger.error(f"Error identifying clusters: {e}")
            return []
    
    def suggest_knowledge_gaps(
        self,
        existing_topics: List[str]
    ) -> List[str]:
        """
        Suggest potential knowledge gaps
        Simple heuristic-based suggestions
        
        This is optional stretch feature - keep simple
        """
        # Common related topics for demonstration
        gap_suggestions = {
            "machine learning": ["deep learning", "reinforcement learning", "optimization"],
            "neural networks": ["backpropagation", "activation functions", "regularization"],
            "transformers": ["attention mechanism", "self-attention", "positional encoding"],
            "python": ["asyncio", "decorators", "generators"],
            "vector databases": ["embeddings", "similarity search", "indexing"]
        }
        
        suggestions = []
        for topic in existing_topics:
            topic_lower = topic.lower()
            for key, gaps in gap_suggestions.items():
                if key in topic_lower:
                    suggestions.extend(gaps)
        
        return list(set(suggestions))[:5]  # Return unique suggestions
