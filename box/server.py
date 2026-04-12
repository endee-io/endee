import os
import sys
from fastapi import FastAPI, HTTPException, Query
from pydantic import BaseModel
from fastapi.middleware.cors import CORSMiddleware
from typing import Optional, List, Dict

# Add parent to path for relative imports
sys.path.append(os.path.dirname(os.path.dirname(__file__)))
sys.path.append(os.path.dirname(os.path.dirname(__file__)))
from box.intelligence import BoxIntelligence, DeveloperAgent, BoxMemory
from box.tasks import TaskManager

app = FastAPI(title="Box Enterprise Engine API")

# Enable CORS for IDE extensions
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# Global instances
intel = BoxIntelligence()
memory = BoxMemory()
tasks = TaskManager()
agent = DeveloperAgent(
    api_key=os.getenv("OPENAI_API_KEY"),
    base_url=os.getenv("LOCAL_AI_BASE_URL")
)

class ChatRequest(BaseModel):
    query: str
    top_k: Optional[int] = 5
    index: Optional[str] = "box_codebase"
    filter: Optional[Dict] = None
    hybrid: Optional[bool] = True

class MemoryRequest(BaseModel):
    observation: str
    metadata: Optional[Dict] = None

class DevelopRequest(BaseModel):
    instruction: str
    file_path: str
    file_content: Optional[str] = ""

class TaskRequest(BaseModel):
    goal: str
    context: Optional[str] = None

class TaskUpdateRequest(BaseModel):
    status: str
    message: str

@app.post("/index")
async def trigger_index():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        intel.index_root(repo_root)
        return {"status": "success", "message": f"Hybrid indexing complete: {repo_root}"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/chat")
async def chat(req: ChatRequest):
    # Perform Hybrid Semantic Search
    results = intel.search(
        req.query, 
        top_k=req.top_k, 
        index_name=req.index, 
        filter_dict=req.filter,
        hybrid=req.hybrid
    )
    context = "\n\n".join([r["meta"]["text"] for r in results])
    
    # Query Agent memory for context
    memory_recall = memory.recall(req.query, top_k=1)
    
    if agent.client:
        response = agent.develop(
            f"Query: {req.query}. Memory: {memory_recall}", 
            context
        )
        return {"response": response, "sources": results, "memory": memory_recall}
    else:
        return {"response": "Hybrid matches found (AI disabled).", "sources": results}

@app.post("/memory/remember")
async def remember(req: MemoryRequest):
    doc_id = memory.remember(req.observation, req.metadata)
    return {"status": "success", "doc_id": doc_id}

@app.post("/develop")
async def develop(req: DevelopRequest):
    results = intel.search(req.instruction, top_k=5, hybrid=True)
    context = "\n\n".join([r["meta"]["text"] for r in results])
    
    try:
        suggestion = agent.develop(req.instruction, context, req.file_content)
        return {"suggestion": suggestion, "context_used": [r["meta"]["path"] for r in results]}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.get("/health")
async def health():
    return {"status": "ok", "engine": "Box Enterprise"}

@app.post("/tasks/create")
async def create_task(req: TaskRequest):
    tid = tasks.create_task(req.goal, req.context)
    return {"status": "success", "task_id": tid}

@app.get("/tasks")
async def list_tasks(status: Optional[str] = None):
    return {"tasks": tasks.list_tasks(status)}

@app.post("/tasks/{task_id}/update")
async def update_task(task_id: str, req: TaskUpdateRequest):
    tasks.update_task(task_id, req.status, req.message)
    return {"status": "success"}

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
