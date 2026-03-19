"""
Legal Document Intelligence Platform
Built on Endee Vector DB + Groq LLM + RAG architecture
Features: Source Highlighting, Clause Extraction, Risk Detection,
          Simplification Mode, Summary Dashboard, PDF Export
"""

import html
import json
import os
import time
import io

import msgpack
import numpy as np
import requests
import streamlit as st
from dotenv import load_dotenv
from groq import Groq

st.set_page_config(
    page_title="Legal Intelligence Platform",
    page_icon="⚖️",
    layout="wide",
    initial_sidebar_state="expanded",
)

load_dotenv()

# ──────────────────────────────────────────────
# CONFIG
# ──────────────────────────────────────────────
def resolve_endee_url() -> str:
    explicit_url = os.getenv("ENDEE_URL")
    if explicit_url:
        return explicit_url.rstrip("/")
    hostport = os.getenv("ENDEE_HOSTPORT")
    if hostport:
        if hostport.startswith("http://") or hostport.startswith("https://"):
            return hostport.rstrip("/")
        return f"http://{hostport}".rstrip("/")
    return "http://localhost:8080"

ENDEE_URL        = resolve_endee_url()
INDEX_NAME       = os.getenv("INDEX_NAME", "LEGALDOC")
GROQ_API_KEY     = os.getenv("GROQ_API_KEY")
ENDEE_AUTH_TOKEN = os.getenv("ENDEE_AUTH_TOKEN", "")
EMBEDDING_MODEL  = "all-MiniLM-L6-v2"

