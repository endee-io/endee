import math
import threading
from typing import Dict, List


class InMemoryVectorStore:
    def __init__(self):
        self._lock = threading.Lock()
        self._items: List[Dict] = []

    def add_documents(self, documents: List[Dict]) -> None:
        with self._lock:
            self._items.extend(documents)

    def search(self, query_vector: List[float], k: int = 5) -> List[Dict]:
        with self._lock:
            results = []
            for item in self._items:
                vector = item.get("vector", [])
                if len(vector) != len(query_vector):
                    continue
                score = self._dot_product(query_vector, vector)
                results.append({"score": score, **item})

        results.sort(key=lambda item: item["score"], reverse=True)
        return results[:k]

    @staticmethod
    def _dot_product(vec_a: List[float], vec_b: List[float]) -> float:
        return sum(float(a) * float(b) for a, b in zip(vec_a, vec_b))

    def reset(self) -> None:
        with self._lock:
            self._items = []
