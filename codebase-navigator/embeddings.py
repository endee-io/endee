"""
Embedding Generator

Generates vector embeddings from text using OpenAI's API.
Can be easily extended to support other embedding providers.
"""

import hashlib
import json
import math
from pathlib import Path
from typing import Callable, Optional
from openai import OpenAI

from config import config


class EmbeddingCache:
    """Simple file-based cache for embeddings to avoid re-computing."""
    
    def __init__(self, cache_dir: str = ".embedding_cache"):
        self.cache_dir = Path(cache_dir)
        self.cache_dir.mkdir(exist_ok=True)
    
    def _get_key(self, text: str, model: str) -> str:
        """Generate a cache key from text and model."""
        content = f"{model}:{text}"
        return hashlib.md5(content.encode()).hexdigest()
    
    def get(self, text: str, model: str) -> Optional[list[float]]:
        """Get cached embedding if exists."""
        key = self._get_key(text, model)
        cache_file = self.cache_dir / f"{key}.json"
        
        if cache_file.exists():
            with open(cache_file, "r") as f:
                return json.load(f)
        return None
    
    def set(self, text: str, model: str, embedding: list[float]):
        """Cache an embedding."""
        key = self._get_key(text, model)
        cache_file = self.cache_dir / f"{key}.json"
        
        with open(cache_file, "w") as f:
            json.dump(embedding, f)
    
    def clear(self):
        """Clear all cached embeddings."""
        for file in self.cache_dir.glob("*.json"):
            file.unlink()


class EmbeddingGenerator:
    """
    Generates embeddings using OpenAI's API.
    
    Usage:
        generator = EmbeddingGenerator()
        embedding = generator.embed("some code here")
        embeddings = generator.embed_batch(["code1", "code2", "code3"])
    """
    
    def __init__(
        self,
        model: Optional[str] = None,
        api_key: Optional[str] = None,
        use_cache: bool = True,
    ):
        """
        Initialize the embedding generator.
        
        Args:
            model: OpenAI embedding model name
            api_key: OpenAI API key
            use_cache: Whether to cache embeddings locally
        """
        self.model = model or config.EMBEDDING_MODEL
        self.api_key = api_key or config.OPENAI_API_KEY
        self.use_openai = bool(self.api_key)
        self.client = OpenAI(api_key=self.api_key) if self.use_openai else None
        self.cache = EmbeddingCache() if use_cache else None
        
        # Track usage for cost estimation
        self.total_tokens = 0
    
    def embed(self, text: str) -> list[float]:
        """
        Generate embedding for a single text.
        
        Args:
            text: Input text to embed
        
        Returns:
            Embedding vector as list of floats
        """
        # Check cache first
        if self.cache:
            cached = self.cache.get(text, self.model)
            if cached:
                return cached
        
        # Truncate if too long (OpenAI has token limits)
        text = self._truncate(text)

        if self.use_openai and self.client is not None:
            response = self.client.embeddings.create(
                model=self.model,
                input=text,
            )
            embedding = response.data[0].embedding
            self.total_tokens += response.usage.total_tokens
        else:
            embedding = self._local_embed(text)
        
        # Cache the result
        if self.cache:
            self.cache.set(text, self.model, embedding)
        
        return embedding
    
    def embed_batch(
        self,
        texts: list[str],
        batch_size: int = 100,
        on_progress: Optional[Callable[[int, int], None]] = None,
    ) -> list[list[float]]:
        """
        Generate embeddings for multiple texts.
        
        Args:
            texts: List of texts to embed
            batch_size: Number of texts per API call
            on_progress: Optional callback(processed, total)
        
        Returns:
            List of embedding vectors
        """
        all_embeddings = []
        total = len(texts)
        
        for i in range(0, total, batch_size):
            batch_texts = texts[i:i + batch_size]
            batch_embeddings = []
            
            # Check cache for each text
            uncached_indices = []
            uncached_texts = []
            
            for j, text in enumerate(batch_texts):
                if self.cache:
                    cached = self.cache.get(text, self.model)
                    if cached:
                        batch_embeddings.append((j, cached))
                        continue
                uncached_indices.append(j)
                uncached_texts.append(self._truncate(text))
            
            # Get embeddings for uncached texts
            if uncached_texts:
                if self.use_openai and self.client is not None:
                    response = self.client.embeddings.create(
                        model=self.model,
                        input=uncached_texts,
                    )

                    self.total_tokens += response.usage.total_tokens

                    for idx, data in zip(uncached_indices, response.data):
                        embedding = data.embedding
                        batch_embeddings.append((idx, embedding))

                        if self.cache:
                            self.cache.set(batch_texts[idx], self.model, embedding)
                else:
                    for idx, text in zip(uncached_indices, uncached_texts):
                        embedding = self._local_embed(text)
                        batch_embeddings.append((idx, embedding))
                        if self.cache:
                            self.cache.set(batch_texts[idx], self.model, embedding)
            
            # Sort by original index and extract embeddings
            batch_embeddings.sort(key=lambda x: x[0])
            all_embeddings.extend([emb for _, emb in batch_embeddings])
            
            if on_progress:
                on_progress(min(i + batch_size, total), total)
        
        return all_embeddings
    
    def _truncate(self, text: str, max_chars: int = 30000) -> str:
        """Truncate text to avoid token limit issues."""
        if len(text) > max_chars:
            return text[:max_chars] + "..."
        return text

    def _local_embed(self, text: str) -> list[float]:
        """Generate deterministic local embeddings without external APIs."""
        dimensions = self.get_dimensions()
        vector = [0.0] * dimensions

        tokens = text.lower().split()
        if not tokens:
            return vector

        for token in tokens:
            digest = hashlib.sha256(token.encode("utf-8")).hexdigest()
            idx = int(digest[:8], 16) % dimensions
            sign = 1.0 if int(digest[8:10], 16) % 2 == 0 else -1.0
            weight = 1.0 + (int(digest[10:12], 16) / 255.0)
            vector[idx] += sign * weight

        norm = math.sqrt(sum(v * v for v in vector))
        if norm > 0:
            vector = [v / norm for v in vector]

        return vector
    
    def get_dimensions(self) -> int:
        """Get the embedding dimensions for the current model."""
        return config.EMBEDDING_DIMENSIONS
    
    def estimate_cost(self) -> float:
        """
        Estimate cost based on tokens used.
        
        Note: Prices as of training cutoff. Check OpenAI pricing for current rates.
        """
        # Approximate pricing for text-embedding-3-small
        price_per_1k_tokens = 0.00002
        return (self.total_tokens / 1000) * price_per_1k_tokens


