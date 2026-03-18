<p align="center">
  <picture>
      <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-dark.svg">
      <source media="(prefers-color-scheme: light)" srcset="docs/assets/logo-light.svg">
      <img height="100" alt="Endee" src="docs/assets/logo-dark.svg">
  </picture>
</p>

<p align="center">
  <b>AI Knowledge Assistant built using Endee for RAG, semantic search, and voice-enabled interaction.</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/AI-RAG-blue?style=flat-square" />
  <img src="https://img.shields.io/badge/VectorDB-Endee-green?style=flat-square" />
  <img src="https://img.shields.io/badge/Frontend-React-black?style=flat-square" />
  <img src="https://img.shields.io/badge/Backend-FastAPI-red?style=flat-square" />
</p>

---

# 🚀 Endee AI Knowledge Assistant (RAG + Voice Enabled)

An advanced AI-powered knowledge assistant built using the **Endee vector database** for semantic search, Retrieval-Augmented Generation (RAG), and voice-based interaction.

This project demonstrates a **production-ready AI/ML system** that retrieves relevant information from documents before generating accurate responses.

---

## 🧠 Project Overview

This application allows users to:

- 📄 Upload documents (PDF / TXT)
- 🔍 Perform semantic search using Endee
- 💬 Ask context-aware questions
- ⚡ Get accurate AI-generated answers using RAG

Unlike traditional chatbots, this system **retrieves relevant data first**, ensuring better accuracy and reduced hallucinations.

---

## 🏗️ System Architecture

```mermaid
flowchart TD
    A --> B[FastAPI Backend]

    C --> D[Document Processing]
    D --> E[Text Chunking]
    E --> F[Embedding Model]

    F --> G[Endee Vector Database]

    C --> H[User Query]
    H --> I[Similarity Search (Endee)]
    I --> J[Relevant Context]

    J --> K[LLM (Answer Generation)]
    K --> L[Response to User]


🔥 Key Features

✅ Retrieval-Augmented Generation (RAG)

Combines retrieval + generation for accurate answers

Uses document context instead of guessing

✅ Endee Vector Database Integration

Stores embeddings efficiently

Performs fast semantic similarity search

Scalable for large datasets


✅ Document Processing

Supports PDF and TXT files

Splits text into chunks

Generates embeddings for semantic search

✅ Premium UI/UX

Dark theme + glassmorphism design

Smooth animations

ChatGPT-like experience


🛠️ Tech Stack

Frontend

React.js

Axios


Backend

FastAPI

Uvicorn

AI / ML

Sentence Transformers / OpenAI

RAG Architecture

Vector Database

Endee

⚙️ Setup Instructions

1️⃣ Clone the Repository

git clone https://github.com/YOUR_USERNAME/endee.git
cd endee


2️⃣ Backend Setup

cd backend
python -m venv venv
venv\Scripts\activate
pip install -r requirements.txt

Run the backend:

uvicorn app.main:app --reload


3️⃣ Frontend Setup

cd frontend
npm install
npm start


▶️ How to Use

Upload a document (PDF / TXT)

System retrieves relevant data from Endee

AI generates a contextual response


💡 Why Endee?

Endee enables:

⚡ High-performance vector search

📈 Scalable AI applications

🧠 Efficient semantic retrieval

This project showcases how Endee can be used in real-world AI systems.


✅ Mandatory Requirements Completed

⭐ Starred Endee repository

🍴 Forked Endee repository

🛠️ Built project on top of fork

🤖 Implemented RAG + AI use case


🤝 Contribution

This project was developed as part of an AI/ML assignment using Endee.