# ──────────────────────────────────────────────
# INJECT THEME
# ──────────────────────────────────────────────
def inject_theme() -> None:
    st.markdown("""
<style>
@import url('https://fonts.googleapis.com/css2?family=Playfair+Display:wght@700;900&family=DM+Sans:wght@300;400;500;600&family=DM+Mono:wght@400;500&display=swap');

:root {
  --bg:        #06090f;
  --surface:   #0d1420;
  --card:      #111928;
  --border:    #1e2d45;
  --text:      #e8edf5;
  --muted:     #8899b4;
  --gold:      #c9a84c;
  --gold-dim:  #7a5f28;
  --blue:      #3b7dd8;
  --green:     #22c55e;
  --red:       #ef4444;
  --amber:     #f59e0b;
  --radius:    14px;
}

html, body, [data-testid="stAppViewContainer"], [data-testid="stMain"] {
  background: var(--bg) !important;
  color: var(--text);
  font-family: 'DM Sans', sans-serif;
}

[data-testid="stAppViewContainer"]::before {
  content: '';
  position: fixed;
  inset: 0;
  background:
    radial-gradient(ellipse 900px 500px at 5% 10%, rgba(57,100,180,0.12) 0%, transparent 60%),
    radial-gradient(ellipse 600px 400px at 95% 80%, rgba(201,168,76,0.06) 0%, transparent 60%);
  pointer-events: none;
  z-index: 0;
}

[data-testid="stSidebar"] {
  background: linear-gradient(180deg, #080e1a 0%, #060b15 100%) !important;
  border-right: 1px solid var(--border) !important;
}
[data-testid="stSidebar"] * { color: var(--text) !important; }
[data-testid="stHeader"] { background: transparent !important; }

[data-testid="stVerticalBlockBorderWrapper"] {
  background: rgba(13,20,32,0.85) !important;
  border: 1px solid var(--border) !important;
  border-radius: var(--radius) !important;
  backdrop-filter: blur(8px);
  box-shadow: 0 8px 32px rgba(0,0,0,0.4);
}

[data-testid="stMetric"] {
  background: rgba(10,15,25,0.9) !important;
  border: 1px solid var(--border) !important;
  border-radius: 12px !important;
  padding: 10px 14px !important;
}
[data-testid="stMetricLabel"], [data-testid="stMetricValue"] {
  color: var(--text) !important;
}

.stMarkdown, [data-testid="stMarkdownContainer"], p, li, label, span {
  color: var(--text);
}
.stButton > button {
  width: 100%;
  border-radius: 10px;
  border: 1px solid var(--gold-dim) !important;
  color: var(--gold) !important;
  background: rgba(201,168,76,0.08) !important;
  font-family: 'DM Sans', sans-serif;
  font-weight: 600;
  letter-spacing: 0.3px;
  transition: all 0.2s ease;
}
.stButton > button:hover {
  background: rgba(201,168,76,0.15) !important;
  box-shadow: 0 0 20px rgba(201,168,76,0.2);
  transform: translateY(-1px);
}

div[data-baseweb="input"] input,
div[data-baseweb="textarea"] textarea,
div[data-baseweb="select"] > div {
  background: var(--surface) !important;
  color: var(--text) !important;
  border: 1px solid var(--border) !important;
  border-radius: 10px !important;
  font-family: 'DM Sans', sans-serif !important;
}

[data-testid="stFileUploaderDropzone"] {
  background: rgba(13,20,32,0.8) !important;
  border: 1px dashed var(--gold-dim) !important;
  border-radius: 12px !important;
}

[data-testid="stExpander"] details {
  background: rgba(10,16,28,0.9) !important;
  border: 1px solid var(--border) !important;
  border-radius: 12px !important;
}
[data-testid="stExpander"] summary * { color: var(--text) !important; }

[data-testid="stTabs"] [role="tab"] {
  color: var(--muted) !important;
  font-family: 'DM Sans', sans-serif !important;
  font-weight: 500;
}
[data-testid="stTabs"] [role="tab"][aria-selected="true"] {
  color: var(--gold) !important;
  border-bottom-color: var(--gold) !important;
}

/* Custom components */
.hero-wrap {
  padding: 20px 0 28px;
  position: relative;
}
.hero-eyebrow {
  font-family: 'DM Mono', monospace;
  font-size: 0.72rem;
  letter-spacing: 3px;
  color: var(--gold);
  text-transform: uppercase;
  margin-bottom: 8px;
}
.hero-title {
  font-family: 'Playfair Display', serif;
  font-size: 2.4rem;
  font-weight: 900;
  color: var(--text);
  margin: 0 0 8px;
  line-height: 1.1;
}
.hero-title span { color: var(--gold); }
.hero-sub {
  color: var(--muted);
  font-size: 0.95rem;
  margin-bottom: 16px;
}
.hero-rule {
  height: 1px;
  background: linear-gradient(90deg, var(--gold), var(--blue), transparent);
  border: none;
  opacity: 0.5;
}

.status-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 12px;
  margin-bottom: 6px;
  background: rgba(6,9,15,0.8);
  border: 1px solid var(--border);
  border-radius: 10px;
}
.status-dot { width: 8px; height: 8px; border-radius: 50%; margin-right: 8px; display: inline-block; }
.pill { font-size: 0.7rem; font-weight: 600; padding: 2px 9px; border-radius: 999px; border: 1px solid transparent; }
.pill-ok  { color: #bbf7d0; background: rgba(34,197,94,0.12);  border-color: rgba(34,197,94,0.4); }
.pill-off { color: #fecaca; background: rgba(239,68,68,0.12); border-color: rgba(239,68,68,0.4); }

.source-card {
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 12px 14px;
  margin-bottom: 10px;
  background: rgba(10,15,25,0.7);
  transition: all 0.18s;
}
.source-card:hover { transform: translateY(-1px); box-shadow: 0 6px 20px rgba(0,0,0,0.3); }
.source-head { display: flex; justify-content: space-between; margin-bottom: 4px; }
.source-rank { font-family:'DM Mono',monospace; font-size:0.78rem; color:var(--gold); font-weight:600; }
.source-score { font-family:'DM Mono',monospace; font-size:0.78rem; color:var(--muted); }
.source-snippet { font-size:0.84rem; color:var(--muted); line-height:1.5; }
.highlight { background: rgba(201,168,76,0.22); border-radius:3px; padding:1px 3px; color: var(--gold); }

.clause-card {
  border-left: 3px solid var(--blue);
  border-radius: 0 10px 10px 0;
  padding: 10px 14px;
  margin-bottom: 8px;
  background: rgba(59,125,216,0.06);
}
.clause-type { font-family:'DM Mono',monospace; font-size:0.7rem; color:var(--blue); text-transform:uppercase; letter-spacing:1px; }
.clause-text { font-size:0.87rem; color:var(--text); margin-top:4px; line-height:1.5; }

.risk-high   { border-left: 3px solid var(--red) !important;  background: rgba(239,68,68,0.06) !important; }
.risk-medium { border-left: 3px solid var(--amber) !important; background: rgba(245,158,11,0.06) !important; }
.risk-low    { border-left: 3px solid var(--green) !important; background: rgba(34,197,94,0.06) !important; }

.risk-badge-high   { font-size:0.7rem; font-weight:700; padding:2px 8px; border-radius:999px; color:#fecaca; background:rgba(239,68,68,0.15); border:1px solid rgba(239,68,68,0.4); }
.risk-badge-medium { font-size:0.7rem; font-weight:700; padding:2px 8px; border-radius:999px; color:#fef3c7; background:rgba(245,158,11,0.15); border:1px solid rgba(245,158,11,0.4); }
.risk-badge-low    { font-size:0.7rem; font-weight:700; padding:2px 8px; border-radius:999px; color:#bbf7d0; background:rgba(34,197,94,0.15); border:1px solid rgba(34,197,94,0.4); }

.simple-card {
  background: rgba(13,20,32,0.9);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 14px 16px;
  margin-bottom: 10px;
}
.original-text { font-size:0.83rem; color:var(--muted); font-style:italic; margin-bottom:6px; }
.simple-text   { font-size:0.9rem; color:var(--text); line-height:1.6; }
.arrow-badge   { font-size:0.72rem; color:var(--gold); background:rgba(201,168,76,0.1); border:1px solid var(--gold-dim); border-radius:999px; padding:1px 8px; display:inline-block; margin-bottom:6px; }

.dash-stat {
  background: rgba(10,15,25,0.9);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 16px;
  text-align: center;
}
.dash-stat-num { font-family:'Playfair Display',serif; font-size:2rem; font-weight:700; color:var(--gold); }
.dash-stat-label { font-size:0.78rem; color:var(--muted); margin-top:2px; }

.risk-gauge {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 6px 14px;
  border-radius: 999px;
  font-size: 0.85rem;
  font-weight: 700;
}
.gauge-high   { background: rgba(239,68,68,0.15);  color:#f87171; border:1px solid rgba(239,68,68,0.4); }
.gauge-medium { background: rgba(245,158,11,0.15); color:#fbbf24; border:1px solid rgba(245,158,11,0.4); }
.gauge-low    { background: rgba(34,197,94,0.15);  color:#4ade80; border:1px solid rgba(34,197,94,0.4); }

.note-muted { color: var(--muted); font-size: 0.82rem; }
</style>
""", unsafe_allow_html=True)

# ──────────────────────────────────────────────
# STATE
# ──────────────────────────────────────────────
def init_state() -> None:
    defaults = {
        "docs_indexed": 0,
        "chunks_stored": 0,
        "last_query_time": None,
        "chat_history": [],
        "last_sources": [],
        "current_doc_text": "",
        "current_doc_name": "",
        "extracted_clauses": [],
        "risk_report": [],
        "simplified_clauses": [],
        "doc_summary": {},
    }
    for k, v in defaults.items():
        if k not in st.session_state:
            st.session_state[k] = v

# ──────────────────────────────────────────────
# ENDEE HELPERS
# ──────────────────────────────────────────────
def endee_headers(json_ct: bool = False):
    h = {}
    if ENDEE_AUTH_TOKEN:
        h["Authorization"] = ENDEE_AUTH_TOKEN
    if json_ct:
        h["Content-Type"] = "application/json"
    return h or None

