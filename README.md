🚀 Endee RAG Chatbot

An AI-powered chatbot built using FastAPI, JavaScript, and Groq LLM, demonstrating a Retrieval-Augmented Generation (RAG) workflow with a vector database concept (Endee).

🌐 Live Demo

🔗 https://endee-rag-chatbot-1.onrender.com

📌 Project Overview

This project implements a RAG-based AI chatbot that enhances response accuracy by retrieving relevant information from a knowledge base before generating answers.

Instead of relying solely on the language model, the system:

Converts user queries into embeddings

Performs semantic search

Retrieves relevant knowledge chunks

Generates responses using contextual data

🧠 Key Concepts

🔹 RAG (Retrieval-Augmented Generation)

🔹 Semantic Search using Embeddings

🔹 Vector Database Concept (Endee)

🔹 Context-aware AI Responses

🛠️ Tech Stack
🔹 Frontend

HTML5

CSS3 (Modern UI, responsive design)

JavaScript (Fetch API, DOM manipulation)

🔹 Backend

FastAPI (Python)

Uvicorn

🔹 AI & ML

Groq API (LLM)

Sentence Transformers (Embeddings)

🔹 Deployment

Backend: Render

Version Control: GitHub

⚙️ How It Works
User Query
   ↓
Convert to Embeddings
   ↓
Semantic Search (Vector DB / Endee Concept)
   ↓
Retrieve Relevant Chunks
   ↓
Send Context + Query to LLM
   ↓
Generate Context-Aware Response
✨ Features

✅ AI chatbot with real-time responses
✅ Retrieval-based answer generation (RAG)
✅ Semantic search using embeddings
✅ Context display (retrieved chunks)
✅ Clean and modern UI
✅ Deployed backend (cloud-ready)
✅ Error handling and fallback responses

📁 Project Structure
project-root/
│
├── backend/
│   ├── main.py
│   ├── knowledge_base.py
│   ├── requirements.txt
│
├── frontend/
│   ├── index.html
│   ├── style.css
│   ├── script.js
│
└── README.md
🚀 Setup Instructions
1️⃣ Clone Repository
git clone https://github.com/your-username/your-repo-name.git
cd your-repo-name
2️⃣ Backend Setup
cd backend
pip install -r requirements.txt

Create .env file:

GROQ_API_KEY=your_api_key_here

Run backend:

uvicorn main:app --reload
3️⃣ Frontend Setup

Open index.html in browser
or deploy using Netlify / GitHub Pages

🔑 Environment Variables
Variable	Description
GROQ_API_KEY	Groq API key for LLM
🧪 Example Queries

Try:

What is Retrieval-Augmented Generation?

What is Endee?

How does vector search improve chatbot answers?

⚠️ Limitations

Knowledge is limited to the predefined dataset

Responses depend on retrieval accuracy

No persistent database (in-memory storage)

All users share the same knowledge base

🚀 Future Enhancements

Persistent vector database integration (Endee full implementation)

User authentication & session-based memory

Document upload support

Real-time streaming responses

Advanced ranking algorithms

🙏 Acknowledgement

Groq API for LLM support

FastAPI for backend framework

Sentence Transformers for embeddings

ChatGPT for development assistance

📢 Conclusion

This project successfully demonstrates a RAG-based AI system where responses are generated using retrieved context from a knowledge base, showcasing the practical use of vector databases (Endee concept) in modern AI applications.

👨‍💻 Author

Belics B
Full-Stack Developer | AI Enthusiast 🚀