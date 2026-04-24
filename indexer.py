import os
import pickle
import numpy as np
from embedder import get_embedding

DATA_DIR = "data"

vectors = []
metadata = []

for root, _, files in os.walk(DATA_DIR):
    for file in files:
        if file.endswith(".py"):
            path = os.path.join(root, file)

            with open(path, "r", encoding="utf-8") as f:
                code = f.read()

            embedding = get_embedding(code)

            vectors.append(embedding)

            # ✅ IMPORTANT: store BOTH file + code
            metadata.append({
                "file": file,
                "code": code
            })

with open("index.pkl", "wb") as f:
    pickle.dump((vectors, metadata), f)

print("✅ Indexing complete")