from fastapi import FastAPI, UploadFile
import uuid
import os

from pdf_loader import load_pdf
from embeddings import create_embedding
from vector_store import store_vector
from rag import generate_answer

app = FastAPI()

UPLOAD_DIR = "data"
os.makedirs(UPLOAD_DIR, exist_ok=True)


@app.post("/upload")
async def upload(file: UploadFile):

    path = f"{UPLOAD_DIR}/{file.filename}"

    with open(path, "wb") as f:
        f.write(await file.read())

    text = load_pdf(path)
    chunks = text.split(". ")

    for chunk in chunks:
        if len(chunk.strip()) == 0:
            continue

        embedding = create_embedding(chunk)
        store_vector(str(uuid.uuid4()), embedding, chunk)

    return {"status":"uploaded"}


@app.get("/ask")
def ask(question: str):

    answer = generate_answer(question)

    return {"answer":answer}