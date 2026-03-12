from fastapi import FastAPI, UploadFile, File
from app.resume_parser import extract_text_from_pdf, extract_skills
from app.generator import generate_interview_set

app = FastAPI()

@app.post("/generate-from-resume")
async def process_resume(file: UploadFile = File(...)):
    # 1. Extract text
    text = extract_text_from_pdf(file.file)
    
    # 2. Identify skills
    skills = extract_skills(text)
    
    # 3. Search Vector DB
    questions = generate_interview_set(skills)
    
    return {
        "filename": file.filename,
        "detected_skills": skills,
        "interview_prep": questions
    }