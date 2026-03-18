import pandas as pd
import json
from typing import List, Dict, Any
from embeddings import EmbeddingModel
from endee_client import EndeeClient

def load_dataset(csv_path: str) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    return df

def create_search_text(row: pd.Series) -> str:
    animal = row['Animal']
    symptoms = row['Symptoms']
    disease = row['Disease']
    return f"{animal} with symptoms: {symptoms}. Disease: {disease}"

def create_payload(row) -> Dict[str, Any]:
    return {
        "animal": row['Animal'],
        "symptoms": row['Symptoms'],
        "disease": row['Disease'],
        "explanation": row['Explanation'],
        "precautions": row['Precautions']
    }

def ingest_dataset(csv_path: str, endee_client: EndeeClient, embedding_model: EmbeddingModel) -> Dict[str, Any]:
    df = load_dataset(csv_path)
    
    if not endee_client.index_exists():
        endee_client.create_index(vector_size=embedding_model.get_vector_size())
    
    search_texts = [create_search_text(row) for _, row in df.iterrows()]
    
    embeddings = embedding_model.encode(search_texts)
    
    vectors = []
    for idx, (embedding, row) in enumerate(zip(embeddings, df.to_dict('records'))):
        vector_data = {
            "id": str(idx),
            "vector": embedding,
            "payload": create_payload(row)
        }
        vectors.append(vector_data)
    
    result = endee_client.add_vectors(vectors)
    return {
        "total_ingested": len(vectors),
        "result": result
    }

def search_similar_cases(query: str, animal: str, endee_client: EndeeClient, 
                         embedding_model: EmbeddingModel, top_k: int = 3) -> List[Dict]:
    search_text = f"{animal} with symptoms: {query}"
    
    query_embedding = embedding_model.encode_single(search_text)
    
    results = endee_client.search(
        vector=query_embedding,
        top_k=top_k,
        filter_animal=animal if animal.lower() != "any" else None
    )
    
    similar_cases = []
    for result in results.get("results", []):
        similar_cases.append({
            "disease": result["payload"]["disease"],
            "symptoms": result["payload"]["symptoms"],
            "explanation": result["payload"]["explanation"],
            "precautions": result["payload"]["precautions"],
            "score": result.get("score", 0.0)
        })
    
    return similar_cases
