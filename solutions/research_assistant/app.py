import requests
from sentence_transformers import SentenceTransformer
from datasets import load_dataset

# Configuration
print("🚀 Starting AI Research Agent...")
model = SentenceTransformer('all-MiniLM-L6-v2')
ENDEE_URL = "http://localhost:8080"

def run_project():
    # Stream 100 high-quality articles to save memory
    print("📥 Streaming technical articles...")
    dataset = load_dataset("wikipedia", "20220301.en", split='train[:100]', trust_remote_code=True)
    
    print("🧬 Vectorizing and storing in Endee...")
    for i, entry in enumerate(dataset):
        text = entry['text'][:500] # Shorter chunks for stability
        vector = model.encode(text).tolist()
        
        payload = {
            "id": f"wiki-{i}",
            "vector": vector,
            "metadata": {"title": entry['title'], "text": text}
        }
        
        try:
            requests.post(f"{ENDEE_URL}/insert", json=payload)
            if i % 10 == 0: print(f"Indexed {i} documents...")
        except:
            print("❌ Connection failed. Check Docker.")
            return

    # Demo Search
    query = "Explain machine learning"
    q_vec = model.encode(query).tolist()
    res = requests.post(f"{ENDEE_URL}/search", json={"vector": q_vec, "top_k": 1})
    if res.status_code == 200:
        print(f"\n✅ Result Found: {res.json()[0]['metadata']['title']}")

if __name__ == "__main__":
    run_project()