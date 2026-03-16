from endee import Endee
from embedder import create_embeddings

client = Endee()

index = client.create_index(
    name="documents",
    dimension=384,
    space_type="cosine"
)

def add_documents(texts):

    embeddings = create_embeddings(texts)

    vectors = []

    for i, emb in enumerate(embeddings):
        vectors.append({
            "id": str(i),
            "vector": emb.tolist(),
            "metadata": {"text": texts[i]}
        })

    index.upsert(vectors)