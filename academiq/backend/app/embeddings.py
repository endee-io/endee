from sentence_transformers import SentenceTransformer
import torch

MODEL_NAME = "all-MiniLM-L6-v2"
DIMENSION = 384  # must match EndeeManager.DIMENSION


class EmbeddingEngine:
    """
    Generates 384-dimensional dense embeddings using all-MiniLM-L6-v2.
    Runs locally — no API key required.
    """

    def __init__(self):
        device = "cuda" if torch.cuda.is_available() else "cpu"
        self.model = SentenceTransformer(MODEL_NAME, device=device)
        print(f"[Embeddings] Loaded {MODEL_NAME} on {device}")

    def embed(self, text: str) -> list[float]:
        """Embed a single string. Returns list of 384 floats."""
        return self.model.encode(text, normalize_embeddings=True).tolist()

    def embed_batch(self, texts: list[str], batch_size: int = 64) -> list[list[float]]:
        """Embed a list of strings efficiently. Returns list of embedding vectors."""
        embeddings = self.model.encode(
            texts,
            batch_size=batch_size,
            normalize_embeddings=True,
            show_progress_bar=len(texts) > 10
        )
        return embeddings.tolist()
