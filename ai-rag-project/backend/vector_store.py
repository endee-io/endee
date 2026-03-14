import faiss
import numpy as np


class VectorStore:

    def __init__(self, dimension):
        self.index = faiss.IndexFlatL2(dimension)
        self.documents = []

    def add(self, embeddings, docs):
        self.index.add(np.array(embeddings))
        self.documents.extend(docs)

    def search(self, query_embedding, k=3):
        D, I = self.index.search(np.array(query_embedding), k)
        return [self.documents[i] for i in I[0]]
