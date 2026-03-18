# AI Animal Disease Finder 🐾

An intelligent system that predicts possible animal diseases based on user-provided symptoms using semantic search and Retrieval-Augmented Generation (RAG).

## Features

- **Symptom-Based Disease Detection** - Input animal type and symptoms in natural language
- **Semantic Search** - Uses Endee vector database for similarity search
- **RAG-Powered Explanations** - Generates human-readable explanations using Google Gemini
- **Confidence Scores** - Shows top 3 matching diseases with confidence levels
- **Simple Web UI** - Easy-to-use interface for farmers, pet owners, and veterinarians

## Tech Stack

| Component | Technology |
|-----------|------------|
| Backend | Python (FastAPI) |
| Vector Database | Endee |
| Embeddings | Sentence Transformers (all-MiniLM-L6-v2) |
| LLM | Google Gemini (Free Tier) |
| Frontend | HTML, CSS, JavaScript |

## Architecture

```
User Input (Animal + Symptoms)
        ↓
Embedding Model (Sentence Transformers)
        ↓
Endee Vector Database
        ↓
Top-K Similar Results
        ↓
Context Builder
        ↓
LLM (Gemini - RAG)
        ↓
Final Output (Disease + Explanation)
```

## Quick Start

### Prerequisites

1. Python 3.9+
2. Endee server running on port 8080
3. Google Gemini API key (free tier)

### Setup

1. **Install Python dependencies:**
```bash
cd animal-disease-rag
pip install -r requirements.txt
```

2. **Configure environment variables:**
Create a `.env` file:
```env
ENDEE_URL=http://localhost:8080
ENDEE_INDEX=animal_disease_index
GEMINI_API_KEY=your_gemini_api_key_here
```

3. **Start Endee server:**
```bash
# Follow Endee documentation to start the server
./run.sh  # or your local setup
```

4. **Start the backend:**
```bash
cd backend
python app.py
```

5. **Open the frontend:**
Open `frontend/index.html` in your browser, or serve it:
```bash
cd frontend
python -m http.server 3000
```

### Ingest Dataset

The first time you run, ingest the disease dataset into Endee:
```bash
curl -X POST "http://localhost:8000/ingest?csv_path=data/dataset.csv"
```

Or use the browser console:
```javascript
fetch('http://localhost:8000/ingest?csv_path=data/dataset.csv', { method: 'POST' })
```

## API Usage

### Predict Disease

```bash
curl -X POST "http://localhost:8000/predict" \
  -H "Content-Type: application/json" \
  -d '{
    "animal": "Dog",
    "symptoms": "fever, vomiting, loss of appetite"
  }'
```

Response:
```json
{
  "disease": "Parvovirus",
  "confidence": "High",
  "explanation": "Parvovirus is a highly contagious viral infection...",
  "precautions": "Keep vaccinations up to date; disinfect living areas...",
  "top_matches": [
    {"disease": "Parvovirus", "symptoms": "fever,vomiting,lethargy", "score": 0.92}
  ]
}
```

### Health Check

```bash
curl "http://localhost:8000/"
```

## Project Structure

```
animal-disease-rag/
├── backend/
│   ├── app.py              # FastAPI server
│   ├── config.py           # Configuration
│   ├── endee_client.py     # Endee DB connection
│   ├── embeddings.py       # Sentence Transformers
│   ├── rag_pipeline.py     # RAG logic
│   └── dataset_loader.py   # Dataset ingestion & search
├── data/
│   └── dataset.csv         # Disease-symptom dataset
├── frontend/
│   ├── index.html          # Web UI
│   ├── style.css           # Styling
│   └── script.js           # API calls
├── requirements.txt        # Python dependencies
└── README.md               # This file
```

## Supported Animals

- Dog 🐶
- Cat 🐱
- Cow 🐄
- Horse 🐴
- Chicken 🐔
- Pig 🐷
- Sheep 🐑
- Goat 🐐
- Duck 🦆
- Turkey 🦃
- Rabbit 🐰
- Fish 🐟

## Getting a Gemini API Key

1. Go to [Google AI Studio](https://makersuite.google.com/app/apikey)
2. Create a new API key
3. Add it to your `.env` file

## License

MIT License

---

**This project uses Endee as the vector database for semantic retrieval, enabling efficient and scalable similarity search.**
