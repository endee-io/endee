import logging
import os

from fastapi import FastAPI, HTTPException
from backend.retriever import load_knowledge, retrieve
import openai

logger = logging.getLogger(__name__)
app = FastAPI()

openai_api_key = os.getenv("OPENAI_API_KEY")
if openai_api_key:
    openai.api_key = openai_api_key


@app.on_event("startup")
def startup_event():
    try:
        load_knowledge()
    except Exception:
        logger.exception("Failed to load knowledge base")


@app.get("/")
def home():

    return {"status": "Endee-style RAG assistant ready"}


@app.get("/ask")

def ask(question: str):

    if not openai_api_key:
        # Return a harmless response so the frontend can render an error message
        # instead of showing "Backend not running" when no API key is configured.
        return {
            "answer": "OPENAI_API_KEY is not set. Set the environment variable to enable OpenAI API calls.",
            "sources": [],
        }

    try:
        docs = retrieve(question)
    except Exception as e:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to retrieve knowledge: {e}",
        ) from e

    context = "\n".join(docs)

    prompt = f"""
    Use the following knowledge to answer the question.

    Knowledge:
    {context}

    Question:
    {question}

    Provide a clear answer and reference the knowledge used.
    """

    try:
        response = openai.ChatCompletion.create(
            model="gpt-4o-mini",
            messages=[{"role": "user", "content": prompt}]
        )
    except Exception as e:
        raise HTTPException(status_code=502, detail=str(e)) from e

    answer = response["choices"][0]["message"]["content"]

    return {
        "answer": answer,
        "sources": docs
    }