def is_endee_available() -> bool:
    try:
        r = requests.get(f"{ENDEE_URL}/api/v1/health", headers=endee_headers(), timeout=2)
        return r.ok
    except Exception:
        return False

@st.cache_resource
def load_embedding_model():
    from sentence_transformers import SentenceTransformer
    return SentenceTransformer(EMBEDDING_MODEL)

def create_index() -> bool:
    try:
        r = requests.post(
            f"{ENDEE_URL}/api/v1/index/create",
            json={"index_name": INDEX_NAME, "dim": 384, "space_type": "cosine",
                  "precision": "float32", "M": 16, "ef_con": 200},
            headers=endee_headers(True), timeout=10,
        )
        return r.ok
    except Exception:
        return False

def delete_index() -> bool:
    try:
        r = requests.delete(f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/delete",
                            headers=endee_headers(), timeout=10)
        return r.status_code in (200, 404)
    except Exception:
        return False

def is_missing_files_error(r: requests.Response) -> bool:
    return r.status_code == 400 and "required files missing for index" in r.text.lower()

def chunk_text(text: str, chunk_size: int = 500):
    words = text.split()
    chunks = []
    for i in range(0, len(words), chunk_size):
        c = " ".join(words[i: i + chunk_size])
        if c.strip():
            chunks.append(c)
    return chunks

def ingest_document(text: str, model, doc_name: str, chunk_size: int) -> int:
    chunks = chunk_text(text, chunk_size)
    if not chunks:
        st.error("No chunks created."); return 0
    embeddings = model.encode(chunks, normalize_embeddings=True)
    seed = f"{doc_name}-{int(time.time())}"
    vectors = [
        {"id": f"{seed}-{i}", "vector": emb.astype(np.float32).tolist(),
         "meta": json.dumps({"doc": doc_name, "text": ch})}
        for i, (ch, emb) in enumerate(zip(chunks, embeddings))
    ]
    try:
        r = requests.post(f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/vector/insert",
                          json=vectors, headers=endee_headers(True), timeout=45)
        if r.ok:
            return len(chunks)
        if is_missing_files_error(r):
            delete_index()
            if create_index():
                r2 = requests.post(f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/vector/insert",
                                   json=vectors, headers=endee_headers(True), timeout=45)
                if r2.ok:
                    return len(chunks)
        st.error(f"Endee error: {r.status_code} – {r.text[:200]}")
        return 0
    except Exception as e:
        st.error(f"Insert failed: {e}"); return 0

def parse_meta(mv):
    if isinstance(mv, bytes):
        mv = mv.decode("utf-8", errors="replace")
    if isinstance(mv, dict):
        return str(mv.get("doc", "Document")), str(mv.get("text", ""))
    if isinstance(mv, str):
        try:
            p = json.loads(mv)
            if isinstance(p, dict):
                return str(p.get("doc", "Document")), str(p.get("text", ""))
        except Exception:
            return "Document", mv
    return "Document", str(mv)

def search_similar(question: str, model, top_k: int = 5):
    qe = model.encode([question])[0]
    try:
        r = requests.post(
            f"{ENDEE_URL}/api/v1/index/{INDEX_NAME}/search",
            json={"vector": qe.astype(np.float32).tolist(), "k": top_k},
            headers=endee_headers(True), timeout=20,
        )
        if not r.ok:
            return []
        raw = msgpack.unpackb(r.content, raw=False)
        sources = []
        if isinstance(raw, list):
            for item in raw:
                score, meta_val = None, ""
                if isinstance(item, dict):
                    meta_val = item.get("meta", "")
                    score    = item.get("score", item.get("distance"))
                elif isinstance(item, list) and len(item) > 2:
                    score    = item[1]
                    meta_val = item[2]
                else:
                    continue
                doc_name, text = parse_meta(meta_val)
                sources.append({"doc": doc_name, "text": text, "score": score})
        return sources
    except Exception as e:
        st.error(f"Search failed: {e}"); return []

# ──────────────────────────────────────────────
# GROQ HELPERS
# ──────────────────────────────────────────────
def get_groq_client():
    if not GROQ_API_KEY or GROQ_API_KEY == "your_groq_api_key_here":
        return None
    return Groq(api_key=GROQ_API_KEY)

def llm_call(prompt: str, temperature: float = 0.2, max_tokens: int = 1024) -> str:
    client = get_groq_client()
    if not client:
        return "⚠️ GROQ_API_KEY missing."
    resp = client.chat.completions.create(
        model="llama-3.3-70b-versatile",
        messages=[{"role": "user", "content": prompt}],
        temperature=temperature,
        max_tokens=max_tokens,
    )
    return resp.choices[0].message.content

def generate_answer(question: str, context: str, temperature: float) -> str:
    prompt = f"""You are a legal document assistant. Use only the context below to answer.
Context:
{context}

Question: {question}

Answer with precision. If the context doesn't contain the answer, say so."""
    return llm_call(prompt, temperature)

# ──────────────────────────────────────────────
# CLAUSE EXTRACTION
# ──────────────────────────────────────────────
CLAUSE_KEYWORDS = {
    "termination":    ["terminat", "cancel", "end of agreement", "expir"],
    "payment":        ["payment", "fee", "invoice", "billing", "price", "cost", "rate"],
    "liability":      ["liabil", "indemnif", "damages", "loss", "harm"],
    "confidentiality":["confidential", "nda", "non-disclosure", "proprietary", "secret"],
    "intellectual_property": ["intellectual property", "ip", "copyright", "patent", "trademark", "ownership"],
    "dispute_resolution": ["arbitration", "mediation", "dispute", "jurisdiction", "governing law"],
    "non_compete":    ["non-compete", "non compete", "competing", "restraint of trade"],
    "warranty":       ["warrant", "guarantee", "represent", "assur"],
    "force_majeure":  ["force majeure", "act of god", "unforeseen", "circumstances beyond"],
    "limitation_of_liability": ["limitation of liability", "limit.*liab", "maximum liability", "cap on"],
    "assignment":     ["assign", "transfer", "delegate", "novat"],
    "renewal":        ["renew", "auto-renew", "extension", "rollover"],
}

