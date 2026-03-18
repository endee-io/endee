SAMPLE_DOCS = [

# 🔹 RAG
{
"title": "RAG",
"content": """
Retrieval-Augmented Generation (RAG) is an AI architecture that combines retrieval and generation.
Instead of answering directly, the system first retrieves relevant information from a knowledge base.
Then it uses a language model to generate a response based on that retrieved data.
RAG improves accuracy, reduces hallucination, and makes responses more reliable.
"""
},

# 🔹 VECTOR SEARCH
{
"title": "Vector Search",
"content": """
Vector search converts text into numerical embeddings.
These embeddings represent the meaning of the text.
Instead of matching keywords, vector search finds similar meanings.
It is used in semantic search, recommendation systems, and chatbots.
Vector search helps retrieve relevant context quickly and accurately.
"""
},

# 🔹 ENDEE
{
"title": "Endee",
"content": """
Endee is a vector database used in this project.

Purpose of Endee:
- Store embeddings of text data
- Perform similarity search
- Retrieve relevant information for user queries

In this project:
1. User query is converted into embeddings
2. Endee searches similar embeddings
3. Relevant chunks are retrieved
4. These chunks are passed to the AI model
5. The AI generates answers based on retrieved context

This enables Retrieval-Augmented Generation (RAG).
"""
},

# 🔹 PROJECT ARCHITECTURE
{
"title": "Project Architecture",
"content": """
This project follows a RAG architecture.
Step 1: User sends a query.
Step 2: Query is converted into embeddings.
Step 3: Vector database (Endee concept) retrieves relevant chunks.
Step 4: Retrieved context is sent to the LLM.
Step 5: LLM generates an answer based only on that context.
"""
},

# 🔹 CHATBOT SPEED
{
"title": "Performance Optimization",
"content": """
Chatbot response speed can be improved by:
1. Using efficient vector search
2. Reducing API latency
3. Using caching for repeated queries
4. Optimizing backend processing
5. Using faster LLM models
6. Minimizing unnecessary computations
"""
},

# 🔹 SEMANTIC SEARCH
{
"title": "Semantic Search",
"content": """
Semantic search focuses on understanding meaning instead of exact keywords.
It uses embeddings to capture context.
This allows the system to return relevant answers even if the words are different.
It improves chatbot accuracy and user experience.
"""
},

# 🔹 LIMITATIONS
{
"title": "Limitations",
"content": """
RAG systems depend on the quality of the knowledge base.
If the information is not present, the system cannot answer.
They also require proper chunking and embedding strategies.
Performance depends on vector search efficiency.
"""
}

]