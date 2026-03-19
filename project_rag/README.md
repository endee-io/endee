# ⚖️ Legal Intelligence Platform
> RAG-powered legal document analysis — built on Endee Vector DB + Groq LLM

---

## Features

| Feature | Description |
|---|---|
| 📤 **Document Upload** | Upload `.txt` or `.pdf` → chunked & stored in Endee |
| 💬 **Q&A + Source Highlighting** | Ask questions → answers with exact paragraph highlights |
| 🧾 **Clause Extraction** | Rule-based + LLM-powered clause detection |
| ⚠️ **Risk Detection** | Missing clauses + risky terms with severity levels |
| 🧠 **Simplification Mode** | Legal jargon → plain English |
| 📊 **Summary Dashboard** | Visual overview: risk level, clause count, AI summary |
| 📥 **PDF Export** | Full report (clauses + risks + simplified + summary) |

---

## File Structure (flat)

```
legal_rag/
├── app.py            # Main Streamlit app — all features integrated
├── ingest.py         # CLI batch ingestor (.txt / .pdf)
├── query.py          # CLI query engine
├── requirements.txt  # Python dependencies
├── .env.example      # Environment variable template
└── README.md
```

---

## Setup

### 1. Start Endee Vector DB

```bash
docker run \
  --ulimit nofile=100000:100000 \
  -p 8080:8080 \
  -v ./endee-data:/data \
  --name endee-server \
  --restart unless-stopped \
  endeeio/endee-server:latest
```

Verify: `curl http://localhost:8080/api/v1/health`

### 2. Install dependencies

```bash
pip install -r requirements.txt
```

### 3. Configure environment

```bash
cp .env.example .env
# Edit .env — add your GROQ_API_KEY
```

### 4. Run the app

```bash
streamlit run app.py
```

Open `http://localhost:8501`

---

## CLI Tools

**Batch ingest a folder of documents:**
```bash
python ingest.py --dir ./data --chunk-size 500
```

**Query from the terminal:**
```bash
python query.py "What is the termination notice period?"
python query.py "What are the payment terms?" --top-k 5 --temp 0.1
```

---

## Environment Variables

| Variable | Required | Default | Description |
|---|---|---|---|
| `GROQ_API_KEY` | ✅ | — | From console.groq.com |
| `ENDEE_URL` | — | `http://localhost:8080` | Endee server URL |
| `INDEX_NAME` | — | `LEGALDOC` | Vector index name |
| `ENDEE_AUTH_TOKEN` | — | `""` | Auth token if Endee secured |
| `ENDEE_HOSTPORT` | — | — | Render internal networking |

---

## Tech Stack

| Component | Technology |
|---|---|
| UI | Streamlit |
| Vector DB | Endee (endee.io) |
| Embeddings | all-MiniLM-L6-v2 |
| LLM | Groq (llama-3.3-70b-versatile) |
| PDF Export | ReportLab |
| PDF Parsing | pypdf |
| Serialization | msgpack |

---

## Deployment on Render

1. Push to GitHub
2. Render → New → Blueprint → connect repo
3. Set `GROQ_API_KEY` environment variable
4. Deploy — auto-recovery handles ephemeral storage restarts