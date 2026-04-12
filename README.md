# Document RAG Search with FastAPI + Streamlit

A professional document question-answering system using local vector search and text generation.

## Project Overview

This project lets users upload PDF or TXT documents, indexes document chunks in a local in-memory vector store, and answers user questions by retrieving the most relevant chunks and generating responses with a local text generation model.

## Features

- Upload PDF and TXT files through the backend
- Extract text using PyPDF2
- Split documents into overlapping semantic chunks
- Generate sentence-transformer embeddings using `all-MiniLM-L6-v2`
- Store embeddings in a lightweight in-memory vector store
- Retrieve the top 5 most relevant chunks for each query
- Generate answers using a local `gpt2` model
- Streamlit frontend chat interface for file upload and Q&A

## Tech Stack

- Python
- FastAPI
- Streamlit
- sentence-transformers
- PyPDF2
- transformers
- torch
- Requests
- python-dotenv

## Architecture

The system uses a Retrieval-Augmented Generation (RAG) flow:

1. Ingestion
   - Upload a PDF or TXT document
   - Extract text and split it into overlapping chunks
   - Embed each chunk with sentence-transformers
   - Store chunk embeddings and metadata in memory
2. Retrieval
   - Embed the user query
   - Find the top 5 most relevant chunks by similarity
   - Build a contextual prompt from the retrieved chunks
3. Generation
   - Send the prompt to a local text generation model
   - Return a generated answer with source references

## Project Structure

```
ai-search-project/
│
├── app/
│   ├── main.py
│   ├── routes.py
│   ├── services/
│   │   ├── embedding.py
│   │   ├── vector_store.py
│   │   ├── retrieval.py
│   │   ├── rag.py
│   ├── utils/
│   │   ├── file_loader.py
│
├── ui/
│   ├── streamlit_app.py
│
├── requirements.txt
├── README.md
├── .env
```

## Setup

1. Install dependencies:

```bash
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

2. Create or update `.env` with your environment settings:

```text
BACKEND_URL=http://127.0.0.1:8000
HF_MODEL_NAME=gpt2
EMBEDDING_MODEL=all-MiniLM-L6-v2
```

3. Run the FastAPI backend:

```bash
uvicorn app.main:app --reload
```

4. Run the Streamlit frontend:

```bash
streamlit run ui/streamlit_app.py
```

## Example Usage

### Upload a document

```bash
curl -X POST "http://127.0.0.1:8000/upload" \
  -F "file=@./example.pdf"
```

### Ask a question

```bash
curl -X POST "http://127.0.0.1:8000/query" \
  -H "Content-Type: application/json" \
  -d '{"question": "What is the main idea of the document?", "top_k": 5}'
```

### Run the UI

```bash
streamlit run ui/streamlit_app.py
```

Open the Streamlit app in the browser, upload a document, ask a question, and view the answer.

## Notes

- Ensure the backend is running before using the Streamlit interface.
- The backend reads `.env` automatically via `python-dotenv`.
- The system uses local model weights and does not require external APIs.

## Example Usage

### Upload a document

```bash
curl -X POST "http://127.0.0.1:8000/upload" \
  -F "file=@./example.pdf"
```

### Ask a question

```bash
curl -X POST "http://127.0.0.1:8000/query" \
  -H "Content-Type: application/json" \
  -d '{"question": "What is the main idea of the document?", "top_k": 5}'
```

### Run the UI

```bash
streamlit run ui/streamlit_app.py
```

Open the Streamlit app in the browser, upload a document, ask a question, and view the answer.

## Notes

- Ensure Endee is reachable at the URL configured in `.env`.
- The backend reads `.env` automatically via `python-dotenv`.
- If you use an Endee auth token, set it in `ENDEE_API_KEY`.
