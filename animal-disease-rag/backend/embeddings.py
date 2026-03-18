from sentence_transformers import SentenceTransformer
from typing import List
import config

class EmbeddingModel:
    def __init__(self, model_name: str = config.EMBEDDING_MODEL):
        self.model = SentenceTransformer(model_name)
        self.vector_size = self.model.get_sentence_embedding_dimension()
    
    def encode(self, texts: List[str]) -> List[List[float]]:
        embeddings = self.model.encode(texts, convert_to_numpy=True)
        return embeddings.tolist()
    
    def encode_single(self, text: str) -> List[float]:
        embedding = self.model.encode([text], convert_to_numpy=True)
        return embedding[0].tolist()
    
    def get_vector_size(self) -> int:
        return self.vector_size
