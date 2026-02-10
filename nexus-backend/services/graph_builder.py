"""
Graph Builder
Constructs knowledge graph from embeddings and similarity relationships
"""

import logging
from typing import List, Dict, Any, Optional, Set, Tuple
import asyncio
from collections import defaultdict
import numpy as np

from services.endee_client import EndeeClient
from services.embedding_service import EmbeddingService
from services.document_processor import DocumentChunk

logger = logging.getLogger(__name__)

class GraphNode:
    """Represents a node in the knowledge graph"""
    
    def __init__(
        self,
        node_id: str,
        label: str,
        summary: str,
        embedding_id: str,
        document_id: str,
        metadata: Dict[str, Any]
    ):
        self.node_id = node_id
        self.label = label
        self.summary = summary
        self.embedding_id = embedding_id
        self.document_id = document_id
        self.metadata = metadata
    
    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.node_id,
            "label": self.label,
            "summary": self.summary,
            "embedding_id": self.embedding_id,
            "document_id": self.document_id,
            "metadata": self.metadata
        }

class GraphEdge:
    """Represents an edge (relationship) in the knowledge graph"""
    
    def __init__(
        self,
        source: str,
        target: str,
        similarity: float,
        relationship_type: str = "semantic_similarity"
    ):
        self.source = source
        self.target = target
        self.similarity = similarity
        self.relationship_type = relationship_type
    
    def to_dict(self) -> Dict[str, Any]:
        return {
            "source": self.source,
            "target": self.target,
            "similarity": self.similarity,
            "relationship_type": self.relationship_type
        }

