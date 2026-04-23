from fastapi import FastAPI, UploadFile
import shutil
import os
import google.generativeai as genai
from pypdf import PdfReader
from dotenv import load_dotenv

load_dotenv()

genai.configure(api_key=os.getenv("GEMINI_API_KEY"))
model = genai.GenerativeModel("gemini-pro")

app = FastAPI()

PDF_TEXT = ""

@app.post("/upload/")
async def upload_pdf(file: UploadFile):
    global PDF_TEXT

    with open(file.filename, "wb") as buffer:
        shutil.copyfileobj(file.file, buffer)

    reader = PdfReader(file.filename)
    text = ""

    for page in reader.pages:
        text += page.extract_text()

    PDF_TEXT = text

    return {"message": "PDF uploaded and processed"}

@app.get("/ask/")
def ask_question(query: str):
    global PDF_TEXT

    prompt = f"""
    Answer the question based on this PDF content:

    {PDF_TEXT[:3000]}

    Question: {query}
    """

    response = model.generate_content(prompt)

    return {"answer": response.text}
