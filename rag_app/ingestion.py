from pypdf import PdfReader
from rag_app.embedding import generate_embedding
from rag_app.vector_store import add_vector

def chunk_text(text, chunk_size=500):
    chunks = []
    for i in range(0, len(text), chunk_size):
        chunks.append(text[i:i+chunk_size])
    return chunks

def ingest_pdf(file_path):
    reader = PdfReader(file_path)
    full_text = ""

    for page in reader.pages:
        full_text += page.extract_text()

    chunks = chunk_text(full_text)

    for i, chunk in enumerate(chunks):
        embedding = generate_embedding(chunk)
        add_vector(f"chunk_{i}", embedding, chunk)

    print("Document ingested successfully!")