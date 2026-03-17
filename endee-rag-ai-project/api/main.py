from fastapi import FastAPI, UploadFile, File, Form, HTTPException
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import uuid
import os

from api.db_client import upsert_document
from api.agent import route_and_execute
import PyPDF2

app = FastAPI(title="AI Document & Recommendation Assistant")

# Mount UI static files
os.makedirs("ui", exist_ok=True)
app.mount("/ui", StaticFiles(directory="ui"), name="ui")

class ChatRequest(BaseModel):
    message: str

@app.get("/", response_class=HTMLResponse)
async def serve_ui():
    """Serve the main UI page."""
    with open("ui/index.html", "r", encoding="utf-8") as f:
        return HTMLResponse(content=f.read())


@app.post("/api/chat")
async def chat_endpoint(req: ChatRequest):
    """Agentic Chat endpoint."""
    try:
        response = route_and_execute(req.message)
        return {"response": response}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/api/upload")
async def upload_pdf(file: UploadFile = File(...)):
    """Ingest a PDF document into the knowledge base."""
    if not file.filename.endswith(".pdf"):
         raise HTTPException(status_code=400, detail="Only PDF files are supported.")
         
    try:
        pdf_reader = PyPDF2.PdfReader(file.file)
        text = ""
        for page in pdf_reader.pages:
            text += page.extract_text() + "\n"
            
        doc_id = str(uuid.uuid4())
        # In a real app we'd chunk this text, but for demo we just store it
        upsert_document(
            collection_name="knowledge_base",
            doc_id=doc_id,
            text=text,
            metadata={"source": file.filename}
        )
        return {"message": "PDF ingested successfully! You can now ask questions about it.", "id": doc_id}
    except Exception as e:
         raise HTTPException(status_code=500, detail=f"Failed to process PDF: {str(e)}")


@app.post("/api/seed_recommendations")
async def seed_recommendations():
    """Seeds some dummy items for the recommendation functionality."""
    items = [
        {"id": "rec_1", "title": "Ergonomic Office Chair", "desc": "A comfortable mesh chair with lumbar support for long working hours."},
        {"id": "rec_2", "title": "Mechanical Keyboard", "desc": "Wireless mechanical keyboard with tactile brown switches, great for typing."},
        {"id": "rec_3", "title": "Noise Cancelling Headphones", "desc": "Over-ear headphones with active noise cancellation and 30-hour battery life."},
        {"id": "rec_4", "title": "4K Monitor", "desc": "27-inch 4K IPS monitor with vibrant colors, perfect for designers and coders."}
    ]
    
    try:
        for item in items:
            upsert_document(
                collection_name="recommendations",
                doc_id=item["id"],
                text=item["desc"],
                metadata={"title": item["title"]}
            )
        return {"message": "Recommendation Engine seeded with sample products!"}
    except Exception as e:
         raise HTTPException(status_code=500, detail=str(e))

