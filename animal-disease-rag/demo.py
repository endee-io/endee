import numpy as np
from sklearn.metrics.pairwise import cosine_similarity
import pandas as pd
from sentence_transformers import SentenceTransformer
import google.generativeai as genai
import os
from dotenv import load_dotenv

load_dotenv()

class SimpleVectorStore:
    def __init__(self):
        self.embeddings = []
        self.metadata = []
        self.model = None
    
    def load_model(self):
        if self.model is None:
            print("Loading embedding model...")
            self.model = SentenceTransformer('sentence-transformers/all-MiniLM-L6-v2')
        return self.model
    
    def ingest(self, data_list):
        model = self.load_model()
        texts = [d['text'] for d in data_list]
        self.embeddings = model.encode(texts).tolist()
        self.metadata = data_list
        print(f"Ingested {len(data_list)} records")
    
    def search(self, query, top_k=3):
        model = self.load_model()
        query_emb = model.encode([query])
        similarities = cosine_similarity(query_emb, self.embeddings)[0]
        top_indices = np.argsort(similarities)[-top_k:][::-1]
        
        results = []
        for idx in top_indices:
            results.append({
                **self.metadata[idx],
                'score': float(similarities[idx])
            })
        return results

class RAGPipeline:
    def __init__(self):
        api_key = os.getenv("GEMINI_API_KEY", "")
        if api_key:
            genai.configure(api_key=api_key)
            self.model = genai.GenerativeModel('gemini-1.5-flash')
        else:
            self.model = None
    
    def generate(self, query, animal, results):
        if not self.model:
            top = results[0] if results else {}
            return {
                "disease": top.get("disease", "Unknown"),
                "confidence": "Medium" if results else "Low",
                "explanation": top.get("explanation", "No explanation available."),
                "precautions": top.get("precautions", "Consult a veterinarian."),
                "simulated": True
            }
        
        context = "\n".join([
            f"Disease: {r['disease']}, Symptoms: {r['symptoms']}, Explanation: {r['explanation']}"
            for r in results[:3]
        ])
        
        prompt = f"""User Query: {animal} with symptoms: {query}

Similar Cases:
{context}

Provide disease prediction, explanation and precautions in JSON format:
{{"disease": "...", "explanation": "...", "precautions": "...", "confidence": "High/Medium/Low"}}"""
        
        try:
            response = self.model.generate_content(prompt)
            text = response.text
            if "```json" in text:
                text = text.split("```json")[1].split("```")[0]
            elif "```" in text:
                text = text.split("```")[1].split("```")[0]
            import json
            return json.loads(text.strip())
        except Exception as e:
            print(f"LLM Error: {e}")
            top = results[0] if results else {}
            return {
                "disease": top.get("disease", "Unknown"),
                "confidence": "Medium",
                "explanation": top.get("explanation", ""),
                "precautions": top.get("precautions", "")
            }

def load_dataset(csv_path):
    df = pd.read_csv(csv_path)
    data = []
    for _, row in df.iterrows():
        data.append({
            "text": f"{row['Animal']} with symptoms: {row['Symptoms']}. Disease: {row['Disease']}",
            "animal": row['Animal'],
            "symptoms": row['Symptoms'],
            "disease": row['Disease'],
            "explanation": row['Explanation'],
            "precautions": row['Precautions']
        })
    return data

print("Loading dataset...")
data = load_dataset("data/dataset.csv")

print("Ingesting into vector store...")
store = SimpleVectorStore()
store.ingest(data)

print("\n=== AI Animal Disease Finder Demo ===")
print("Type 'quit' to exit\n")

while True:
    animal = input("Animal (e.g., Dog, Cat): ").strip()
    if animal.lower() == 'quit':
        break
    symptoms = input("Symptoms: ").strip()
    
    query = f"{animal} with symptoms: {symptoms}"
    results = store.search(query, top_k=3)
    
    rag = RAGPipeline()
    answer = rag.generate(symptoms, animal, results)
    
    print(f"\n{'='*50}")
    print(f"Disease: {answer['disease']}")
    print(f"Confidence: {answer['confidence']}")
    print(f"\nExplanation: {answer['explanation']}")
    print(f"\nPrecautions: {answer['precautions']}")
    print(f"\nTop Matches:")
    for i, r in enumerate(results, 1):
        print(f"  {i}. {r['disease']} ({r['score']:.2f})")
    print(f"{'='*50}\n")
