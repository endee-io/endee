# Nexus Project - Quick Reference

## 🎯 Project URLs (Local Development)

- **Frontend**: http://localhost:3000
- **Backend API**: http://localhost:8000
- **API Docs**: http://localhost:8000/docs
- **Endee**: http://localhost:3001

---

## 🚀 Quick Start Commands

### Start All Services
```bash
# Option 1: Automated script (Linux/Mac)
./scripts/start-nexus.sh

# Option 2: Manual (any OS)
# Terminal 1 - Endee
./run.sh

# Terminal 2 - Backend
cd nexus-backend && source venv/bin/activate && python main.py

# Terminal 3 - Frontend
cd nexus-frontend && npm run dev
```

### Stop All Services
```bash
./scripts/stop-nexus.sh
```

---

## 📁 Project Structure Quick Reference

```
endee-network-map/
├── src/                     # Endee C++ source
├── nexus-backend/           # Python FastAPI backend
│   ├── main.py              # API entry point
│   └── services/            # Core services
├── nexus-frontend/          # Next.js frontend
│   └── src/
│       ├── app/             # Pages
│       ├── components/      # React components
│       └── store/           # State management
├── scripts/                 # Utility scripts
├── NEXUS_README.md          # Main documentation
├── ARCHITECTURE.md          # System architecture
├── DEVELOPMENT.md           # Dev guide
└── DEPLOYMENT.md            # Deployment guide
```

---

## 🛠️ Common Development Tasks

### Backend

**Add new endpoint:**
```python
# In nexus-backend/main.py
@app.get("/api/your-endpoint")
async def your_function():
    return {"data": "value"}
```

**Test endpoint:**
```bash
curl http://localhost:8000/api/your-endpoint
```

### Frontend

**Create new component:**
```tsx
// In nexus-frontend/src/components/YourComponent.tsx
export default function YourComponent() {
  return <div>Content</div>
}
```

**Use in page:**
```tsx
import YourComponent from '@/components/YourComponent'
```

### Database (Endee)

**Insert vectors:**
```python
await endee_client.insert_vectors(
    index_name="nexus_knowledge",
    vectors=[[0.1, 0.2, ...]],
    ids=["id1"]
)
```

**Search:**
```python
results = await endee_client.search(
    index_name="nexus_knowledge",
    query_vector=[0.1, 0.2, ...],
    top_k=10
)
```

---

## 🐛 Troubleshooting Quick Fixes

### Backend won't start
```bash
cd nexus-backend
source venv/bin/activate
pip install -r requirements.txt
```

### Frontend won't start
```bash
cd nexus-frontend
rm -rf node_modules package-lock.json
npm install
```

### Endee connection error
```bash
# Check if running
curl http://localhost:3001/health

# Restart
./run.sh
```

### CORS error
```python
# In main.py, add your frontend URL
allow_origins=["http://localhost:3000"]
```

---

## 📦 Dependencies

### Backend (Python)
```bash
pip install fastapi uvicorn sentence-transformers PyPDF2 python-docx httpx
```

### Frontend (Node.js)
```bash
npm install next react react-dom reactflow typescript tailwindcss
```

---

## 🔑 Environment Variables

### Backend (.env)
```env
ENDEE_URL=http://localhost:3001
ENDEE_INDEX=nexus_knowledge
```

### Frontend (.env.local)
```env
NEXT_PUBLIC_API_URL=http://localhost:8000
```

---

## 📊 Key Features Implementation

### 1. Document Upload
- **Backend**: `POST /api/documents/upload`
- **Frontend**: `ControlPanel.tsx`
- **Service**: `document_processor.py`

### 2. Knowledge Graph
- **Backend**: `GET /api/graph`
- **Frontend**: `KnowledgeGraph.tsx`
- **Service**: `graph_builder.py`

### 3. Semantic Search
- **Backend**: `POST /api/query`
- **Frontend**: `ControlPanel.tsx` (search section)
- **Service**: `query_engine.py`

### 4. Node Details
- **Backend**: `GET /api/node/{node_id}`
- **Frontend**: `NodePanel.tsx`
- **Service**: `graph_builder.py::get_node_details()`

---

## 🎨 UI Component Hierarchy

```
App (page.tsx)
├── Header
├── ControlPanel
│   ├── Upload Section
│   ├── Search Section
│   └── Stats Section
├── KnowledgeGraph (React Flow)
│   ├── Nodes
│   ├── Edges
│   ├── Controls
│   └── MiniMap
└── NodePanel (conditional)
    ├── Summary
    ├── Metadata
    └── Related Nodes
```

---

## 🧪 Testing

### Manual API Testing
```bash
# Health check
curl http://localhost:8000/health

# Upload document
curl -X POST http://localhost:8000/api/documents/upload \
  -F "file=@test.pdf"

# Get graph
curl http://localhost:8000/api/graph

# Search
curl -X POST http://localhost:8000/api/query \
  -H "Content-Type: application/json" \
  -d '{"query": "machine learning", "top_k": 10}'
```

### Frontend Testing
1. Open http://localhost:3000
2. Upload a document
3. Wait for graph to generate
4. Click nodes to see details
5. Try semantic search

---

## 📝 Git Workflow

```bash
# Create feature branch
git checkout -b feature/your-feature

# Make changes
git add .
git commit -m "feat: add your feature"

# Push
git push origin feature/your-feature

# Create PR on GitHub
```

---

## 🚀 Deployment Quick Guide

1. **Build Endee**: `./install.sh --release --avx2`
2. **Deploy Endee**: Docker container on cloud VM
3. **Deploy Backend**: Render.com or Railway
4. **Deploy Frontend**: Vercel.com

See [DEPLOYMENT.md](DEPLOYMENT.md) for detailed instructions.

---

## 📚 Documentation Links

- [Main README](NEXUS_README.md) - Project overview
- [Architecture](ARCHITECTURE.md) - System design
- [Development Guide](DEVELOPMENT.md) - Detailed dev instructions
- [Deployment Guide](DEPLOYMENT.md) - Production deployment

---

## 🆘 Getting Help

1. Check documentation files above
2. Review code comments
3. Test with curl commands
4. Check browser console (F12)
5. Review logs in terminal
6. Open issue on GitHub

---

## ⚡ Performance Tips

- Use batch operations for embeddings
- Cache frequent queries
- Limit graph nodes displayed (100 max recommended)
- Use async/await properly
- Enable production builds for deployment

---

## 🎯 Next Steps

- [ ] Add more document formats
- [ ] Implement authentication
- [ ] Add export functionality
- [ ] Improve graph layout algorithms
- [ ] Add real-time updates
- [ ] Create mobile responsive design

---

**Version**: 1.0.0
**Last Updated**: February 2026
