from fastapi import APIRouter, UploadFile, File
from app.services.embedding import chunk_text, get_embeddings, store_chunks

import fitz  # PyMuPDF
from docx import Document

router = APIRouter()


def extract_text_from_pdf(file_bytes):
    text = ""
    pdf = fitz.open(stream=file_bytes, filetype="pdf")

    for page in pdf:
        text += page.get_text()

    return text


def extract_text_from_docx(file_bytes):
    from io import BytesIO
    doc = Document(BytesIO(file_bytes))
    return "\n".join([para.text for para in doc.paragraphs])
    

@router.post("/upload")
async def upload_file(file: UploadFile = File(...)):
    content = await file.read()

    # 🔥 Detect file type
    if file.filename.endswith(".pdf"):
        text = extract_text_from_pdf(content)

    elif file.filename.endswith(".docx"):
        text = extract_text_from_docx(content)

    else:
        # default: txt or unknown
        try:
            text = content.decode("utf-8")
        except UnicodeDecodeError:
            text = content.decode("latin-1")

    # Process text
    chunks = chunk_text(text)
    embeddings = get_embeddings(chunks)
    store_chunks(chunks, embeddings)

    return {"message": f"{file.filename} processed successfully"}