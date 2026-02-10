"""
Embedding Service
Generates vector embeddings for text using sentence-transformers
"""

from sentence_transformers import SentenceTransformer
import logging
from typing import List, Union
import numpy as np

logger = logging.getLogger(__name__)

class EmbeddingService:
    """
    Service for generating embeddings using sentence-transformers
    Using all-MiniLM-L6-v2: Fast, efficient, 384-dimensional embeddings
    """
    
    def __init__(self, model_name: str = "all-MiniLM-L6-v2"):
        """
        Initialize embedding model
        
        Args:
            model_name: Name of the sentence-transformer model
                       Default: all-MiniLM-L6-v2 (384 dims, fast, good quality)
        """
        self.model_name = model_name
        logger.info(f"Loading embedding model: {model_name}")
        
        try:
            self.model = SentenceTransformer(model_name)
            self.dimension = self.model.get_sentence_embedding_dimension()
            logger.info(f"Embedding model loaded successfully. Dimension: {self.dimension}")
        except Exception as e:
            logger.error(f"Failed to load embedding model: {e}")
            raise
    
    def encode(
        self,
        texts: Union[str, List[str]],
        batch_size: int = 32,
        show_progress: bool = False
    ) -> Union[List[float], List[List[float]]]:
        """
        Generate embeddings for text(s)
        
        Args:
            texts: Single text string or list of texts
            batch_size: Batch size for encoding
            show_progress: Show progress bar for large batches
        
        Returns:
            Single embedding vector or list of vectors
        """
        try:
            is_single = isinstance(texts, str)
            
            if is_single:
                texts = [texts]
            
            # Generate embeddings
            embeddings = self.model.encode(
                texts,
                batch_size=batch_size,
                show_progress_bar=show_progress,
                normalize_embeddings=True  # Normalize for cosine similarity
            )
            
            # Convert to list format
            embeddings_list = embeddings.tolist()
            
            # Return single vector if input was single text
            if is_single:
                return embeddings_list[0]
            
            return embeddings_list
            
        except Exception as e:
            logger.error(f"Error generating embeddings: {e}")
            raise
    
    def encode_batch(
        self,
        texts: List[str],
        batch_size: int = 32
    ) -> List[List[float]]:
        """
        Optimized batch encoding
        
        Args:
            texts: List of texts to encode
            batch_size: Batch size for processing
        
        Returns:
            List of embedding vectors
        """
        return self.encode(texts, batch_size=batch_size, show_progress=True)
    
    def compute_similarity(
        self,
        embedding1: List[float],
        embedding2: List[float]
    ) -> float:
        """
        Compute cosine similarity between two embeddings
        
        Args:
            embedding1: First embedding vector
            embedding2: Second embedding vector
        
        Returns:
            Similarity score (0-1, higher is more similar)
        """
        try:
            vec1 = np.array(embedding1)
            vec2 = np.array(embedding2)
            
            # Cosine similarity
            similarity = np.dot(vec1, vec2) / (np.linalg.norm(vec1) * np.linalg.norm(vec2))
            
            return float(similarity)
            
        except Exception as e:
            logger.error(f"Error computing similarity: {e}")
            raise
    
    def get_dimension(self) -> int:
        """Return embedding dimension"""
        return self.dimension
    
    def get_model_info(self) -> dict:
        """Return model information"""
        return {
            "model_name": self.model_name,
            "dimension": self.dimension,
            "max_seq_length": self.model.max_seq_length
        }