def extract_clauses_rule_based(text: str) -> list:
    paragraphs = [p.strip() for p in text.split("\n") if len(p.strip()) > 40]
    results = []
    seen = set()
    for para in paragraphs:
        para_lower = para.lower()
        for clause_type, keywords in CLAUSE_KEYWORDS.items():
            for kw in keywords:
                if kw in para_lower and para[:60] not in seen:
                    seen.add(para[:60])
                    results.append({
                        "type": clause_type.replace("_", " ").title(),
                        "text": para[:600],
                        "keyword_matched": kw,
                    })
                    break
    return results[:30]

def extract_clauses_llm(text: str) -> list:
    prompt = f"""You are a legal analyst. Extract all important clauses from this document.
Return a JSON array where each element is:
{{"type": "Clause Type", "text": "exact text from document", "summary": "brief summary"}}

Only return the JSON array, no other text.

Document:
{text[:6000]}
"""
    raw = llm_call(prompt, temperature=0.1, max_tokens=2000)
    try:
        raw = raw.strip().lstrip("```json").lstrip("```").rstrip("```").strip()
        return json.loads(raw)
    except Exception:
        return []

# ──────────────────────────────────────────────
# RISK DETECTION
# ──────────────────────────────────────────────
RISK_RULES = {
    "Missing termination clause":        (["terminat", "cancel"], "high",   "No termination clause found — either party may be locked indefinitely."),
    "Missing limitation of liability":   (["limitation of liability", "limit.*liab"], "high", "No liability cap found — exposure could be unlimited."),
    "Missing confidentiality clause":    (["confidential", "non-disclosure"], "medium", "No confidentiality protections — sensitive info may not be protected."),
    "Missing dispute resolution clause": (["arbitration", "mediation", "dispute"], "medium", "No dispute resolution mechanism — litigation by default."),
    "Missing payment terms":             (["payment", "fee", "invoice"], "medium", "No clear payment terms — financial obligations are ambiguous."),
    "Missing warranty clause":           (["warrant", "guarantee"], "low", "No warranty provisions — service quality undefined."),
    "Missing force majeure":             (["force majeure", "act of god"], "low", "No force majeure clause — parties may be liable for unforeseeable events."),
    "Missing renewal terms":             (["renew", "auto-renew", "extension"], "low", "No renewal clause — agreement terms on expiry are unclear."),
}

RISKY_TERMS = [
    ("sole discretion",       "high",   "'Sole discretion' grants unilateral power — one-sided."),
    ("irrevocable",           "high",   "'Irrevocable' terms cannot be undone — high commitment risk."),
    ("unlimited liability",   "high",   "Explicit unlimited liability — extremely risky."),
    ("indemnify and hold harmless", "high", "Broad indemnification — may cover third-party claims."),
    ("automatically renews",  "medium", "Auto-renewal without active consent — review cancellation window."),
    ("non-refundable",        "medium", "Non-refundable payments even if service fails."),
    ("assigns this agreement","medium", "Agreement can be transferred without consent."),
    ("waive any claim",       "high",   "Broad claim waiver — may forfeit legal rights."),
    ("as is",                 "medium", "No warranty expressed — take what you get."),
]

def detect_risks(text: str, clauses: list) -> list:
    risks = []
    text_lower = text.lower()
    clause_types_lower = [c.get("type", "").lower() for c in clauses]

    # Rule-based missing clause checks
    for risk_name, (keywords, level, explanation) in RISK_RULES.items():
        found = any(kw in text_lower for kw in keywords)
        clause_found = any(kw in " ".join(clause_types_lower) for kw in [keywords[0]])
        if not found and not clause_found:
            risks.append({
                "name": risk_name,
                "level": level,
                "explanation": explanation,
                "type": "missing_clause",
                "text_excerpt": "",
            })

    # Risky term scan
    for term, level, explanation in RISKY_TERMS:
        idx = text_lower.find(term)
        if idx != -1:
            start = max(0, idx - 60)
            end   = min(len(text), idx + len(term) + 60)
            excerpt = "…" + text[start:end] + "…"
            risks.append({
                "name": f"Risky term: '{term}'",
                "level": level,
                "explanation": explanation,
                "type": "risky_term",
                "text_excerpt": excerpt,
            })

    return risks

def enrich_risk_with_llm(risks: list, text: str) -> list:
    if not risks or not get_groq_client():
        return risks
    risk_names = [r["name"] for r in risks[:8]]
    prompt = f"""You are a legal risk analyst. For each risk below, provide a short (1-2 sentence) 
professional explanation of why it matters and what the consequence could be.

Risks identified:
{json.dumps(risk_names)}

Document excerpt:
{text[:3000]}

Return a JSON object mapping each risk name to its explanation.
Return only JSON, no other text."""
    try:
        raw = llm_call(prompt, temperature=0.1, max_tokens=1000)
        raw = raw.strip().lstrip("```json").lstrip("```").rstrip("```").strip()
        enrichments = json.loads(raw)
        for r in risks:
            if r["name"] in enrichments:
                r["llm_explanation"] = enrichments[r["name"]]
    except Exception:
        pass
    return risks

# ──────────────────────────────────────────────
# SIMPLIFICATION
# ──────────────────────────────────────────────
def simplify_clauses(clauses: list) -> list:
    if not clauses or not get_groq_client():
        return []
    items = [{"type": c.get("type",""), "text": c.get("text","")[:400]} for c in clauses[:12]]
    prompt = f"""You are a plain-English legal translator for non-lawyers.
For each clause below, rewrite it in simple, everyday English (1-3 sentences max).

Clauses:
{json.dumps(items)}

Return a JSON array where each element has:
{{"type": "Clause Type", "original": "first 100 chars", "simple": "plain English version"}}
Return only the JSON array."""
    try:
        raw = llm_call(prompt, temperature=0.3, max_tokens=2000)
        raw = raw.strip().lstrip("```json").lstrip("```").rstrip("```").strip()
        return json.loads(raw)
    except Exception:
        return []

