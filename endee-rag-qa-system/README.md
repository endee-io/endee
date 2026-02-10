
# RAG-Based Question Answering using Endee

## Problem Statement
Traditional LLMs cannot answer questions from private documents. This project solves that using Retrieval Augmented Generation (RAG).

## Use Case
Question answering over custom documents using vector similarity search.

## Tech Stack
- Python
- Endee (Vector Database)
- Sentence Transformers
- OpenAI API

## Why Endee?
Endee is used as the vector database to store and retrieve embeddings efficiently, enabling fast semantic search.

## Architecture
1. Documents are converted into embeddings
2. Stored in Endee vector database
3. User question is embedded
4. Similar documents retrieved
5. LLM generates final answer

## How to Run
```bash
pip install -r requirements.txt
python src/ingest.py
python src/rag.py
