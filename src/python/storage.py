import faiss
import numpy as np

class VectorStore:
    def __init__(self, dim=384):
        self.index = faiss.IndexFlatIP(dim)
        self.data = []

    def add(self, embedding, text, employee_id, department):
        embedding = np.array([embedding]).astype("float32")
        self.index.add(embedding)

        self.data.append({
            "text": text,
            "employee_id": employee_id,
            "department": department
        })

    def search(self, query_embedding, k=5, department=None):
        if len(self.data) == 0:
            return []

        query_embedding = np.array([query_embedding]).astype("float32")
        scores, indices = self.index.search(query_embedding, k)

        results = []
        for i in indices[0]:
            if i < len(self.data):
                record = self.data[i]

                if department and record["department"] != department:
                    continue

                results.append(record)

        return results