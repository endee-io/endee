#!/usr/bin/env python
import os
from typing import List, Optional

from dotenv import load_dotenv
from fastapi import FastAPI, File, UploadFile, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from langchain_text_splitters import RecursiveCharacterTextSplitter
from langchain_google_genai import ChatGoogleGenerativeAI
from sentence_transformers import SentenceTransformer, CrossEncoder
from starlette.responses import JSONResponse
from tavily import TavilyClient

from endee import Endee, Precision
from endee.schema import VectorItem as EndeeVectorItem

# Monkey-patch Endee VectorItem to behave like a dict for `.get()` calls inside the SDK
if not hasattr(EndeeVectorItem, "get"):
    EndeeVectorItem.get = lambda self, key, default=None: getattr(self, key, default)

load_dotenv()

# Environment
ENDEE_BASE_URL = os.getenv("ENDEE_BASE_URL", "http://localhost:8080/api/v1")
ENDEE_AUTH_TOKEN = os.getenv("ENDEE_AUTH_TOKEN", "")
ENDEE_INDEX_NAME = os.getenv("ENDEE_INDEX_NAME", "rag_app")

GEMINI_API_KEY = os.getenv("GEMINI_API_KEY")
GEMINI_MODEL = os.getenv("GEMINI_MODEL", "gemini-2.5-flash")

CHUNK_SIZE = int(os.getenv("CHUNK_SIZE", "800"))
CHUNK_OVERLAP = int(os.getenv("CHUNK_OVERLAP", "120"))
TOP_K = int(os.getenv("TOP_K", "6"))

TAVILY_API_KEY = os.getenv("TAVILY_API_KEY")

