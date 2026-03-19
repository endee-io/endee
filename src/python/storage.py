import faiss
import numpy as np

class VectorStore:
    def __init__(self, dim=384):
        self.index = faiss.IndexFlatIP(dim)
        self.data = []
        self.text_set = set()

    def add(self, embedding, text, employee_id, department):
        if text in self.text_set:
            return

        embedding = np.array([embedding]).astype("float32")
        self.index.add(embedding)

        self.data.append({
            "text": text,
            "employee_id": employee_id,
            "department": department
        })

        self.text_set.add(text)

    def search(self, query_embedding, k=5, department=None):
        if len(self.data) == 0:
            return []

        query_embedding = np.array([query_embedding]).astype("float32")
        scores, indices = self.index.search(query_embedding, k)

        results = []
        seen = set()

        for i in indices[0]:
            if i < len(self.data):
                record = self.data[i]

                if department and record["department"] != department:
                    continue

                if record["text"] not in seen:
                    results.append(record)
                    seen.add(record["text"])

        return results