class GraphBuilder:
    """
    Builds and manages the knowledge graph
    Core intelligence layer that creates semantic connections
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
        self.nodes: Dict[str, GraphNode] = {}
        self.edges: List[GraphEdge] = []
    
    async def add_document_to_graph(
        self,
        document_id: str,
        chunks: List[DocumentChunk]
    ):
        """
        Add document chunks to the knowledge graph
        
        This is where the magic happens:
        1. Generate embeddings for each chunk
        2. Store in Endee
        3. Find semantic relationships
        4. Create graph nodes and edges
        """
        try:
            logger.info(f"Adding document {document_id} to graph with {len(chunks)} chunks")
            
            # Extract text from chunks
            texts = [chunk.text for chunk in chunks]
            
            # Generate embeddings in batch
            embeddings = self.embeddings.encode_batch(texts)
            
            # Prepare data for Endee
            chunk_ids = [chunk.chunk_id for chunk in chunks]
            metadata_list = [
                {
                    "document_id": chunk.document_id,
                    "chunk_index": chunk.chunk_index,
                    "text": chunk.text[:200],  # Store preview
                    "filename": chunk.metadata.get("filename", "")
                }
                for chunk in chunks
            ]
            
            # Insert into Endee
            await self.endee.insert_vectors(
                index_name=self.index_name,
                vectors=embeddings,
                ids=chunk_ids,
                metadata=metadata_list
            )
            
            # Create graph nodes
            for chunk, embedding in zip(chunks, embeddings):
                node = GraphNode(
                    node_id=chunk.chunk_id,
                    label=self._generate_label(chunk.text),
                    summary=chunk.text[:150] + "...",
                    embedding_id=chunk.chunk_id,
                    document_id=chunk.document_id,
                    metadata={
                        "chunk_index": chunk.chunk_index,
                        "filename": chunk.metadata.get("filename", ""),
                        "text_length": len(chunk.text)
                    }
                )
                self.nodes[node.node_id] = node
            
            # Find relationships (edges) between this document and existing knowledge
            await self._discover_relationships(chunk_ids, embeddings)
            
            logger.info(f"Successfully added document {document_id} to graph")
            
        except Exception as e:
            logger.error(f"Error adding document to graph: {e}")
            raise
    
    def _generate_label(self, text: str, max_words: int = 5) -> str:
        """
        Generate a concise label for a node
        Uses first few meaningful words
        """
        words = text.split()[:max_words]
        label = " ".join(words)
        
        if len(label) > 50:
            label = label[:47] + "..."
        
        return label
    
    async def _discover_relationships(
        self,
        new_chunk_ids: List[str],
        new_embeddings: List[List[float]],
        similarity_threshold: float = 0.7,
        top_k: int = 5
    ):
        """
        Discover semantic relationships between new chunks and existing knowledge
        
        This creates the "living network" effect
        """
        try:
            # For each new chunk, find similar existing chunks
            for chunk_id, embedding in zip(new_chunk_ids, new_embeddings):
                results = await self.endee.search(
                    index_name=self.index_name,
                    query_vector=embedding,
                    top_k=top_k + 1  # +1 because it includes self
                )
                
                for result in results:
                    target_id = result.get("id")
                    similarity = result.get("distance", 0)
                    
                    # Skip self-connections and low similarity
                    if target_id == chunk_id or similarity < similarity_threshold:
                        continue
                    
                    # Create bidirectional edge
                    edge = GraphEdge(
                        source=chunk_id,
                        target=target_id,
                        similarity=float(similarity),
                        relationship_type="semantic_similarity"
                    )
                    
                    self.edges.append(edge)
            
            logger.info(f"Discovered {len(self.edges)} relationships")
            
        except Exception as e:
            logger.error(f"Error discovering relationships: {e}")
            # Don't fail the entire operation if relationship discovery fails
    
    async def build_graph(
        self,
        similarity_threshold: float = 0.7,
        max_nodes: int = 100
    ) -> Dict[str, Any]:
        """
        Build the complete knowledge graph
        
        Returns:
            Graph structure with nodes, edges, and statistics
        """
        try:
            # Get all nodes (limit for performance)
            nodes_list = list(self.nodes.values())[:max_nodes]
            
            # Filter edges based on similarity threshold
            filtered_edges = [
                edge for edge in self.edges
                if edge.similarity >= similarity_threshold
            ]
            
            # Calculate graph statistics
            stats = self._calculate_stats(nodes_list, filtered_edges)
            
            return {
                "nodes": [node.to_dict() for node in nodes_list],
                "edges": [edge.to_dict() for edge in filtered_edges],
                "stats": stats
            }
            
        except Exception as e:
            logger.error(f"Error building graph: {e}")
            raise
    
    def _calculate_stats(
        self,
        nodes: List[GraphNode],
        edges: List[GraphEdge]
    ) -> Dict[str, Any]:
        """
        Calculate graph statistics
        """
        # Count unique documents
        unique_docs = set(node.document_id for node in nodes)
        
        # Calculate node degree (connections per node)
        node_degrees = defaultdict(int)
        for edge in edges:
            node_degrees[edge.source] += 1
            node_degrees[edge.target] += 1
        
        avg_degree = sum(node_degrees.values()) / len(nodes) if nodes else 0
        
        # Average similarity
        avg_similarity = sum(edge.similarity for edge in edges) / len(edges) if edges else 0
        
        return {
            "total_nodes": len(nodes),
            "total_edges": len(edges),
            "unique_documents": len(unique_docs),
            "avg_connections_per_node": round(avg_degree, 2),
            "avg_similarity": round(avg_similarity, 3),
            "graph_density": round(len(edges) / (len(nodes) * (len(nodes) - 1)) if len(nodes) > 1 else 0, 4)
        }
    
    async def get_node_details(self, node_id: str) -> Optional[Dict[str, Any]]:
        """
        Get detailed information about a specific node
        Includes related nodes and context
        """
        if node_id not in self.nodes:
            return None
        
        node = self.nodes[node_id]
        
        # Find connected nodes
        connected_edges = [
            edge for edge in self.edges
            if edge.source == node_id or edge.target == node_id
        ]
        
        related_nodes = []
        for edge in connected_edges[:10]:  # Limit related nodes
            related_id = edge.target if edge.source == node_id else edge.source
            if related_id in self.nodes:
                related_nodes.append({
                    "node_id": related_id,
                    "label": self.nodes[related_id].label,
                    "similarity": edge.similarity
                })
        
        return {
            **node.to_dict(),
            "related_nodes": related_nodes,
            "connection_count": len(connected_edges)
        }
    
    async def remove_document_from_graph(self, document_id: str):
        """
        Remove all nodes and edges associated with a document
        """
        # Remove nodes
        nodes_to_remove = [
            node_id for node_id, node in self.nodes.items()
            if node.document_id == document_id
        ]
        
        for node_id in nodes_to_remove:
            del self.nodes[node_id]
        
        # Remove edges connected to removed nodes
        self.edges = [
            edge for edge in self.edges
            if edge.source not in nodes_to_remove and edge.target not in nodes_to_remove
        ]
        
        # Delete from Endee
        if nodes_to_remove:
            await self.endee.delete_vectors(
                index_name=self.index_name,
                vector_ids=nodes_to_remove
            )
        
        logger.info(f"Removed document {document_id} from graph")
    
    async def get_statistics(self) -> Dict[str, Any]:
        """
        Get comprehensive system statistics
        """
        return self._calculate_stats(list(self.nodes.values()), self.edges)
