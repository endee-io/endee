from sentence_transformers import SentenceTransformer

model = SentenceTransformer("all-MiniLM-L6-v2")

def embed_text(texts):
    return model.encode(texts)

def embed_query(query):
    return model.encode([query])[0]