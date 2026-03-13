<p align="center">
  <picture>
      <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-dark.svg">
      <source media="(prefers-color-scheme: light)" srcset="docs/assets/logo-light.svg">
      <img height="100" alt="Endee" src="docs/assets/logo-dark.svg">
  </picture>
</p>

<p align="center">
    <b>High-performance open-source vector database for AI search, RAG, semantic search, and hybrid retrieval.</b>
</p>

<p align="center">
    <a href="./docs/getting-started.md"><img src="https://img.shields.io/badge/Quick_Start-Local_Setup-success?style=flat-square" alt="Quick Start"></a>
    <a href="https://docs.endee.io/quick-start"><img src="https://img.shields.io/badge/Docs-Quick_Start-success?style=flat-square" alt="Docs"></a>
    <a href="https://github.com/endee-io/endee/blob/master/LICENSE"><img src="https://img.shields.io/github/license/endee-io/endee?style=flat-square" alt="License"></a>
    <a href="https://discord.gg/5HFGqDZQE3"><img src="https://img.shields.io/badge/Discord-Join_Chat-5865F2?logo=discord&style=flat-square" alt="Discord"></a>
    <a href="https://endee.io/"><img src="https://img.shields.io/badge/Website-Endee-111111?style=flat-square" alt="Website"></a>
    <!-- <a href="https://endee.io/benchmarks"><img src="https://img.shields.io/badge/Benchmarks-Coming_Soon-1F8B4C?style=flat-square" alt="Benchmarks"></a> -->
    <!-- <a href="https://endee.io/cloud"><img src="https://img.shields.io/badge/Cloud-Coming_Soon-2496ED?style=flat-square" alt="Cloud"></a> -->
</p>

<p align="center">
<strong><a href="./docs/getting-started.md">Quick Start</a> • <a href="#why-endee">Why Endee</a> • <a href="#use-cases">Use Cases</a> • <a href="#features">Features</a> • <a href="#api-and-clients">API and Clients</a> • <a href="#docs-and-links">Docs</a> • <a href="#community-and-contact">Contact</a></strong>
</p>

# Endee: Open-Source Vector Database for AI Search

**Endee** is a high-performance open-source vector database built for AI search and retrieval workloads. It is designed for teams building **RAG pipelines**, **semantic search**, **hybrid search**, recommendation systems, and filtered vector retrieval APIs that need production-oriented performance and control.

Endee combines vector search with filtering, sparse retrieval support, backup workflows, and deployment flexibility across local builds and Docker-based environments. The project is implemented in C++ and optimized for modern CPU targets, including AVX2, AVX512, NEON, and SVE2.

