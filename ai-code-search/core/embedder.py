"""
embedder.py

This module generates vector embeddings for code snippets
using Sentence Transformers.
"""

from sentence_transformers import SentenceTransformer
import numpy as np


class CodeEmbedder:
    def __init__(self, model_name="all-MiniLM-L6-v2"):
        """
        Initialize embedding model.
        """
        print("Loading embedding model...")
        self.model = SentenceTransformer(model_name)
        print("Model loaded successfully.")

    def embed_text(self, text: str):
        """
        Convert a single text/code snippet into a vector embedding.
        """
        embedding = self.model.encode(text)
        return embedding.tolist()

    def embed_batch(self, texts):
        """
        Convert multiple texts into embeddings.
        """
        embeddings = self.model.encode(texts)
        return [e.tolist() for e in embeddings]


# Quick test
if __name__ == "__main__":
    embedder = CodeEmbedder()

    sample_code = """
    def create_access_token(data):
        payload = jwt.encode(data, SECRET_KEY)
        return payload
    """

    vector = embedder.embed_text(sample_code)

    print("Vector length:", len(vector))
    print("First 10 values:", vector[:10])