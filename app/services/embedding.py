from sentence_transformers import SentenceTransformer
from app.services.vector_store import add_vector

model = SentenceTransformer("all-MiniLM-L6-v2")


def chunk_text(text, chunk_size=300):
    words = text.split()
    return [" ".join(words[i:i+chunk_size]) for i in range(0, len(words), chunk_size)]


def get_embeddings(chunks):
    return model.encode(chunks).tolist()


def store_chunks(chunks, embeddings):
    for i, chunk in enumerate(chunks):
        add_vector(
            vector=embeddings[i],
            metadata={"text": chunk}
        )