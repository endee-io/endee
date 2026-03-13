<p align="center">
  <img src="https://endee.io/favicon.png" width="120">
</p>

# 📄 PDF RAG Application using Endee Vector Database

A **production-style Retrieval-Augmented Generation (RAG) application** that allows users to ask questions about any PDF document.

The system converts PDF content into vector embeddings, stores them in the **Endee vector database**, retrieves the most relevant sections, and uses a **Groq LLM** to generate accurate answers.

---

# 🧠 Problem Statement

Large PDF documents often contain valuable information, but manually searching through them is inefficient.

Traditional keyword search systems also struggle because they:

* Miss **semantic meaning**
* Fail to capture **context**
* Retrieve irrelevant results

This project solves these issues by:

* Converting PDF text into **semantic embeddings**
* Storing them in the **Endee vector database**
* Retrieving **contextually relevant chunks**
* Generating answers using an **LLM with retrieved context**

---

# 🏗️ System Architecture

## Indexing Phase (Runs Once per Document)

```
PDF File
   ↓
PyMuPDF (Text Extraction)
   ↓
Text Chunking (Overlapping Chunks)
   ↓
SentenceTransformer Embeddings (384-dim)
   ↓
Endee Vector Database
(Cosine Similarity, INT8 Precision)
```

## Query Phase (Runs for Every Question)

```
User Question
   ↓
SentenceTransformer Embedding
   ↓
Endee Vector Search (Top-K Similar Chunks)
   ↓
LangChain Prompt Template
   ↓
Groq LLM
   ↓
Generated Answer
```

---

# 🧰 Tech Stack

| Component       | Technology                               |
| --------------- | ---------------------------------------- |
| Vector Database | Endee (self-hosted via Docker)           |
| Embeddings      | sentence-transformers / all-MiniLM-L6-v2 |
| PDF Parsing     | PyMuPDF (fitz)                           |
| LLM             | Groq — llama3-8b-8192                    |
| Orchestration   | LangChain (LCEL chains)                  |
| Language        | Python 3.11                              |

---

# 🔍 How Endee is Used

Endee acts as the **core vector storage and retrieval engine** in this project.

### Index Creation

A cosine similarity index is created in Endee with:

* **Dimension:** 384
* **Similarity Metric:** Cosine
* **Precision:** INT8

### Upsert Operation

Each PDF chunk is:

1. Converted into an embedding
2. Stored in Endee along with metadata:

* chunk id
* original text

### Query Operation

When a user asks a question:

1. The question is embedded into a vector
2. Endee performs a **cosine similarity search**
3. The **Top 4 most relevant chunks** are retrieved

### Context Assembly

The retrieved chunks are combined into a **context block**, which is passed to the **Groq LLM** to generate the final answer.

Endee runs locally as a **Docker container on `localhost:8080`**, making the entire pipeline **self-hosted without relying on external vector database services**.

---

# ⚙️ Setup & Execution

## Prerequisites

* Python **3.10+**
* Docker Desktop
* Groq API Key
  https://console.groq.com

---

# 1️⃣ Clone the Repository

```
git clone https://github.com/YOUR_USERNAME/endee
cd endee
```

---

# 2️⃣ Install Dependencies

```
pip install -r requirements.txt
```

---

# 3️⃣ Start the Endee Server

```
docker compose up -d
```

Endee will start at:

```
http://localhost:8080
```

---

# 4️⃣ Configure the Notebook

Open **rag.ipynb** and update the configuration cell:

```
GROQ_API_KEY = "gsk_your_actual_key_here"
PDF_PATH     = "your_document.pdf"
```

---

# 5️⃣ Run the Notebook

Run the notebook cells sequentially.

The pipeline will:

1. Extract and chunk the PDF
2. Generate embeddings
3. Store vectors in Endee
4. Configure the LangChain + Groq pipeline
5. Enable question answering over the document

---

# 6️⃣ Ask Questions

Example:

```
my_question = "What is the main topic of this document?"
answer = rag_query(my_question)
print(answer)
```

The system will retrieve relevant document chunks and generate an answer using the LLM.

---

# 📁 Project Structure

```
endee/
│
├── rag.ipynb
├── requirements.txt
├── docker-compose.yml
├── .gitignore
└── README.md
```

---

# 🔄 Useful Docker Commands

```
docker compose up -d
docker compose down
docker logs -f endee-server
```

---

# 📌 Notes

* Never commit a **real Groq API key** to GitHub.
* Endee index data persists across Docker restarts using a **named Docker volume**.
* To index a new PDF, restart the kernel and run the notebook again.
* For large documents, consider using **mixtral-8x7b**