# ──────────────────────────────────────────────
# DOCUMENT SUMMARY
# ──────────────────────────────────────────────
def build_summary(text: str, clauses: list, risks: list) -> dict:
    high_risks   = [r for r in risks if r["level"] == "high"]
    medium_risks = [r for r in risks if r["level"] == "medium"]
    low_risks    = [r for r in risks if r["level"] == "low"]

    if len(high_risks) >= 3:
        overall_risk = "HIGH"
    elif len(high_risks) >= 1 or len(medium_risks) >= 3:
        overall_risk = "MEDIUM"
    else:
        overall_risk = "LOW"

    llm_summary = ""
    if get_groq_client():
        prompt = f"""Summarize this legal document in 3-4 sentences. Be concise and factual.
Document:
{text[:4000]}"""
        llm_summary = llm_call(prompt, temperature=0.2, max_tokens=300)

    return {
        "clause_count": len(clauses),
        "risk_count": len(risks),
        "high_risk_count": len(high_risks),
        "medium_risk_count": len(medium_risks),
        "low_risk_count": len(low_risks),
        "overall_risk": overall_risk,
        "word_count": len(text.split()),
        "llm_summary": llm_summary,
        "clause_types": list({c.get("type","") for c in clauses}),
    }

# ──────────────────────────────────────────────
# PDF EXPORT
# ──────────────────────────────────────────────
def generate_pdf_report(doc_name: str, summary: dict, clauses: list, risks: list, simplified: list) -> bytes:
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
    from reportlab.lib.colors import HexColor, white, black
    from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, HRFlowable
    from reportlab.lib.units import cm
    from reportlab.lib.enums import TA_LEFT, TA_CENTER, TA_RIGHT

    buf = io.BytesIO()
    doc = SimpleDocTemplate(buf, pagesize=A4,
                            leftMargin=2*cm, rightMargin=2*cm,
                            topMargin=2.5*cm, bottomMargin=2*cm)

    DARK      = HexColor("#06090f")
    SURFACE   = HexColor("#0d1420")
    GOLD      = HexColor("#c9a84c")
    BLUE      = HexColor("#3b7dd8")
    RED       = HexColor("#ef4444")
    AMBER     = HexColor("#f59e0b")
    GREEN     = HexColor("#22c55e")
    MUTED     = HexColor("#8899b4")
    TEXT      = HexColor("#e8edf5")
    WHITE     = white

    styles = getSampleStyleSheet()

    def ps(name, **kwargs):
        return ParagraphStyle(name, **kwargs)

    title_style    = ps("T", fontSize=22, textColor=GOLD, spaceAfter=4, fontName="Helvetica-Bold")
    sub_style      = ps("S", fontSize=10, textColor=MUTED, spaceAfter=16, fontName="Helvetica")
    h2_style       = ps("H2", fontSize=14, textColor=GOLD, spaceAfter=8, spaceBefore=16, fontName="Helvetica-Bold")
    body_style     = ps("B", fontSize=9, textColor=TEXT, spaceAfter=6, fontName="Helvetica", leading=14)
    clause_style   = ps("C", fontSize=8.5, textColor=MUTED, spaceAfter=4, fontName="Helvetica", leading=13)
    label_style    = ps("L", fontSize=8, textColor=GOLD, fontName="Helvetica-Bold")
    risk_h_style   = ps("RH", fontSize=9, textColor=RED,   fontName="Helvetica-Bold")
    risk_m_style   = ps("RM", fontSize=9, textColor=AMBER, fontName="Helvetica-Bold")
    risk_l_style   = ps("RL", fontSize=9, textColor=GREEN, fontName="Helvetica-Bold")

    story = []

    # Title block
    story.append(Paragraph("⚖ Legal Intelligence Report", title_style))
    story.append(Paragraph(f"Document: {doc_name}  |  Generated: {time.strftime('%B %d, %Y')}", sub_style))
    story.append(HRFlowable(width="100%", thickness=1, color=GOLD, spaceAfter=16))

    # Summary stats table
    risk_color = {"HIGH": RED, "MEDIUM": AMBER, "LOW": GREEN}.get(summary.get("overall_risk","LOW"), GREEN)
    stat_data = [
        [Paragraph("OVERALL RISK", label_style),
         Paragraph("CLAUSES", label_style),
         Paragraph("RISKS FOUND", label_style),
         Paragraph("WORD COUNT", label_style)],
        [Paragraph(f"<font color='#{risk_color.hexval()[2:]}' size='12'><b>{summary.get('overall_risk','–')}</b></font>", body_style),
         Paragraph(str(summary.get("clause_count","–")), body_style),
         Paragraph(str(summary.get("risk_count","–")), body_style),
         Paragraph(str(summary.get("word_count","–")), body_style)],
    ]
    stat_tbl = Table(stat_data, colWidths=["25%","25%","25%","25%"])
    stat_tbl.setStyle(TableStyle([
        ("BACKGROUND",   (0,0),(-1,0), SURFACE),
        ("BACKGROUND",   (0,1),(-1,1), DARK),
        ("TEXTCOLOR",    (0,0),(-1,-1), TEXT),
        ("GRID",         (0,0),(-1,-1), 0.3, MUTED),
        ("PADDING",      (0,0),(-1,-1), 10),
        ("ALIGN",        (0,0),(-1,-1), "CENTER"),
        ("ROUNDEDCORNERS", [4]),
    ]))
    story.append(stat_tbl)
    story.append(Spacer(1, 12))

    # LLM Summary
    if summary.get("llm_summary"):
        story.append(Paragraph("Document Summary", h2_style))
        story.append(Paragraph(summary["llm_summary"], body_style))

    # Clauses
    if clauses:
        story.append(Paragraph("Extracted Clauses", h2_style))
        for c in clauses[:20]:
            story.append(Paragraph(f"<b>[{c.get('type','')}]</b>", label_style))
            text_val = c.get("text", c.get("summary",""))[:400]
            story.append(Paragraph(text_val, clause_style))
            story.append(Spacer(1, 4))

    # Risks
    if risks:
        story.append(Paragraph("Risk Analysis", h2_style))
        for r in risks:
            lvl = r.get("level","low")
            rstyle = {"high": risk_h_style, "medium": risk_m_style, "low": risk_l_style}.get(lvl, risk_l_style)
            badge = {"high": "🔴 HIGH", "medium": "🟡 MEDIUM", "low": "🟢 LOW"}.get(lvl,"🟢 LOW")
            story.append(Paragraph(f"{badge}  {r['name']}", rstyle))
            exp = r.get("llm_explanation") or r.get("explanation","")
            story.append(Paragraph(exp, clause_style))
            if r.get("text_excerpt"):
                story.append(Paragraph(f"<i>{r['text_excerpt'][:200]}</i>", clause_style))
            story.append(Spacer(1, 4))

    # Simplified
    if simplified:
        story.append(Paragraph("Plain-English Summary", h2_style))
        for s in simplified[:12]:
            story.append(Paragraph(f"<b>{s.get('type','')}</b>", label_style))
            story.append(Paragraph(s.get("simple",""), body_style))
            story.append(Spacer(1, 4))

    doc.build(story)
    buf.seek(0)
    return buf.read()

