from fastapi import FastAPI
from rag_app.ingestion import ingest_pdf
from rag_app.rag_pipeline import generate_answer

app = FastAPI()

@app.post("/upload")
def upload_document(file_path: str):
    ingest_pdf(file_path)
    return {"message": "Document ingested successfully"}

@app.get("/ask")
def ask_question(query: str):
    answer = generate_answer(query)
    return {"answer": answer}