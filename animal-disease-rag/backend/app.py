from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import List, Optional
import uvicorn
from endee_client import EndeeClient
from embeddings import EmbeddingModel
from rag_pipeline import RAGPipeline
from dataset_loader import search_similar_cases
import config

app = FastAPI(title="AI Animal Disease Finder", 
              description="RAG-powered animal disease prediction using Endee vector database")

endee_client = None
embedding_model = None
rag_pipeline = None
initialized = False

class PredictRequest(BaseModel):
    animal: str
    symptoms: str

class PredictionResponse(BaseModel):
    disease: str
    confidence: str
    explanation: str
    precautions: str
    top_matches: List[dict]

class HealthResponse(BaseModel):
    status: str
    endee_connected: bool
    index_exists: bool

@app.on_event("startup")
async def startup_event():
    global endee_client, embedding_model, rag_pipeline, initialized
    
    try:
        endee_client = EndeeClient()
        embedding_model = EmbeddingModel()
        rag_pipeline = RAGPipeline()
        
        if not endee_client.health_check():
            print("Warning: Endee server not reachable at", config.ENDEE_URL)
        else:
            print("Connected to Endee server")
            
        initialized = True
    except Exception as e:
        print(f"Initialization error: {e}")
        initialized = False

@app.get("/", response_model=HealthResponse)
async def health_check():
    endee_connected = False
    index_exists = False
    
    if endee_client:
        try:
            endee_connected = endee_client.health_check()
            index_exists = endee_client.index_exists() if endee_connected else False
        except:
            pass
    
    return HealthResponse(
        status="running" if initialized else "initializing",
        endee_connected=endee_connected,
        index_exists=index_exists
    )

@app.post("/predict", response_model=PredictionResponse)
async def predict_disease(request: PredictRequest):
    if not initialized:
        raise HTTPException(status_code=503, detail="Service not initialized")
    
    if not endee_client or not embedding_model:
        raise HTTPException(status_code=503, detail="Endee client not available")
    
    if not request.animal or not request.symptoms:
        raise HTTPException(status_code=400, detail="Animal and symptoms are required")
    
    try:
        similar_cases = search_similar_cases(
            query=request.symptoms,
            animal=request.animal,
            endee_client=endee_client,
            embedding_model=embedding_model,
            top_k=config.TOP_K
        )
        
        if not similar_cases:
            return PredictionResponse(
                disease="Unknown",
                confidence="Low",
                explanation="No similar cases found. Please consult a veterinarian.",
                precautions="Seek professional veterinary help",
                top_matches=[]
            )
        
        rag_result = rag_pipeline.generate_explanation(
            query=request.symptoms,
            animal=request.animal,
            similar_cases=similar_cases
        )
        
        top_matches = [
            {
                "disease": case["disease"],
                "symptoms": case["symptoms"],
                "score": round(case["score"], 3)
            }
            for case in similar_cases
        ]
        
        return PredictionResponse(
            disease=rag_result.get("disease", "Unknown"),
            confidence=rag_result.get("confidence", "Low"),
            explanation=rag_result.get("explanation", ""),
            precautions=rag_result.get("precautions", ""),
            top_matches=top_matches
        )
        
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Prediction error: {str(e)}")

@app.post("/ingest")
async def ingest_data(csv_path: str = "data/dataset.csv"):
    if not initialized:
        raise HTTPException(status_code=503, detail="Service not initialized")
    
    if not endee_client or not embedding_model:
        raise HTTPException(status_code=503, detail="Endee client not available")
    
    try:
        from dataset_loader import ingest_dataset
        result = ingest_dataset(csv_path, endee_client, embedding_model)
        return result
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Ingestion error: {str(e)}")

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)
