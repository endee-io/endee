from fastapi import APIRouter, UploadFile, File, Form, HTTPException
from app.rag_pipeline import RAGPipeline

router = APIRouter()
pipeline = RAGPipeline()


@router.post("/ingest")
async def ingest_document(
    file: UploadFile = File(None),
    text: str = Form(None),
    doc_name: str = Form("untitled_document")
):
    """
    Ingest a document into Endee.
    Accepts either:
      - multipart PDF file upload (file field)
      - plain text via form field (text field)
    """
    if file and file.filename:
        if not file.filename.endswith(".pdf"):
            raise HTTPException(status_code=400, detail="Only PDF files are supported")
        content = await file.read()
        doc_name = doc_name or file.filename.replace(".pdf", "")
        result = pipeline.ingest_pdf(content, doc_name)
    elif text:
        result = pipeline.ingest_text(text, doc_name)
    else:
        raise HTTPException(status_code=400, detail="Provide either a PDF file or text content")

    if result["status"] == "error":
        raise HTTPException(status_code=422, detail=result["message"])

    return result
