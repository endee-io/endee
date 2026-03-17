from fastapi import FastAPI
from pydantic import BaseModel
from rag_pipeline import generate_answer

app = FastAPI()

class Question(BaseModel):
    question: str
    context: str = ""

@app.post("/ask")
def ask_question(data: Question):

    answer = generate_answer(data.question, data.context)

    return {"answer": answer}