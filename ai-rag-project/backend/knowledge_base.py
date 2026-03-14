from sentence_transformers import SentenceTransformer
from pypdf import PdfReader
import numpy as np
from backend.vector_store import VectorStore

model = SentenceTransformer("all-MiniLM-L6-v2")

vector_db = None


def load_pdf(file_path):

    global vector_db

    reader = PdfReader(file_path)

    text = ""

    for page in reader.pages:
        text += page.extract_text()

    chunks = text.split("\n")

    chunks = [c.strip() for c in chunks if len(c) > 40]

    embeddings = model.encode(chunks)

    dimension = embeddings.shape[1]

    vector_db = VectorStore(dimension)

    vector_db.add(embeddings, chunks)


def retrieve(query):

    query_vec = model.encode([query])

    results = vector_db.search(query_vec)

    return results