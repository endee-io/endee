import os
import google.generativeai as genai
from dotenv import load_dotenv
from rag_app.retriever import search_similar

load_dotenv()
genai.configure(api_key=os.getenv("GOOGLE_API_KEY"))

model = genai.GenerativeModel("gemini-1.5-flash")

def generate_answer(query):
    contexts = search_similar(query)

    combined_context = "\n".join(contexts)

    prompt = f"""
    You are a helpful research assistant.
    Answer the question using ONLY the context below.

    Context:
    {combined_context}

    Question:
    {query}
    """

    response = model.generate_content(prompt)
    return response.text