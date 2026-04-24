# embedder.py
from sentence_transformers import SentenceTransformer

# MUST BE SAME MODEL EVERYWHERE
model = SentenceTransformer("BAAI/bge-small-en-v1.5")

def get_embedding(text: str):
    return model.encode(text, normalize_embeddings=True)