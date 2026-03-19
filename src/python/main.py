from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse
from pydantic import BaseModel, Field
from typing import Optional

from storage import VectorStore
from embedder import embed

app = FastAPI(
    title="Employee Memory AI",
    version="1.0.0"
)

store = VectorStore()

# -------- Models --------
class AddRequest(BaseModel):
    text: str = Field(..., min_length=2)
    employee_id: str
    department: str

class SearchRequest(BaseModel):
    query: str = Field(..., min_length=2)
    department: Optional[str] = None
    top_k: int = Field(5, ge=1, le=20)

# -------- UI --------
@app.get("/", response_class=HTMLResponse)
def ui():
    return """
    <html>
    <head>
        <title>Employee Memory AI</title>
    </head>
    <body style="font-family: Arial; padding: 40px;">
        <h1>Employee Memory AI 🚀</h1>

        <h3>Add Memory</h3>
        <input id="text" placeholder="Text"><br><br>
        <input id="emp" placeholder="Employee ID"><br><br>
        <input id="dept" placeholder="Department"><br><br>
        <button onclick="add()">Add</button>

        <h3>Search</h3>
        <input id="query" placeholder="Search query"><br><br>
        <input id="dept2" placeholder="Department (optional)"><br><br>
        <button onclick="search()">Search</button>

        <h3>Results:</h3>
        <pre id="results"></pre>

        <script>
        async function add() {
            await fetch('/add', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({
                    text: document.getElementById('text').value,
                    employee_id: document.getElementById('emp').value,
                    department: document.getElementById('dept').value
                })
            });
            alert("Memory Added ✅");
        }

        async function search() {
            const res = await fetch('/search', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({
                    query: document.getElementById('query').value,
                    department: document.getElementById('dept2').value
                })
            });

            const data = await res.json();
            document.getElementById('results').innerText =
                JSON.stringify(data, null, 2);
        }
        </script>
    </body>
    </html>
    """

# -------- API --------
@app.get("/health")
def health():
    return {"status": "ok"}

@app.post("/add")
def add(req: AddRequest):
    try:
        vec = embed(req.text)
        store.add(vec, req.text, req.employee_id, req.department)
        return {"status": "success"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/search")
def search(req: SearchRequest):
    try:
        vec = embed(req.query)
        results = store.search(vec, req.top_k, req.department)
        return {"results": results}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))