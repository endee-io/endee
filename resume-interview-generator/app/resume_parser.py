from pypdf import PdfReader
import re

def extract_text_from_pdf(pdf_file):
    reader = PdfReader(pdf_file)
    text = ""
    for page in reader.pages:
        content = page.extract_text()
        if content:
            text += content
    return text

def extract_skills(text):
    # A simple keyword-based extraction for the MVP.
    # In a production app, you'd use an LLM or a library like Spacy.
    common_skills = ["Java", "Python", "React", "MySQL", "Spring Boot", "Node.js", "Docker", "AWS"]
    found_skills = []
    
    for skill in common_skills:
        if re.search(rf"\b{skill}\b", text, re.IGNORECASE):
            found_skills.append(skill)
            
    return list(set(found_skills))