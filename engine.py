import os
import json
import numpy as np
from PIL import Image
from sentence_transformers import SentenceTransformer

# 1. Setup the AI Model
print("Loading AI Model...")
model = SentenceTransformer('clip-ViT-B-32')

DB_FILE = "vector_db.json"

def load_local_db():
    if os.path.exists(DB_FILE):
        with open(DB_FILE, 'r') as f:
            return json.load(f)
    return []

def save_local_db(data):
    with open(DB_FILE, 'w') as f:
        json.dump(data, f)

def populate_db(folder_path):
    database = load_local_db()
    files = [f for f in os.listdir(folder_path) if f.lower().endswith((".jpg", ".png", ".jpeg"))]
    
    indexed_new = 0
    for filename in files:
        if any(item['name'] == filename for item in database):
            continue
        print(f"Indexing: {filename}...")
        path = os.path.join(folder_path, filename)
        try:
            img = Image.open(path)
            vector = model.encode(img).tolist()
            database.append({"name": filename, "path": path, "vector": vector})
            indexed_new += 1
        except Exception as e:
            print(f"Error: {e}")
    save_local_db(database)
    print(f"✅ Indexed {indexed_new} images.")

def search_visuals(query):
    database = load_local_db()
    if not database: return []
    
    query_vec = model.encode(query)
    results = []
    
    for item in database:
        item_vec = np.array(item['vector'])
        score = np.dot(query_vec, item_vec) / (np.linalg.norm(query_vec) * np.linalg.norm(item_vec))
        results.append((score, item))
    
    # Sort highest to lowest
    results.sort(key=lambda x: x[0], reverse=True)
    
    final_matches = []
    if results:
        # Check the #1 best match
        best_score, best_item = results[0]
        if best_score > 0.20:
            final_matches.append(best_item)
            
            # ADAPTIVE LOGIC: 
            # If there is a second result and it's at least 90% as good as the first, take it!
            if len(results) > 1:
                second_score, second_item = results[1]
                if second_score >= (best_score * 0.90):
                    final_matches.append(second_item)
                    
    return final_matches

if __name__ == "__main__":
    populate_db("pics")