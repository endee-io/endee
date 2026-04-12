# Endee Docs RAG Assistant

A fast, highly scalable **Retrieval-Augmented Generation (RAG)** application and **Semantic Search Engine** built with the [Endee](https://github.com/endee-io/endee) vector database.

This project demonstrates a practical, real-world AI use case by vectorizing the official Endee documentation and providing a sleek UI to query it naturally. It features:
- **Semantic Search**: Understands context to find relevant documentation efficiently.
- **RAG Generation**: Uses an LLM to generate precise answers based *only* on the matching context.
- **Dual Mode**: Fully functional offline as a Semantic Search tool natively without any API Key, seamlessly upgrading to RAG capabilities if an OpenAI key is provided.

## System Design
1. **Embedding**: Extracts text from `.md` files (the Docs) and embeds them into 384-dimensional dense vectors using the local `sentence-transformers/all-MiniLM-L6-v2` model.
2. **Indexing**: Uses the Endee Vector Database using the Python SDK `upsert` API. Endee organizes this context in highly-efficient structures enabling rapid `query` retrieval times.
3. **Frontend**: Streamlit application processes user questions, embeds the query, searches Endee, formats retrieved contexts, and prompts OpenAI (if configured).

## Prerequisites
- A running instance of the **Endee Database**.
  ```bash
  # Inside the root endee directory
  docker compose up -d
  # Or run the binary manually: NDD_DATA_DIR=./data ./build/ndd
  ```
- Make sure Endee is reachable on `localhost:8080` (default).

## Setup Instructions

1. **Install dependencies**
   ```bash
   pip install -r requirements.txt
   ```

2. **Ingest the Documentation into Endee**
   Run the ingestion script. This will read the markdown files, vectorize them, and populate the `endee_docs` index.
   ```bash
   python ingest.py
   ```

3. **Launch the UI**
   Start the interactive Streamlit assistant.
   ```bash
   streamlit run app.py
   ```

4. **Navigate to the App**  
   Open your browser and navigate to `http://localhost:8501`. 
   * **Semantic Search**: Works out of the box after ingestion. 
   * **RAG Mode**: Insert your `OpenAI_API_Key` in the sidebar to have the assistant write natural, summarized documentation responses!