# ──────────────────────────────────────────────
# UI COMPONENTS
# ──────────────────────────────────────────────
def status_badge(label: str, ok: bool) -> str:
    color = "#22c55e" if ok else "#ef4444"
    pill  = "pill-ok" if ok else "pill-off"
    txt   = "LIVE" if ok else "OFF"
    return (f"<div class='status-row'>"
            f"<div style='display:flex;align-items:center;'>"
            f"<span class='status-dot' style='background:{color}'></span>"
            f"<span style='font-size:.85rem'>{html.escape(label)}</span></div>"
            f"<span class='pill {pill}'>{txt}</span></div>")

def render_header() -> None:
    st.markdown("""
<div class="hero-wrap">
  <div class="hero-eyebrow">⚖ Legal Intelligence Platform</div>
  <h1 class="hero-title">Document <span>Analysis</span> Suite</h1>
  <p class="hero-sub">Powered by Endee Vector DB · Groq LLM · RAG Architecture</p>
  <hr class="hero-rule">
</div>
""", unsafe_allow_html=True)

def render_sidebar(endee_ok: bool) -> dict:
    with st.sidebar:
        st.markdown("### System Status")
        st.markdown(status_badge("Vector DB (Endee)", endee_ok), unsafe_allow_html=True)
        st.markdown(status_badge("LLM (Groq)", bool(GROQ_API_KEY and GROQ_API_KEY != "your_groq_api_key_here")), unsafe_allow_html=True)
        st.markdown(status_badge("Document Loaded", bool(st.session_state.current_doc_text)), unsafe_allow_html=True)
        st.markdown("---")
        st.markdown("### Settings")
        top_k      = st.slider("Retrieval Top-K", 1, 8, 4)
        temperature= st.slider("Temperature", 0.0, 1.0, 0.2, 0.05)
        chunk_size = st.slider("Chunk Size (words)", 200, 900, 500, 50)
        st.markdown("---")
        if st.button("🔄 Initialize Index"):
            if create_index():
                st.success("Index ready")
            else:
                st.error("Failed — is Endee running?")
        st.markdown(f"<div class='note-muted'>Index: <b>{INDEX_NAME}</b><br>Endee: <code>{ENDEE_URL}</code></div>", unsafe_allow_html=True)
    return {"top_k": top_k, "temperature": temperature, "chunk_size": chunk_size}

def render_upload_tab(endee_ok: bool, settings: dict):
    st.markdown("#### Upload Legal Document")
    col_l, col_r = st.columns([2, 1])
    with col_l:
        uploaded = st.file_uploader("Upload .txt or .pdf", type=["txt", "pdf"])
    with col_r:
        st.markdown("<br>", unsafe_allow_html=True)
        if st.session_state.current_doc_name:
            st.success(f"✓ {st.session_state.current_doc_name}")

    if uploaded:
        if uploaded.name.endswith(".pdf"):
            try:
                from pypdf import PdfReader
                reader = PdfReader(io.BytesIO(uploaded.getvalue()))
                text = "\n".join(page.extract_text() or "" for page in reader.pages)
            except Exception as e:
                st.error(f"PDF read error: {e}"); return
        else:
            text = uploaded.getvalue().decode("utf-8", errors="replace")

        est_chunks = len(chunk_text(text, settings["chunk_size"]))
        c1, c2, c3 = st.columns(3)
        c1.metric("Characters", f"{len(text):,}")
        c2.metric("Words", f"{len(text.split()):,}")
        c3.metric("Est. Chunks", est_chunks)

        with st.expander("📄 Preview (first 1000 chars)"):
            st.text(text[:1000] + ("…" if len(text) > 1000 else ""))

        if st.button("⬆ Ingest to Vector DB", disabled=not endee_ok):
            model = load_embedding_model()
            with st.status("Ingesting document…", expanded=True) as s:
                s.write("Chunking text…")
                s.write("Generating embeddings…")
                count = ingest_document(text, model, uploaded.name, settings["chunk_size"])
                if count > 0:
                    st.session_state.docs_indexed    += 1
                    st.session_state.chunks_stored   += count
                    st.session_state.current_doc_text = text
                    st.session_state.current_doc_name = uploaded.name
                    s.update(label="✓ Ingestion complete", state="complete", expanded=False)
                    st.success(f"Stored {count} chunks.")
                else:
                    s.update(label="✗ Ingestion failed", state="error")

        if not endee_ok:
            st.warning("Vector DB offline — document stored locally only.")
            st.session_state.current_doc_text = text
            st.session_state.current_doc_name = uploaded.name