If you want the fastest path to evaluate Endee locally, start with the [Getting Started guide](./docs/getting-started.md) or the hosted docs at [docs.endee.io](https://docs.endee.io/quick-start).

## Why Endee

- Built as a dedicated vector database for AI applications, search systems, and retrieval-heavy workloads.
- Supports dense vector retrieval plus sparse search capabilities for hybrid search use cases.
- Includes payload filtering for metadata-aware retrieval and application-specific query logic.
- Ships with operational features already documented in this repo, including backup flows and runtime observability.
- Offers flexible deployment paths: local scripts, manual builds, Docker images, and prebuilt registry images.

## Getting Started

The full installation, build, Docker, runtime, and authentication instructions are in [docs/getting-started.md](./docs/getting-started.md).

Fastest local path:

```bash
chmod +x ./install.sh ./run.sh
./install.sh --release --avx2
./run.sh
```

The server listens on port `8080`. For detailed setup paths, supported operating systems, CPU optimization flags, Docker usage, and authentication examples, use:

- [Getting Started](./docs/getting-started.md)
- [Hosted Quick Start Docs](https://docs.endee.io/quick-start)

## Use Cases

### RAG and AI Retrieval

Use Endee as the retrieval layer for question answering, chat assistants, copilots, and other RAG applications that need fast vector search with metadata-aware filtering.

### Agentic AI and AI Agent Memory

Use Endee as the long-term memory and context retrieval layer for AI agents built with frameworks like LangChain, CrewAI, AutoGen, and LlamaIndex. Store and retrieve past observations, tool outputs, conversation history, and domain knowledge mid-execution with low-latency filtered vector search, so your autonomous agents get the right context without stalling their reasoning loop.

### Semantic Search

Build semantic search experiences for documents, products, support content, and knowledge bases using vector similarity search instead of exact keyword-only matching.

### Hybrid Search

Combine dense retrieval, sparse vectors, and filtering to improve relevance for search workflows where both semantic understanding and term-level precision matter.

### Recommendations and Matching

Support recommendation, similarity matching, and nearest-neighbor retrieval workflows across text, embeddings, and other high-dimensional representations.

## Features

- **Vector search** for AI retrieval and semantic similarity workloads.
- **Hybrid retrieval support** with sparse vector capabilities documented in [docs/sparse.md](./docs/sparse.md).
- **Payload filtering** for structured retrieval logic documented in [docs/filter.md](./docs/filter.md).
- **Backup APIs and flows** documented in [docs/backup-system.md](./docs/backup-system.md).
- **Operational logging and instrumentation** documented in [docs/logs.md](./docs/logs.md) and [docs/mdbx-instrumentation.md](./docs/mdbx-instrumentation.md).
- **CPU-targeted builds** for AVX2, AVX512, NEON, and SVE2 deployments.
- **Docker deployment options** for local and server environments.

## API and Clients

Endee exposes an HTTP API for managing indexes and serving retrieval workloads. The current repo documentation and examples focus on running the server directly and calling its API endpoints.

Current developer entry points:

- [Getting Started](./docs/getting-started.md) for local build and run flows
- [Hosted Docs](https://docs.endee.io/quick-start) for product documentation
- [Release Notes 1.0.0](https://github.com/endee-io/endee/releases/tag/1.0.0) for recent platform changes

## Docs and Links

- [Getting Started](./docs/getting-started.md)
- [Hosted Documentation](https://docs.endee.io/quick-start)
- [Release Notes](https://github.com/endee-io/endee/releases/tag/1.0.0)
- [Sparse Search](./docs/sparse.md)
- [Filtering](./docs/filter.md)
- [Backups](./docs/backup-system.md)

## Community and Contact

- Join the community on [Discord](https://discord.gg/5HFGqDZQE3)
- Visit the website at [endee.io](https://endee.io/)
- For trademark or branding permissions, contact [enterprise@endee.io](mailto:enterprise@endee.io)

## Contributing

We welcome contributions from the community to help make vector search faster and more accessible for everyone.

- Submit pull requests for fixes, features, and improvements
- Report bugs or performance issues through GitHub issues
- Propose enhancements for search quality, performance, and deployment workflows

## License

Endee is open source software licensed under the **Apache License 2.0**. See the [LICENSE](./LICENSE) file for full terms.

## Trademark and Branding

“Endee” and the Endee logo are trademarks of Endee Labs.

The Apache License 2.0 does not grant permission to use the Endee name, logos, or branding in a way that suggests endorsement or affiliation.

If you offer a hosted or managed service based on this software, you must use your own branding and avoid implying it is an official Endee service.

## Third-Party Software

This project includes or depends on third-party software components licensed under their respective open-source licenses. Use of those components is governed by their own license terms.






# AI Semantic Search & RAG System using Endee Vector Database

## Project Overview

This project demonstrates an **AI-powered semantic search system** built using the Endee vector database. The application allows users to upload PDF documents, automatically index them into vector embeddings, and ask natural language questions to retrieve relevant information from the documents.

The system implements a **Retrieval-Augmented Generation (RAG) pipeline**, where document chunks are embedded into vectors and stored in a vector database. When a user asks a question, the system retrieves the most semantically similar document chunks and returns the relevant information.

This project showcases practical AI infrastructure concepts such as vector databases, embeddings, semantic similarity search, and document-based question answering.

---

## Problem Statement

Traditional keyword-based search systems often fail to understand the **context and semantic meaning** of user queries. This makes it difficult to retrieve relevant information from large documents.

The goal of this project is to build a **semantic document search system** that can understand the meaning of a user's query and retrieve the most relevant information from uploaded documents.

---

## Key Features

* Upload PDF documents dynamically
* Automatic text extraction from PDFs
* Document chunking for better retrieval accuracy
* Embedding generation using transformer models
* Semantic similarity search
* Vector storage and retrieval using Endee
* Question answering interface
* Simple web-based UI using Flask

---

## System Architecture

User Interface (HTML + JavaScript)
↓
Flask Backend API
↓
PDF Upload & Text Extraction
↓
Document Chunking
↓
Embedding Generation
↓
Vector Storage in Endee
↓
Semantic Similarity Search
↓
Retrieve Relevant Context
↓
Return Answer to User

---

## Technologies Used

### Backend

* Python
* Flask

### AI / Machine Learning

* Sentence Transformers (for embeddings)

### Vector Database

* Endee Vector Database

### Document Processing

* PyPDF2

### Frontend

* HTML
* CSS
* JavaScript (Fetch API)

---

## How Endee is Used in This Project

Endee is used as the **vector database** to store and retrieve document embeddings.

Steps:

1. Extract text from uploaded PDFs.
2. Split text into smaller chunks.
3. Convert each chunk into vector embeddings.
4. Store embeddings in Endee.
5. Convert user queries into embeddings.
6. Perform similarity search in Endee.
7. Retrieve the most relevant document chunks.

This enables **semantic search instead of simple keyword matching**.

---

## Project Folder Structure

```
endeeP
│
├── app.py
├── semantic_search.py
├── pdf_indexer.py
├── embeddings.py
├── data_store.py
│
├── templates
│   └── index.html
│
├── uploads
│
└── README.md
```

---

## Installation & Setup

### Step 1: Clone the Forked Repository

```
git clone https://github.com/YOUR_USERNAME/endee.git
```

### Step 2: Navigate to the Project Folder

```
cd endee
```

### Step 3: Create Virtual Environment

```
python -m venv venv
```

Activate it:

Windows:

```
venv\Scripts\activate
```

Mac/Linux:

```
source venv/bin/activate
```

### Step 4: Install Dependencies

```
pip install flask PyPDF2 sentence-transformers numpy
```

---

## Running the Project

Start the Flask server:

```
python app.py
```

Open browser:

```
http://127.0.0.1:5000
```

---

## How to Use the System

1. Upload a PDF document.
2. The system extracts text and converts it into vector embeddings.
3. The embeddings are stored in the vector database.
4. Ask a question related to the uploaded document.
5. The system retrieves the most relevant document chunks using semantic similarity.
6. The result is displayed in the interface.

---

## Example Queries

* What is machine learning?
* Explain neural networks.
* What are the applications of artificial intelligence?

---

## Future Improvements

* Multi-document semantic search
* Chat-style conversational interface
* Highlighting answer sources
* Integration with large language models for better RAG responses
* Support for multiple file formats (PDF, DOCX, TXT)

---

## Conclusion

This project demonstrates a practical implementation of **semantic search and Retrieval-Augmented Generation using vector databases**. By leveraging embeddings and vector similarity search, the system can retrieve relevant information based on meaning rather than keywords.

The project highlights how modern AI systems use vector databases such as Endee to power intelligent search and document understanding applications.

---

## References

* Endee Vector Database Documentation
* Sentence Transformers Documentation
* Flask Documentation
* PyPDF2 Documentation

---