app = FastAPI(title="RAG Agentic Backend", version="0.2.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


def get_endee_client() -> Endee:
    client = Endee(ENDEE_AUTH_TOKEN) if ENDEE_AUTH_TOKEN else Endee()
    client.set_base_url(ENDEE_BASE_URL)
    return client


def ensure_index(client: Endee, dim: int) -> None:
    try:
        client.get_index(name=ENDEE_INDEX_NAME)
        return
    except Exception:
        pass
    client.create_index(
        name=ENDEE_INDEX_NAME,
        dimension=dim,
        space_type="cosine",
        precision=Precision.INT8,
    )


embedder: Optional[SentenceTransformer] = None
reranker: Optional[CrossEncoder] = None
llm: Optional[ChatGoogleGenerativeAI] = None
endee_client: Optional[Endee] = None
endee_index = None
tavily_client: Optional[TavilyClient] = None


def bootstrap():
    global embedder, reranker, llm, endee_client, endee_index, tavily_client
    if GEMINI_API_KEY is None:
        raise RuntimeError("GEMINI_API_KEY is required")

    embedder = SentenceTransformer("sentence-transformers/all-MiniLM-L6-v2")
    reranker = CrossEncoder("cross-encoder/ms-marco-MiniLM-L-6-v2")
    llm = ChatGoogleGenerativeAI(
        model=GEMINI_MODEL,
        api_key=GEMINI_API_KEY,
        temperature=0.2,
    )
    endee_client = get_endee_client()
    ensure_index(endee_client, embedder.get_sentence_embedding_dimension())
    endee_index = endee_client.get_index(name=ENDEE_INDEX_NAME)

    if TAVILY_API_KEY:
        tavily_client = TavilyClient(api_key=TAVILY_API_KEY)


bootstrap()


def read_file(file: UploadFile) -> str:
    suffix = file.filename.split(".")[-1].lower()
    content = file.file.read()
    if suffix == "pdf":
        try:
            from pypdf import PdfReader
        except Exception as exc:
            raise HTTPException(status_code=500, detail=f"pypdf missing: {exc}")
        file.file.seek(0)
        reader = PdfReader(file.file)
        text = "\n".join([p.extract_text() or "" for p in reader.pages])
    elif suffix in {"txt", "md"}:
        text = content.decode("utf-8", errors="ignore")
    elif suffix in {"docx"}:
        try:
            import docx2txt
        except Exception as exc:
            raise HTTPException(status_code=500, detail=f"docx2txt missing: {exc}")
        temp_path = f"/tmp/{file.filename}"
        with open(temp_path, "wb") as f:
            f.write(content)
        text = docx2txt.process(temp_path) or ""
        os.remove(temp_path)
    else:
        raise HTTPException(status_code=400, detail="Unsupported file type")
    return text


def chunk_text(text: str) -> List[str]:
    splitter = RecursiveCharacterTextSplitter(
        chunk_size=CHUNK_SIZE,
        chunk_overlap=CHUNK_OVERLAP,
        separators=["\n\n", "\n", " ", ""],
    )
    return splitter.split_text(text)


def upsert_chunks(chunks: List[str], source: str):
    embeddings = embedder.encode(chunks, convert_to_numpy=False)
    payload = []
    for idx, (chunk, vector) in enumerate(zip(chunks, embeddings)):
        payload.append(
            {
                "id": f"{source}::chunk-{idx}",
                "vector": vector.tolist(),
                "meta": {"source": source, "text": chunk},
            }
        )
    endee_index.upsert(payload)


def _normalize_result(item) -> dict:
    if hasattr(item, "dict"):
        base = item.dict()
        similarity = getattr(item, "similarity", None)
        distance = getattr(item, "distance", None)
    else:
        base = dict(item)
        similarity = base.get("similarity")
        distance = base.get("distance")
    return {
        "id": base.get("id"),
        "meta": base.get("meta") or {},
        "similarity": similarity,
        "distance": distance,
        "raw": item,
    }


def retrieve(query: str):
    query_vec = embedder.encode([query])[0].tolist()
    results = endee_index.query(vector=query_vec, top_k=TOP_K, include_vectors=False)
    if not results:
        return []
    docs = [_normalize_result(r) for r in results]
    rerank_inputs = [(query, doc["meta"].get("text", "")) for doc in docs]
    scores = reranker.predict(rerank_inputs)
    reranked = sorted(
        [
            {**doc, "rerank_score": float(score)}
            for doc, score in zip(docs, scores)
        ],
        key=lambda x: x["rerank_score"],
        reverse=True,
    )
    return reranked[:4]


def web_search(query: str):
    if not tavily_client:
        raise HTTPException(status_code=500, detail="TAVILY_API_KEY not configured")
    res = tavily_client.search(query=query, max_results=5, include_images=False)
    docs = []
    for item in res.get("results", []):
        docs.append(
            {
                "id": item.get("url"),
                "meta": {
                    "source": item.get("url"),
                    "text": item.get("content", ""),
                    "title": item.get("title", "")
                },
                "score": item.get("score"),
            }
        )
    return docs


def build_context(docs: List[dict]) -> str:
    parts = []
    for doc in docs:
        meta = doc.get("meta", {})
        src = meta.get("title") or meta.get("source")
        parts.append(f"[{src}] {meta.get('text', '')}")
    return "\n\n".join(parts)


def build_history(history: List[dict]) -> str:
    lines = []
    for turn in history[-5:]:
        u = turn.get("user") or ""
        a = turn.get("answer") or ""
        lines.append(f"User: {u}\nAssistant: {a}")
    return "\n\n".join(lines)


def route_query(question: str, force_mode: str = "auto") -> str:
    if force_mode in {"rag", "web", "direct"}:
        return force_mode
    # lightweight classification with the main LLM
    prompt = (
        "Decide routing for the question. Output only one token: RAG, WEB, or DIRECT.\n"
        "Use RAG if it likely needs internal docs; WEB if it's about current/general external info; otherwise DIRECT.\n"
        f"Question: {question}\nAnswer:"
    )
    try:
        resp = llm.invoke(prompt)
        text = resp.content.strip().upper()
        if "WEB" in text:
            return "web"
        if "RAG" in text:
            return "rag"
        return "direct"
    except Exception:
        return "direct"


def answer(question: str, context_docs: List[dict], history: List[dict], mode: str) -> dict:
    if context_docs:
        context_text = build_context(context_docs)
        prompt = (
            "Use ONLY the provided context (and brief history if present). "
            "If something is missing, say you don't have it.\n"
            f"Context:\n{context_text}\n\n"
        )
    else:
        prompt = "Answer the user. If you don't know, say so.\n"

    history_text = build_history(history)
    if history_text:
        prompt += f"Recent history (for coherence, not new facts):\n{history_text}\n\n"

    prompt += f"Question: {question}"

    resp = llm.invoke(prompt)
    return {
        "answer": resp.content,
        "sources": [
            {
                "id": doc.get("id"),
                "source": doc.get("meta", {}).get("source"),
                "title": doc.get("meta", {}).get("title"),
                "score": doc.get("rerank_score", doc.get("similarity", doc.get("score"))),
                "preview": doc.get("meta", {}).get("text", "")[:200],
            }
            for doc in context_docs
        ],
        "mode": mode,
    }


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/upload")
async def upload(file: UploadFile = File(...)):
    try:
        text = read_file(file)
        chunks = chunk_text(text)
        upsert_chunks(chunks, source=file.filename)
        return {"message": f"Indexed {len(chunks)} chunks from {file.filename}"}
    except HTTPException as e:
        raise e
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc))


@app.post("/chat")
async def chat(payload: dict):
    question = payload.get("message") or ""
    force_mode = (payload.get("mode") or "auto").lower()
    history = payload.get("history") or []
    if not question:
        raise HTTPException(status_code=400, detail="message is required")

    route = route_query(question, force_mode=force_mode)

    if route == "rag":
        docs = retrieve(question)
    elif route == "web":
        docs = web_search(question)
    else:
        docs = []

    result = answer(question, docs, history, mode=route)
    return JSONResponse(result)
