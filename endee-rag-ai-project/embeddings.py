from sentence_transformers import SentenceTransformer
from endee_db import store_embeddings

model = SentenceTransformer("paraphrase-MiniLM-L3-v2")

def load_documents():
    with open("data/documents.txt","r") as f:
        docs = f.readlines()

    return [d.strip() for d in docs]


def create_embeddings():

    docs = load_documents()

    vectors = model.encode(docs)

    store_embeddings(vectors, docs)


if __name__ == "__main__":
    create_embeddings()