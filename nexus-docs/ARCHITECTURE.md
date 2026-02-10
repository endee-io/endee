# Nexus Architecture Diagram

## System Components

```mermaid
graph TB
    subgraph "Frontend Layer"
        UI[Next.js Application]
        RF[React Flow Graph]
        ZS[Zustand Store]
    end
    
    subgraph "Backend Layer"
        API[FastAPI REST API]
        DP[Document Processor]
        ES[Embedding Service]
        GB[Graph Builder]
        QE[Query Engine]
    end
    
    subgraph "Data Layer"
        VDB[(Endee Vector DB)]
        FS[File Storage]
    end
    
    subgraph "External Services"
        ST[Sentence Transformers]
    end
    
    UI --> API
    RF --> ZS
    ZS --> API
    
    API --> DP
    API --> GB
    API --> QE
    
    DP --> ES
    GB --> ES
    QE --> ES
    
    ES --> ST
    
    GB --> VDB
    QE --> VDB
    DP --> FS
    
    VDB -.similarity search.-> GB
    VDB -.semantic query.-> QE

    style UI fill:#667eea
    style API fill:#48bb78
    style VDB fill:#ed8936
    style ES fill:#9f7aea
```

## Data Flow: Document Upload

```mermaid
sequenceDiagram
    participant U as User
    participant F as Frontend
    participant B as Backend
    participant D as Doc Processor
    participant E as Embeddings
    participant V as Endee
    participant G as Graph Builder

    U->>F: Upload PDF
    F->>B: POST /api/documents/upload
    B->>D: process_document(file)
    D->>D: Extract text
    D->>D: Create chunks
    D-->>B: document_id, chunks
    
    B->>E: encode_batch(texts)
    E->>E: Generate embeddings
    E-->>B: vectors (384-dim)
    
    B->>V: insert_vectors(vectors, metadata)
    V-->>B: success
    
    B->>G: discover_relationships()
    G->>V: search(similarity)
    V-->>G: similar vectors
    G->>G: Create edges
    G-->>B: graph updated
    
    B-->>F: {document_id, chunks_created}
    F->>F: Refresh graph
    F-->>U: Graph visualization
```

## Data Flow: Semantic Query

```mermaid
sequenceDiagram
    participant U as User
    participant F as Frontend
    participant B as Backend
    participant E as Embeddings
    participant V as Endee
    participant Q as Query Engine

    U->>F: Enter query: "machine learning"
    F->>B: POST /api/query
    B->>Q: execute_query(query)
    
    Q->>E: encode(query)
    E-->>Q: query_embedding
    
    Q->>V: search(query_embedding, top_k=10)
    V->>V: HNSW search
    V-->>Q: similar vectors + metadata
    
    Q->>Q: Build graph from results
    Q->>Q: Find edges between results
    Q-->>B: {nodes, edges}
    
    B-->>F: {nodes, edges, exec_time}
    F->>F: Update visualization
    F-->>U: Filtered graph view
```

## Component Interactions

### 1. Document Processing Pipeline

```
PDF/TXT/DOCX → Text Extraction → Chunking → Embedding
                                              ↓
                                    Store in Endee (384-dim vectors)
                                              ↓
                                    Similarity Search (cosine)
                                              ↓
                                    Graph Construction (nodes + edges)
```

### 2. Graph Building Logic

```
For each new chunk embedding:
  1. Search Endee for top-K similar vectors (threshold > 0.7)
  2. Create GraphNode for chunk
  3. Create GraphEdge for each similar result
  4. Update global graph structure
```

### 3. Query Execution Flow

```
User Query → Embed Query → Search Endee → Retrieve Nodes
                                              ↓
                              Find Edges Between Nodes
                                              ↓
                              Return Sub-graph
```

## Technology Stack Diagram