def render_qa_tab(endee_ok: bool, settings: dict):
    st.markdown("#### Ask Questions — with Source Highlighting")
    question = st.text_area("Your question", height=100, placeholder="e.g. What is the termination notice period?")
    if st.button("🔍 Search & Answer", disabled=not question.strip()):
        model   = load_embedding_model()
        t_start = time.perf_counter()
        with st.status("Searching knowledge base…", expanded=True) as s:
            s.write("Embedding question…")
            sources = search_similar(question.strip(), model, settings["top_k"])
            s.write("Generating answer…")
            context = "\n\n".join(src["text"] for src in sources)
            answer  = generate_answer(question.strip(), context, settings["temperature"])
            elapsed = time.perf_counter() - t_start
            st.session_state.last_query_time = elapsed
            st.session_state.last_sources    = sources
            s.update(label="✓ Done", state="complete", expanded=False)

        st.session_state.chat_history.append({"role": "user",      "content": question.strip()})
        st.session_state.chat_history.append({"role": "assistant", "content": answer, "sources": sources})

    # Chat display
    for msg in st.session_state.chat_history:
        with st.chat_message(msg["role"]):
            st.markdown(msg["content"])

    # Source highlighting
    if st.session_state.last_sources:
        st.markdown("#### 📎 Source Highlights")
        for i, src in enumerate(st.session_state.last_sources, 1):
            score   = src.get("score")
            score_s = f"{score:.4f}" if isinstance(score, (int, float)) else "N/A"
            snippet = html.escape(src.get("text","")[:400])
            doc     = html.escape(str(src.get("doc","Document")))
            # Highlight any query words in the snippet
            if st.session_state.chat_history:
                last_q = next((m["content"] for m in reversed(st.session_state.chat_history) if m["role"]=="user"), "")
                for word in last_q.split():
                    if len(word) > 3:
                        snippet = snippet.replace(
                            html.escape(word),
                            f"<span class='highlight'>{html.escape(word)}</span>",
                        )
            st.markdown(f"""
<div class="source-card">
  <div class="source-head">
    <span class="source-rank">Source {i} — {doc}</span>
    <span class="source-score">Score: {score_s}</span>
  </div>
  <div class="source-snippet">{snippet}</div>
</div>""", unsafe_allow_html=True)

def render_clauses_tab():
    st.markdown("#### 🧾 Clause Extraction Engine")
    text = st.session_state.current_doc_text
    if not text:
        st.info("Upload a document first."); return

    col1, col2 = st.columns(2)
    with col1:
        if st.button("📋 Extract Clauses (Rule-Based)"):
            clauses = extract_clauses_rule_based(text)
            st.session_state.extracted_clauses = clauses
    with col2:
        if st.button("🧠 Extract Clauses (LLM-Enhanced)"):
            with st.spinner("LLM extracting…"):
                clauses = extract_clauses_llm(text)
                if not clauses:
                    clauses = extract_clauses_rule_based(text)
                st.session_state.extracted_clauses = clauses

    clauses = st.session_state.extracted_clauses
    if clauses:
        st.markdown(f"**Found {len(clauses)} clauses**")
        for c in clauses:
            ctype   = html.escape(c.get("type","Unknown"))
            ctext   = html.escape(c.get("text", c.get("summary",""))[:500])
            csummary= html.escape(c.get("summary",""))
            st.markdown(f"""
<div class="clause-card">
  <div class="clause-type">{ctype}</div>
  <div class="clause-text">{ctext}</div>
  {f'<div style="color:#8899b4;font-size:.8rem;margin-top:4px">{csummary}</div>' if csummary else ''}
</div>""", unsafe_allow_html=True)

def render_risk_tab():
    st.markdown("#### ⚠️ Risk Detection System")
    text = st.session_state.current_doc_text
    if not text:
        st.info("Upload a document first."); return

    if st.button("🔎 Run Risk Analysis"):
        clauses = st.session_state.extracted_clauses or extract_clauses_rule_based(text)
        risks   = detect_risks(text, clauses)
        with st.spinner("Enriching with LLM explanations…"):
            risks = enrich_risk_with_llm(risks, text)
        st.session_state.risk_report = risks

    risks = st.session_state.risk_report
    if risks:
        high   = [r for r in risks if r["level"]=="high"]
        medium = [r for r in risks if r["level"]=="medium"]
        low    = [r for r in risks if r["level"]=="low"]

        c1, c2, c3 = st.columns(3)
        c1.metric("🔴 High Risk",   len(high))
        c2.metric("🟡 Medium Risk", len(medium))
        c3.metric("🟢 Low Risk",    len(low))

        for r in sorted(risks, key=lambda x: {"high":0,"medium":1,"low":2}[x["level"]]):
            lvl         = r["level"]
            badge_class = f"risk-badge-{lvl}"
            card_class  = f"clause-card risk-{lvl}"
            exp         = html.escape(r.get("llm_explanation") or r.get("explanation",""))
            excerpt     = html.escape(r.get("text_excerpt","")[:200])
            st.markdown(f"""
<div class="{card_class}">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px">
    <span style="font-weight:600;font-size:.9rem">{html.escape(r['name'])}</span>
    <span class="{badge_class}">{lvl.upper()}</span>
  </div>
  <div class="clause-text">{exp}</div>
  {f'<div style="font-size:.78rem;color:#8899b4;margin-top:6px;font-style:italic">{excerpt}</div>' if excerpt else ''}
</div>""", unsafe_allow_html=True)

