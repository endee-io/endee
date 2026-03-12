import json
import os
from sentence_transformers import SentenceTransformer
from endee import Endee, Precision

def initialize_db():
    # 1. Initialize the embedding model
    model = SentenceTransformer('all-MiniLM-L6-v2')
    
    # 2. Initialize the Endee Client
    client = Endee() 
    index_name = "interview_db"

    # 3. Create the index if it doesn't exist (all-MiniLM-L6-v2 uses 384 dimensions)
    try:
        client.create_index(
            name=index_name,
            dimension=384,
            space_type="cosine",
            precision=Precision.FLOAT32
        )
        print(f"Created index: {index_name}")
    except Exception as e:
        print(f"Index {index_name} already exists or error: {e}")

    # 4. Get the index object
    index = client.get_index(index_name)
    
    # 5. Load data
    data_path = os.path.join("data", "questions.json")
    with open(data_path, "r") as f:
        questions = json.load(f)

    print("Indexing questions into Endee...")
    
    # Prepare data for batch upsert
    vectors_to_upsert = []
    for i, item in enumerate(questions):
        vector = model.encode(f"{item['skill']}: {item['question']}").tolist()
        vectors_to_upsert.append({
            "id": str(i),
            "vector": vector,
            "metadata": item
        })
    
    # 6. Use .upsert() instead of .add()
    index.upsert(vectors=vectors_to_upsert)
    
    print(f"Successfully indexed {len(questions)} questions.")

if __name__ == "__main__":
    initialize_db()