# For generating sparse vectors (keyword-based)
class SparseVectorGenerator:
    """
    Generate sparse vectors for hybrid search.
    
    Creates TF-IDF-like sparse representations from text.
    """
    
    def __init__(self, max_terms: int = 100):
        self.max_terms = max_terms
        
        # Common programming stop words to ignore
        self.stop_words = {
            "the", "a", "an", "is", "are", "was", "were", "be", "been",
            "being", "have", "has", "had", "do", "does", "did", "will",
            "would", "could", "should", "may", "might", "must", "shall",
            "can", "need", "dare", "ought", "used", "to", "of", "in",
            "for", "on", "with", "at", "by", "from", "as", "into",
            "through", "during", "before", "after", "above", "below",
            "between", "under", "again", "further", "then", "once",
            "if", "else", "elif", "while", "for", "and", "or", "not",
            "this", "that", "these", "those", "it", "its",
        }
    
    def generate(self, text: str) -> dict[str, float]:
        """
        Generate a sparse vector from text.
        
        Args:
            text: Input text (code)
        
        Returns:
            Dict mapping terms to weights
        """
        import re
        
        # Tokenize: split on non-alphanumeric, keeping underscores
        tokens = re.findall(r'\b[a-zA-Z_][a-zA-Z0-9_]*\b', text.lower())
        
        # Filter stop words and very short tokens
        tokens = [t for t in tokens if t not in self.stop_words and len(t) > 2]
        
        # Count term frequencies
        term_freq = {}
        for token in tokens:
            term_freq[token] = term_freq.get(token, 0) + 1
        
        # Normalize by max frequency
        if not term_freq:
            return {}
        
        max_freq = max(term_freq.values())
        sparse_vector = {
            term: freq / max_freq
            for term, freq in term_freq.items()
        }
        
        # Keep only top N terms
        if len(sparse_vector) > self.max_terms:
            sorted_terms = sorted(sparse_vector.items(), key=lambda x: -x[1])
            sparse_vector = dict(sorted_terms[:self.max_terms])
        
        return sparse_vector


# Convenience functions
def get_embedding_generator() -> EmbeddingGenerator:
    """Get a configured embedding generator instance."""
    return EmbeddingGenerator()


def get_sparse_generator() -> SparseVectorGenerator:
    """Get a sparse vector generator instance."""
    return SparseVectorGenerator()