def render_simplify_tab():
    st.markdown("#### 🧠 Plain-English Simplification")
    text    = st.session_state.current_doc_text
    clauses = st.session_state.extracted_clauses
    if not text:
        st.info("Upload a document first."); return
    if not clauses:
        st.warning("Run Clause Extraction first for better results.")

    if st.button("✨ Simplify All Clauses"):
        if not clauses:
            clauses = extract_clauses_rule_based(text)
        with st.spinner("Translating legalese to plain English…"):
            simplified = simplify_clauses(clauses)
            st.session_state.simplified_clauses = simplified

    simplified = st.session_state.simplified_clauses
    if simplified:
        st.markdown(f"**{len(simplified)} clauses simplified**")
        for s in simplified:
            orig   = html.escape(str(s.get("original",""))[:120])
            simple = html.escape(str(s.get("simple","")))
            ctype  = html.escape(str(s.get("type","")))
            st.markdown(f"""
<div class="simple-card">
  <div class="clause-type">{ctype}</div>
  <div class="original-text">Original: {orig}…</div>
  <div class="arrow-badge">Plain English ↓</div>
  <div class="simple-text">{simple}</div>
</div>""", unsafe_allow_html=True)

def render_dashboard_tab():
    st.markdown("#### 📊 Document Summary Dashboard")
    text = st.session_state.current_doc_text
    if not text:
        st.info("Upload a document first."); return

    if st.button("📈 Generate Dashboard"):
        clauses = st.session_state.extracted_clauses or extract_clauses_rule_based(text)
        risks   = st.session_state.risk_report or detect_risks(text, clauses)
        with st.spinner("Building summary…"):
            summary = build_summary(text, clauses, risks)
        st.session_state.doc_summary = summary
        st.session_state.extracted_clauses = clauses
        st.session_state.risk_report       = risks

    summary = st.session_state.doc_summary
    if summary:
        risk_gauge_class = {"HIGH":"gauge-high","MEDIUM":"gauge-medium","LOW":"gauge-low"}.get(summary.get("overall_risk","LOW"),"gauge-low")
        risk_icon        = {"HIGH":"🔴","MEDIUM":"🟡","LOW":"🟢"}.get(summary.get("overall_risk","LOW"),"🟢")
        st.markdown(f"""
<div style="margin-bottom:16px">
  <span class="risk-gauge {risk_gauge_class}">{risk_icon} Overall Risk: {summary.get('overall_risk','–')}</span>
</div>""", unsafe_allow_html=True)

        c1, c2, c3, c4 = st.columns(4)
        for col, num, label in zip([c1,c2,c3,c4],
            [summary.get("clause_count",0), summary.get("risk_count",0),
             summary.get("word_count",0), summary.get("high_risk_count",0)],
            ["Clauses Found","Risks Detected","Document Words","High Risks"]):
            with col:
                st.markdown(f"""<div class="dash-stat">
  <div class="dash-stat-num">{num}</div>
  <div class="dash-stat-label">{label}</div>
</div>""", unsafe_allow_html=True)

        if summary.get("llm_summary"):
            st.markdown("---")
            st.markdown("**AI Summary**")
            st.info(summary["llm_summary"])

        if summary.get("clause_types"):
            st.markdown("**Clause Types Detected**")
            cols = st.columns(min(4, len(summary["clause_types"])))
            for i, ct in enumerate(summary["clause_types"]):
                cols[i % len(cols)].markdown(f"`{ct}`")

def render_export_tab():
    st.markdown("#### 📥 Export Report")
    text      = st.session_state.current_doc_text
    doc_name  = st.session_state.current_doc_name or "document"
    clauses   = st.session_state.extracted_clauses
    risks     = st.session_state.risk_report
    simplified= st.session_state.simplified_clauses
    summary   = st.session_state.doc_summary

    if not text:
        st.info("Upload and analyse a document first."); return

    missing = []
    if not clauses:   missing.append("clauses")
    if not risks:     missing.append("risks")
    if not summary:   missing.append("summary")
    if missing:
        st.warning(f"For a complete report, also run: {', '.join(missing)}")

    st.markdown("**What's included in the PDF:**")
    st.markdown("""
- Document summary & AI overview  
- All extracted clauses with types  
- Risk analysis with severity levels  
- Plain-English simplifications  
- Key statistics dashboard  
""")

    if st.button("📄 Generate & Download PDF Report"):
        if not summary:
            clauses  = clauses  or extract_clauses_rule_based(text)
            risks    = risks    or detect_risks(text, clauses)
            summary  = build_summary(text, clauses, risks)

        with st.spinner("Building PDF…"):
            try:
                pdf_bytes = generate_pdf_report(doc_name, summary, clauses, risks, simplified)
                st.download_button(
                    label="⬇ Download PDF",
                    data=pdf_bytes,
                    file_name=f"legal_report_{doc_name.replace(' ','_')}.pdf",
                    mime="application/pdf",
                )
                st.success("PDF ready — click above to download.")
            except ImportError:
                st.error("reportlab not installed. Run: pip install reportlab")
            except Exception as e:
                st.error(f"PDF generation failed: {e}")

# ──────────────────────────────────────────────
# MAIN
# ──────────────────────────────────────────────
def main() -> None:
    inject_theme()
    init_state()
    endee_ok = is_endee_available()
    settings = render_sidebar(endee_ok)
    render_header()

    # Metrics row
    c1, c2, c3, c4 = st.columns(4)
    c1.metric("Documents", st.session_state.docs_indexed)
    c2.metric("Chunks",    st.session_state.chunks_stored)
    qt = st.session_state.last_query_time
    c3.metric("Last Query", f"{qt:.2f}s" if isinstance(qt, float) else "–")
    c4.metric("Clauses",   len(st.session_state.extracted_clauses))

    st.markdown("")

    tabs = st.tabs([
        "📤 Upload",
        "💬 Q&A + Sources",
        "🧾 Clauses",
        "⚠️ Risks",
        "🧠 Simplify",
        "📊 Dashboard",
        "📥 Export",
    ])

    with tabs[0]: render_upload_tab(endee_ok, settings)
    with tabs[1]: render_qa_tab(endee_ok, settings)
    with tabs[2]: render_clauses_tab()
    with tabs[3]: render_risk_tab()
    with tabs[4]: render_simplify_tab()
    with tabs[5]: render_dashboard_tab()
    with tabs[6]: render_export_tab()

if __name__ == "__main__":
    main()