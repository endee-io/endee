# 📄 Resume-to-Interview Question Generator
### Powered by the Endee Vector Database

![Python](https://img.shields.io/badge/Python-3.10%2B-blue)
![VectorDB](https://img.shields.io/badge/VectorDB-Endee-orange)
![Framework](https://img.shields.io/badge/Framework-FastAPI-green)
![Frontend](https://img.shields.io/badge/UI-Streamlit-red)

---

# 📖 Project Overview

## The Problem
Preparing for technical interviews can be overwhelming. Candidates often struggle to identify which interview questions correspond to the technologies listed on their resumes.

Traditional question banks:
- Are generic
- Contain thousands of unrelated questions
- Lack personalization based on a candidate's skills

---

## The Solution

This project builds an **AI-powered Interview Preparation Engine** that:

1. Parses a candidate's **resume (PDF)**
2. Detects **technical skills**
3. Uses **semantic vector search**
4. Retrieves **relevant interview questions**

This system uses a **Retrieval-Augmented Generation (RAG) architecture powered by Endee Vector Database**.

---

# 🏗️ System Architecture

```mermaid
flowchart TD
    A[Resume PDF Upload] --> B[Resume Parser]
    B --> C[Skill Extraction]
    C --> D[Embedding Model]
    D --> E[Endee Vector Database]
    E --> F[Semantic Similarity Search]
    F --> G[Top-K Relevant Questions]
    G --> H[Streamlit UI Display]
```

## ⚙️ System Pipeline

### 1️⃣ Ingestion Phase

A dataset of interview questions is converted into vector embeddings.

```python
from sentence_transformers import SentenceTransformer

model = SentenceTransformer("all-MiniLM-L6-v2")
```

Each question becomes a **384-dimensional vector** and is stored in the **Endee Vector Database**.

---

### 2️⃣ Resume Processing

The user uploads a **PDF resume**.

```python
from pypdf import PdfReader

reader = PdfReader("resume.pdf")
text = ""

for page in reader.pages:
    text += page.extract_text()
```

Skills are extracted using **keyword matching and regex patterns**.

---

### 3️⃣ Semantic Retrieval

Extracted skills are converted into embeddings and searched in Endee.

```python
results = db.search(query_vector, top_k=3)
```

This retrieves **semantically similar interview questions**.

---

### 4️⃣ Presentation Layer

Questions are displayed in a **Streamlit interface** grouped by skill.

Example output:

```
Skill: React

• What is Virtual DOM?
• Explain React Hooks.
• How does React diffing algorithm work?
```

---

# 🛠️ Technical Stack

| Component | Technology |
|----------|------------|
| Vector Database | Endee |
| Embedding Model | sentence-transformers |
| Backend | FastAPI |
| Frontend | Streamlit |
| PDF Parsing | PyPDF |
| Programming Language | Python |

---

# ⚡ How Endee is Used in This Project

Endee acts as the **semantic search engine** powering this application.  
Instead of relying on keyword-based search, the system converts interview questions and resume skills into **vector embeddings** and performs **similarity search** to retrieve the most relevant interview questions.

---

## 1️⃣ Vector Embedding Creation

Interview questions are converted into embeddings using a transformer model.

```python
from sentence_transformers import SentenceTransformer

model = SentenceTransformer("all-MiniLM-L6-v2")
embedding = model.encode(question_text).tolist()
```

Each embedding captures the **semantic meaning** of the question.

---

## 2️⃣ Storing Vectors in Endee

Embeddings are stored in Endee along with metadata such as the skill and question text.

```python
from endee import Client

client = Client()
index = client.get_or_create_index("interview_vectors")

index.upsert(
    id=str(i),
    vector=embedding,
    metadata={
        "skill": item["skill"],
        "question": item["question"]
    }
)
```

Endee indexes these vectors to enable **fast semantic retrieval**.

---

## 3️⃣ Semantic Similarity Search

When a resume is uploaded, detected skills are converted into embeddings and used as query vectors.

```python
query_vector = model.encode(skill).tolist()

results = index.search(
    vector=query_vector,
    top_k=3
)
```

Endee compares the query vector with stored vectors using **cosine similarity** and returns the **most relevant interview questions**.

---

## 4️⃣ Metadata Retrieval

Each stored vector contains metadata that allows the system to display structured results.

Example response:

```
{
  "skill": "React",
  "question": "Explain React Hooks"
}
```

This eliminates the need for additional database lookups.

---

## 5️⃣ Why Endee Was Chosen

Endee provides several advantages for this application:

- Semantic search instead of keyword matching
- High-performance vector indexing
- Fast similarity retrieval
- Scalable architecture for large datasets

These features make Endee ideal for building **AI-powered retrieval systems such as interview preparation assistants**.

---

# ✅ Internship Challenge Requirements Checklist

The following steps were completed as part of the Endee internship evaluation process:

- [x] Starred the Endee GitHub repository
- [x] Forked the Endee repository
- [x] Built an AI project using Endee as the vector database
- [x] Implemented semantic search using vector embeddings
- [x] Hosted the complete project on GitHub
- [x] Provided setup and execution instructions in the README

# 📂 Project Structure

```
resume-interview-generator/

├── app/
│   ├── main.py            # FastAPI backend API
│   ├── ui.py              # Streamlit frontend UI
│   ├── embed_store.py     # Script to seed Endee with vectors
│   ├── vector_search.py   # Endee semantic search logic
│   ├── resume_parser.py   # Resume PDF parsing
│   └── generator.py       # Orchestration pipeline
│
├── data/
│   └── questions.json     # Interview questions dataset
│
├── requirements.txt
└── README.md
```

---

# 🚀 Setup & Execution

### 1️⃣ Start Endee Server

```bash
./endee.exe
```

---

### 2️⃣ Install Dependencies

```bash
pip install -r requirements.txt
```

---

### 3️⃣ Seed the Vector Database

Convert interview questions into embeddings and store them in Endee.

```bash
python -m app.embed_store
```

---

### 4️⃣ Start Backend API

```bash
python -m uvicorn app.main:app --reload
```

---

### 5️⃣ Start Frontend

```bash
python -m streamlit run app/ui.py
```

---

# 📊 Demo

### Upload Resume

Upload your **PDF resume** and the system automatically detects skills.

```
Detected Skills:
Python, React, SQL
```

---

### Generated Questions

```
React
- What is Virtual DOM?
- Explain React Hooks.

Python
- What is a Python decorator?
- Explain list vs tuple.
```

---

# 🔌 API Documentation

FastAPI automatically generates **interactive API documentation**.

Open in browser:

```
http://127.0.0.1:8000/docs
```

Example endpoint:

```
POST /upload-resume
```

Upload a PDF and receive:

- detected skills  
- recommended interview questions  

---

# 🎯 Future Improvements

### 🤖 LLM Integration
Generate **sample answers** using an LLM.

### 🎙 Mock Interview Mode
Use **Text-to-Speech** to simulate real interviews.

### 🧠 Smarter Skill Extraction
Implement **Named Entity Recognition (NER)**.

### 📊 Difficulty Levels
Allow filtering questions by:

- Beginner  
- Intermediate  
- Advanced  

---

# 👨‍💻 Developer Note

This project was built as part of the **Endee Internship Challenge**.

It demonstrates practical experience with:

- Vector Databases  
- Semantic Search  
- Retrieval-Augmented Generation (RAG)  
- FastAPI Backend Development  
- Streamlit AI Interfaces

  
