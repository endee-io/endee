# Nexus Backend - FastAPI Service

## Overview
Backend service for Nexus AI Knowledge Network. Handles document processing, embedding generation, and knowledge graph construction using Endee vector database.

## Architecture

```
nexus-backend/
├── main.py                 # FastAPI application entry point
├── requirements.txt        # Python dependencies
├── services/
│   ├── endee_client.py     # Endee vector database client
│   ├── embedding_service.py # Sentence-transformers embeddings
│   ├── document_processor.py # Document parsing and chunking
│   ├── graph_builder.py    # Knowledge graph construction
│   └── query_engine.py     # Semantic query execution
└── uploads/                # Temporary file storage
```

## Prerequisites

1. **Python 3.10+**
2. **Endee Vector Database** running at `http://localhost:3001`

## Quick Start

### 1. Install Dependencies

```bash
pip install -r requirements.txt
```

### 2. Configure Environment

Create `.env` file:

```env
ENDEE_URL=http://localhost:3001
ENDEE_INDEX=nexus_knowledge
PORT=8000
```

### 3. Start Endee Server

```bash
# From the main endee directory
./run.sh
```

### 4. Initialize Nexus Backend

```bash
python main.py
```

The API will be available at `http://localhost:8000`

### 5. Initialize System

```bash
curl -X POST http://localhost:8000/api/initialize
```

## API Endpoints

### Health Check
```bash
GET /health
```

### Upload Document
```bash
POST /api/documents/upload
Content-Type: multipart/form-data

file: <PDF/TXT/MD/DOCX>
```

### Get Knowledge Graph
```bash
GET /api/graph?similarity_threshold=0.7&max_nodes=100
```

### Semantic Query
```bash
POST /api/query
Content-Type: application/json

{
  "query": "Show everything related to machine learning",
  "top_k": 10,
  "similarity_threshold": 0.7
}
```

### Get Node Details
```bash
GET /api/node/{node_id}
```

### System Statistics
```bash
GET /api/stats
```

## Core Services

### 1. Endee Client (`endee_client.py`)
- Vector database interface
- CRUD operations on embeddings
- Similarity search
- Index management

### 2. Embedding Service (`embedding_service.py`)
- Sentence-transformers: `all-MiniLM-L6-v2`
- 384-dimensional embeddings
- Normalized for cosine similarity
- Batch processing support

### 3. Document Processor (`document_processor.py`)
- Multi-format support (PDF, TXT, MD, DOCX)
- Intelligent chunking with overlap
- Metadata extraction
- File management

### 4. Graph Builder (`graph_builder.py`)
- Automatic relationship discovery
- Node and edge generation
- Graph statistics
- Document-to-graph pipeline

### 5. Query Engine (`query_engine.py`)
- Natural language query processing
- Semantic search execution
- Related concept discovery
- Knowledge gap detection (heuristic)

## Development

### Running with Auto-reload
```bash
uvicorn main:app --reload --port 8000
```

### Testing API
```bash
# Interactive API docs
http://localhost:8000/docs

# Alternative docs
http://localhost:8000/redoc
```

## Production Deployment

### Railway
```bash
# Install Railway CLI
npm install -g railway

# Login and deploy
railway login
railway init
railway up
```

### Render
1. Connect GitHub repository
2. Select "Web Service"
3. Build Command: `pip install -r requirements.txt`
4. Start Command: `uvicorn main:app --host 0.0.0.0 --port $PORT`

## Performance Notes

- **Embedding Generation**: ~50ms per chunk (GPU accelerated if available)
- **Vector Search**: <10ms for 1000 vectors (Endee native performance)
- **Graph Construction**: Scales linearly with document count
- **Recommended Limits**: 1000 documents, 50,000 chunks

## Error Handling

All endpoints return JSON errors:

```json
{
  "error": "Detailed error message"
}
```

Status codes:
- `200`: Success
- `400`: Invalid request
- `404`: Resource not found
- `500`: Server error

## Logging

Logs are output to stdout with format:
```
INFO - timestamp - message
ERROR - timestamp - error details
```

## Security Notes

Development mode features:
- CORS enabled for localhost:3000
- No authentication (add JWT for production)
- File uploads limited to 10MB
- Input validation on all endpoints

## Future Enhancements

- [ ] Multi-user support with JWT auth
- [ ] PostgreSQL for metadata persistence
- [ ] Redis caching layer
- [ ] Websocket support for real-time graph updates
- [ ] Advanced clustering algorithms
- [ ] Export graph as JSON/GraphML
