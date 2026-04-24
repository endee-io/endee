import os
import pickle
from sentence_transformers import SentenceTransformer

# Load model once
model = SentenceTransformer('all-MiniLM-L6-v2')


def load_files(folder="data", limit=50):
    data = []
    count = 0

    for root, _, files in os.walk(folder):
        for file in files:
            if file.endswith(('.py', '.cpp', '.txt', '.md')):
                path = os.path.join(root, file)

                try:
                    with open(path, 'r', encoding='utf-8') as f:
                        data.append((path, f.read()))
                        count += 1
                except:
                    continue

                if count >= limit:
                    return data
    return data


def chunk_text(text, size=300):
    return [text[i:i+size] for i in range(0, len(text), size)]


def process_data(folder="data"):
    files = load_files(folder)

    all_data = []

    for file_name, content in files:
        print(f"Processing: {file_name}")

        chunks = chunk_text(content)[:10]  # 🔥 LIMIT chunks

        embeddings = model.encode(chunks)

        for chunk, emb in zip(chunks, embeddings):
            all_data.append({
                "file": file_name,
                "text": chunk,
                "embedding": emb
            })

    return all_data


if __name__ == "__main__":
    data = process_data("data/searches")  # 🔥 USE SMALL FOLDER

    with open("embeddings.pkl", "wb") as f:
        pickle.dump(data, f)

    print(f"\n✅ Saved {len(data)} chunks to embeddings.pkl")