```
┌─────────────────────────────────────────┐
│         Frontend (Next.js)              │
│  ┌────────────┐      ┌───────────────┐ │
│  │ React Flow │      │ Tailwind CSS  │ │
│  └────────────┘      └───────────────┘ │
│  ┌────────────┐      ┌───────────────┐ │
│  │   Zustand  │      │  TypeScript   │ │
│  └────────────┘      └───────────────┘ │
└─────────────┬───────────────────────────┘
              │ HTTP/REST
┌─────────────▼───────────────────────────┐
│       Backend (FastAPI)                 │
│  ┌────────────────────────────────────┐ │
│  │   Document Processor               │ │
│  │   - PyPDF2, python-docx            │ │
│  └────────────────────────────────────┘ │
│  ┌────────────────────────────────────┐ │
│  │   Embedding Service                │ │
│  │   - sentence-transformers          │ │
│  │   - all-MiniLM-L6-v2 (384-dim)     │ │
│  └────────────────────────────────────┘ │
│  ┌────────────────────────────────────┐ │
│  │   Graph Builder                    │ │
│  │   - Relationship discovery         │ │
│  │   - Node/Edge construction         │ │
│  └────────────────────────────────────┘ │
└─────────────┬───────────────────────────┘
              │ HTTP/REST
┌─────────────▼───────────────────────────┐
│      Endee Vector Database (C++)        │
│  ┌────────────────────────────────────┐ │
│  │   HNSW Index                       │ │
│  │   - Fast similarity search         │ │
│  │   - Cosine distance                │ │
│  └────────────────────────────────────┘ │
│  ┌────────────────────────────────────┐ │
│  │   Quantization                     │ │
│  │   - INT8, FP16 support             │ │
│  └────────────────────────────────────┘ │
│  ┌────────────────────────────────────┐ │
│  │   Crow REST API                    │ │
│  └────────────────────────────────────┘ │
└─────────────────────────────────────────┘
```

## Deployment Architecture

```
┌──────────────────────────────────────────────┐
│              Vercel (Frontend)               │
│           ┌─────────────────┐                │
│           │   Next.js App   │                │
│           └─────────────────┘                │
└────────────────┬─────────────────────────────┘
                 │ HTTPS
┌────────────────▼─────────────────────────────┐
│           Render/Railway (Backend)           │
│      ┌──────────────────────────────┐        │
│      │   FastAPI + Uvicorn          │        │
│      │   Python 3.10+               │        │
│      └──────────────────────────────┘        │
└────────────────┬─────────────────────────────┘
                 │ HTTP
┌────────────────▼─────────────────────────────┐
│           Docker Container (Endee)           │
│      ┌──────────────────────────────┐        │
│      │   Endee Vector Database      │        │
│      │   C++ Binary                 │        │
│      └──────────────────────────────┘        │
│      ┌──────────────────────────────┐        │
│      │   Persistent Volume          │        │
│      │   (Vector Storage)           │        │
│      └──────────────────────────────┘        │
└──────────────────────────────────────────────┘
```

## Performance Characteristics

### Latency Breakdown (typical request)

```
User Query: "machine learning"
    ↓
Frontend: 5ms (state update)
    ↓
Network: 20ms (HTTP)
    ↓
Backend: 15ms (API processing)
    ↓
Embedding: 50ms (GPU) / 200ms (CPU)
    ↓
Endee Search: 5ms (10K vectors)
    ↓
Graph Build: 10ms
    ↓
Network: 20ms (response)
    ↓
Frontend Render: 16ms (60 FPS)
    ↓
Total: ~141ms (GPU) / ~291ms (CPU)
```

### Scalability Points

- **Endee**: Horizontal scaling with sharding
- **Backend**: Stateless, load balancer ready
- **Frontend**: CDN distribution, edge rendering
- **Embeddings**: GPU acceleration, batch processing

---

This architecture demonstrates:
- **Clean separation of concerns**
- **Vector-native design**  
- **Production-ready scalability**
- **Modern best practices**
