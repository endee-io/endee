"""
Streamlit RAG Application (app.py)
Full-featured UI with:
- PDF upload & ingestion
- Hybrid search + chat interface
- Live performance dashboard (WOW Feature #2)
- Conversation memory display
- Multi-document search
- Encryption status
"""
import os
import sys
import time
import json
import tempfile
import streamlit as st
import plotly.graph_objects as go
import plotly.express as px
from pathlib import Path

# ─── Page Config ─────────────────────────────────────────────────────────
st.set_page_config(
    page_title="EndeeRAG — Intelligent Document Q&A",
    page_icon="🧠",
    layout="wide",
    initial_sidebar_state="expanded",
)

# ─── Custom CSS ──────────────────────────────────────────────────────────
st.markdown("""
<style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap');

    .stApp {
        font-family: 'Inter', sans-serif;
    }

    .main-header {
        background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
        padding: 2rem;
        border-radius: 16px;
        margin-bottom: 2rem;
        color: white;
        text-align: center;
    }

    .main-header h1 {
        font-size: 2.5rem;
        font-weight: 700;
        margin: 0;
    }

    .main-header p {
        font-size: 1.1rem;
        opacity: 0.9;
        margin-top: 0.5rem;
    }

    .metric-card {
        background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
        border-radius: 12px;
        padding: 1.5rem;
        border: 1px solid rgba(255,255,255,0.1);
        text-align: center;
    }

    .metric-value {
        font-size: 2rem;
        font-weight: 700;
        color: #667eea;
    }

    .metric-label {
        font-size: 0.85rem;
        color: #a0aec0;
        margin-top: 0.3rem;
    }

    .citation-card {
        background: rgba(102, 126, 234, 0.08);
        border-left: 4px solid #667eea;
        padding: 1rem;
        border-radius: 0 8px 8px 0;
        margin: 0.5rem 0;
    }

    .chunk-card {
        background: rgba(56, 178, 172, 0.06);
        border-left: 4px solid #38b2ac;
        padding: 1rem 1.2rem;
        border-radius: 0 10px 10px 0;
        margin: 0.6rem 0;
        font-size: 0.9rem;
        line-height: 1.5;
    }

    .chunk-header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 0.4rem;
        font-size: 0.82rem;
    }

    .chunk-score {
        background: linear-gradient(135deg, #667eea, #764ba2);
        color: white;
        padding: 2px 10px;
        border-radius: 12px;
        font-weight: 600;
        font-size: 0.78rem;
    }

    .chunk-meta {
        color: #a0aec0;
        font-size: 0.78rem;
    }

    .chunk-text {
        color: #cdd5e0;
        font-size: 0.85rem;
        margin-top: 0.3rem;
        line-height: 1.55;
    }

    .success-banner {
        background: linear-gradient(135deg, rgba(72,187,120,0.15), rgba(56,178,172,0.10));
        border: 1px solid rgba(72,187,120,0.3);
        border-radius: 12px;
        padding: 1.2rem 1.5rem;
        margin: 0.5rem 0 1rem 0;
    }

    .search-mode-badge {
        display: inline-block;
        padding: 0.25rem 0.75rem;
        border-radius: 20px;
        font-size: 0.8rem;
        font-weight: 600;
    }

    .badge-hybrid { background: linear-gradient(135deg, #667eea, #764ba2); color: white; }
    .badge-dense { background: #38b2ac; color: white; }
    .badge-sparse { background: #ed8936; color: white; }

    .chat-message {
        padding: 1rem 1.5rem;
        border-radius: 12px;
        margin: 0.5rem 0;
    }

    .user-message {
        background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
        color: white;
        margin-left: 20%;
    }

    .assistant-message {
        background: rgba(255,255,255,0.05);
        border: 1px solid rgba(255,255,255,0.1);
        margin-right: 20%;
    }

    .status-indicator {
        display: inline-block;
        width: 10px;
        height: 10px;
        border-radius: 50%;
        margin-right: 6px;
    }

    .status-green { background: #48bb78; }
    .status-yellow { background: #ecc94b; }
    .status-red { background: #fc8181; }

    div[data-testid="stSidebar"] {
        background: linear-gradient(180deg, #1a1a2e 0%, #16213e 100%);
    }
</style>
""", unsafe_allow_html=True)


# ─── Session State Initialization ────────────────────────────────────────

def init_session_state():
    defaults = {
        "initialized": False,
        "ingestor": None,
        "rag_pipeline": None,
        "benchmark_runner": None,
        "chat_history": [],
        "ingested_docs": [],
        "benchmark_results": {},
        "performance_log": [],
        "search_mode": "hybrid",
        "top_k": 5,
        "encryption_enabled": True,
    }
    for key, value in defaults.items():
        if key not in st.session_state:
            st.session_state[key] = value


init_session_state()


# ─── Lazy Initialization ─────────────────────────────────────────────────

@st.cache_resource
def load_pipeline():
    """Initialize all components (cached for performance)."""
    from ingest import DocumentIngestor
    from rag import RAGPipeline
    from benchmarks import BenchmarkRunner

    ingestor = DocumentIngestor()
    rag = RAGPipeline()
    benchmark = BenchmarkRunner(retriever=rag.retriever, rag_pipeline=rag)

    return ingestor, rag, benchmark


def get_components():
    """Get or initialize pipeline components."""
    if not st.session_state.initialized:
        with st.spinner("🚀 Initializing EndeeRAG pipeline..."):
            ingestor, rag, benchmark = load_pipeline()
            st.session_state.ingestor = ingestor
            st.session_state.rag_pipeline = rag
            st.session_state.benchmark_runner = benchmark
            st.session_state.initialized = True

    return (
        st.session_state.ingestor,
        st.session_state.rag_pipeline,
        st.session_state.benchmark_runner,
    )


# ─── Header ──────────────────────────────────────────────────────────────

st.markdown("""
<div class="main-header">
    <h1>🧠 EndeeRAG</h1>
    <p>Production-Grade RAG System powered by Endee Vector Database</p>
    <p style="font-size: 0.85rem; opacity: 0.7;">
        Hybrid Search (Dense + Sparse + RRF) · Client-Side Encryption · Live Benchmarks · Conversation Memory
    </p>
</div>
""", unsafe_allow_html=True)


# ─── Sidebar ─────────────────────────────────────────────────────────────

with st.sidebar:
    st.markdown("## ⚙️ Settings")

    # Search Configuration
    st.markdown("### 🔍 Search Mode")
    st.session_state.search_mode = st.selectbox(
        "Mode",
        ["hybrid", "dense", "sparse"],
        index=0,
        help="Hybrid combines semantic (dense) and keyword (sparse) search via RRF fusion"
    )

    st.session_state.top_k = st.slider("Top-K Results", 1, 20, 5,
                                        help="Number of chunks to retrieve")

    # Encryption Toggle
    st.markdown("### 🔒 Encryption")
    st.session_state.encryption_enabled = st.toggle(
        "Client-Side Encryption", value=True,
        help="Encrypt document content before storing in Endee"
    )

    # System Status
    st.markdown("### 📊 System Status")
    col1, col2 = st.columns(2)
    with col1:
        status_color = "status-green" if st.session_state.initialized else "status-yellow"
        st.markdown(
            f'<span class="status-indicator {status_color}"></span> Pipeline',
            unsafe_allow_html=True
        )
    with col2:
        enc_status = "🔐 ON" if st.session_state.encryption_enabled else "🔓 OFF"
        st.markdown(f"Encryption: {enc_status}")

    st.markdown(f"📄 Documents: {len(st.session_state.ingested_docs)}")
    st.markdown(f"💬 Chat turns: {len(st.session_state.chat_history)}")

    # Document filter
    if st.session_state.ingested_docs:
        st.markdown("### 📁 Filter by Document")
        doc_options = ["All Documents"] + st.session_state.ingested_docs
        selected_doc = st.selectbox("Search in:", doc_options)
    else:
        selected_doc = "All Documents"

    # Clear actions
    st.markdown("---")
    if st.button("🗑️ Clear Chat History"):
        st.session_state.chat_history = []
        try:
            _, rag, _ = get_components()
            rag.clear_memory()
        except:
            pass
        st.rerun()


# ─── Main Content Tabs ──────────────────────────────────────────────────

tab_chat, tab_upload, tab_dashboard, tab_about = st.tabs([
    "💬 Chat", "📤 Upload Documents", "📊 Performance Dashboard", "ℹ️ About"
])


# ─── Tab 1: Chat Interface ──────────────────────────────────────────────

with tab_chat:
    # Display chat history
    for msg in st.session_state.chat_history:
        if msg["role"] == "user":
            with st.chat_message("user"):
                st.markdown(msg["content"])
        else:
            with st.chat_message("assistant"):
                st.markdown(msg["content"])

                # Show retrieved chunks (matches live response styling)
                if msg.get("citations"):
                    with st.expander(f"🔍 Retrieved Chunks ({len(msg['citations'])} sources)"):
                        for c in msg["citations"]:
                            sim_pct = f"{c['similarity'] * 100:.1f}%" if c['similarity'] <= 1 else f"{c['similarity']:.2f}"
                            st.markdown(f"""
<div class="chunk-card">
    <div class="chunk-header">
        <span><strong>📄 {c['filename']}</strong> &nbsp;·&nbsp; <span class="chunk-meta">Pages: {c['pages']}  |  Chunk&nbsp;ID: {c['chunk_id']}</span></span>
        <span class="chunk-score">Score: {sim_pct}</span>
    </div>
    <div class="chunk-text">{c['preview']}</div>
</div>
""", unsafe_allow_html=True)

                # Show performance metrics
                if msg.get("metrics"):
                    m = msg["metrics"]
                    cols = st.columns(4)
                    cols[0].metric("⚡ Retrieval", f"{m.get('retrieval_ms', 0):.0f}ms")
                    cols[1].metric("🧠 Generation", f"{m.get('generation_ms', 0):.0f}ms")
                    cols[2].metric("⏱️ Total", f"{m.get('total_ms', 0):.0f}ms")
                    cols[3].metric("🔍 Mode", m.get('mode', 'hybrid').upper())

    # Chat input
    if prompt := st.chat_input("Ask a question about your documents..."):
        # Guard: empty query
        if not prompt or not prompt.strip():
            st.warning("Please enter a question.")
        # Check if documents are ingested
        elif not st.session_state.ingested_docs:
            st.warning("📤 Please upload a document first in the 'Upload Documents' tab.")
        else:
            # Display user message
            st.session_state.chat_history.append({"role": "user", "content": prompt})
            with st.chat_message("user"):
                st.markdown(prompt)

            # Generate response
            with st.chat_message("assistant"):
                with st.spinner("🧠 Thinking..."):
                    try:
                        _, rag, _ = get_components()

                        # Build filters based on sidebar selection
                        filters = None
                        if selected_doc != "All Documents":
                            filters = [{"filename": {"$eq": selected_doc}}]

                        result = rag.query(
                            prompt,
                            mode=st.session_state.search_mode,
                            top_k=st.session_state.top_k,
                            filters=filters,
                        )

                        # Display answer
                        st.markdown(result["answer"])

                        # ── WOW Feature: Retrieved Chunks with scores ──
                        if result["citations"]:
                            with st.expander(
                                f"🔍 Retrieved Chunks ({len(result['citations'])} sources · "
                                f"{result['search_mode'].upper()} search)",
                                expanded=True,
                            ):
                                for c in result["citations"]:
                                    sim_pct = f"{c['similarity'] * 100:.1f}%" if c['similarity'] <= 1 else f"{c['similarity']:.2f}"
                                    st.markdown(f"""
<div class="chunk-card">
    <div class="chunk-header">
        <span><strong>📄 {c['filename']}</strong> &nbsp;·&nbsp; <span class="chunk-meta">Pages: {c['pages']}  |  Chunk&nbsp;ID: {c['chunk_id']}</span></span>
        <span class="chunk-score">Score: {sim_pct}</span>
    </div>
    <div class="chunk-text">{c['preview']}</div>
</div>
""", unsafe_allow_html=True)
                        else:
                            st.info("No relevant chunks were found. Try rephrasing your question or uploading more documents.")

                        # Performance metrics (latency WOW)
                        metrics = {
                            "retrieval_ms": result["retrieval_time_ms"],
                            "generation_ms": result["generation_time_ms"],
                            "total_ms": result["total_time_ms"],
                            "mode": result["search_mode"],
                        }
                        cols = st.columns(4)
                        cols[0].metric("⚡ Retrieval", f"{metrics['retrieval_ms']:.0f}ms")
                        cols[1].metric("🧠 Generation", f"{metrics['generation_ms']:.0f}ms")
                        cols[2].metric("⏱️ Total", f"{metrics['total_ms']:.0f}ms")
                        cols[3].metric("🔍 Mode", metrics['mode'].upper())

                        # Save to chat history
                        st.session_state.chat_history.append({
                            "role": "assistant",
                            "content": result["answer"],
                            "citations": result["citations"],
                            "metrics": metrics,
                        })

                        # Log performance
                        st.session_state.performance_log.append(metrics)

                    except Exception as e:
                        error_msg = str(e).encode("utf-8", errors="ignore").decode("utf-8")
                        st.error(f"Something went wrong: {error_msg}")
                        st.session_state.chat_history.append({
                            "role": "assistant", "content": f"Error: {error_msg}"
                        })


# ─── Tab 2: Document Upload ─────────────────────────────────────────────

with tab_upload:
    st.markdown("### 📤 Upload PDF Documents")
    st.markdown("Upload your documents to build the knowledge base. Documents are parsed, "
                "chunked, embedded (dense + sparse), and stored in Endee Vector Database.")

    uploaded_files = st.file_uploader(
        "Choose PDF files",
        type=["pdf"],
        accept_multiple_files=True,
        help="Upload one or more PDF documents"
    )

    if uploaded_files:
        if st.button("🚀 Ingest Documents", type="primary"):
            ingestor, rag, _ = get_components()

            for uploaded_file in uploaded_files:
                with st.spinner(f"⏳ Processing **{uploaded_file.name}** — parsing, chunking, embedding..."):
                    # Save to temp file
                    tmp_path = None
                    try:
                        with tempfile.NamedTemporaryFile(delete=False, suffix=".pdf") as tmp:
                            tmp.write(uploaded_file.getvalue())
                            tmp_path = tmp.name

                        result = ingestor.ingest_pdf(
                            tmp_path,
                            encrypt=st.session_state.encryption_enabled,
                            original_filename=uploaded_file.name
                        )

                        # ── Clear success feedback ──
                        pages_info = result["metadata"].get("parsed_pages", result["metadata"]["total_pages"])
                        skipped = result["metadata"].get("skipped_pages", [])
                        skip_note = f" ({len(skipped)} pages skipped)" if skipped else ""

                        st.markdown(f"""
<div class="success-banner">
    <h4 style="margin:0 0 0.3rem 0;">✅ Ingestion Successful</h4>
    <p style="margin:0; opacity:0.9;">
        <strong>{uploaded_file.name}</strong> processed in <strong>{result['total_time_ms']:.0f}ms</strong>
    </p>
    <p style="margin:0.3rem 0 0 0; font-size:0.9rem; opacity:0.8;">
        Pages processed: <strong>{pages_info}</strong>{skip_note} &nbsp;·&nbsp;
        Chunks stored: <strong>{result['stored_count']}</strong> / {result['chunks_count']} &nbsp;·&nbsp;
        Encrypted: <strong>{"Yes 🔐" if result.get('encrypted') else "No 🔓"}</strong>
    </p>
</div>
""", unsafe_allow_html=True)

                        # Display ingestion stats
                        col1, col2, col3, col4 = st.columns(4)
                        col1.metric("📄 Pages", pages_info)
                        col2.metric("🧩 Chunks", result["chunks_count"])
                        col3.metric("💾 Stored", result["stored_count"])
                        col4.metric("⏱️ Time", f"{result['total_time_ms']:.0f}ms")

                        with st.expander("📊 Detailed Ingestion Stats"):
                            st.json(result)

                        # Track ingested docs
                        if uploaded_file.name not in st.session_state.ingested_docs:
                            st.session_state.ingested_docs.append(uploaded_file.name)

                    except Exception as e:
                        error_msg = str(e).encode("utf-8", errors="ignore").decode("utf-8")
                        st.error(f"❌ Failed to process **{uploaded_file.name}**: {error_msg}")
                        st.info("💡 Tip: Ensure the PDF is not password-protected or corrupted.")
                    finally:
                        if tmp_path:
                            try:
                                os.unlink(tmp_path)
                            except OSError:
                                pass

    # Show ingested documents
    if st.session_state.ingested_docs:
        st.markdown("### 📚 Ingested Documents")
        for doc in st.session_state.ingested_docs:
            enc_badge = "🔐" if st.session_state.encryption_enabled else "🔓"
            st.markdown(f"- {enc_badge} **{doc}**")

    # Text input option
    st.markdown("---")
    st.markdown("### 📝 Or paste text directly")
    text_input = st.text_area("Paste your text here:", height=200)
    text_title = st.text_input("Title for this text:", value="Pasted Document")

    if text_input and st.button("📥 Ingest Text"):
        with st.spinner("Processing text..."):
            try:
                ingestor, _, _ = get_components()
                result = ingestor.ingest_text(
                    text_input,
                    title=text_title,
                    encrypt=st.session_state.encryption_enabled
                )
                st.success(f"✅ Text ingested: {result['chunks_count']} chunks stored")
                if "user_input" not in st.session_state.ingested_docs:
                    st.session_state.ingested_docs.append(text_title)
            except Exception as e:
                st.error(f"❌ Error: {str(e)}")


# ─── Tab 3: Performance Dashboard (WOW Feature #2) ──────────────────────

with tab_dashboard:
    st.markdown("### 📊 Live Performance Dashboard")

    _, _, benchmark_runner = get_components()

    # Real-time metrics from chat
    if st.session_state.performance_log:
        st.markdown("#### 📈 Session Performance (from your queries)")

        perf_data = st.session_state.performance_log
        col1, col2, col3, col4 = st.columns(4)

        avg_retrieval = sum(p["retrieval_ms"] for p in perf_data) / len(perf_data)
        avg_generation = sum(p["generation_ms"] for p in perf_data) / len(perf_data)
        avg_total = sum(p["total_ms"] for p in perf_data) / len(perf_data)

        col1.metric("Avg Retrieval", f"{avg_retrieval:.0f}ms")
        col2.metric("Avg Generation", f"{avg_generation:.0f}ms")
        col3.metric("Avg Total", f"{avg_total:.0f}ms")
        col4.metric("Total Queries", len(perf_data))

        # Latency trend chart
        fig = go.Figure()
        fig.add_trace(go.Scatter(
            y=[p["retrieval_ms"] for p in perf_data],
            mode="lines+markers",
            name="Retrieval",
            line=dict(color="#667eea", width=2),
        ))
        fig.add_trace(go.Scatter(
            y=[p["generation_ms"] for p in perf_data],
            mode="lines+markers",
            name="Generation",
            line=dict(color="#764ba2", width=2),
        ))
        fig.add_trace(go.Scatter(
            y=[p["total_ms"] for p in perf_data],
            mode="lines+markers",
            name="Total",
            line=dict(color="#48bb78", width=2),
        ))
        fig.update_layout(
            title="Query Latency Over Time",
            xaxis_title="Query #",
            yaxis_title="Latency (ms)",
            template="plotly_dark",
            height=400,
        )
        st.plotly_chart(fig, use_container_width=True)

    # Benchmark runner
    st.markdown("#### 🏃 Run Benchmarks")

    col1, col2 = st.columns(2)
    with col1:
        if st.button("🔄 Run Latency Benchmark", type="primary"):
            with st.spinner("Running latency benchmark..."):
                try:
                    results = benchmark_runner.run_latency_benchmark(runs_per_query=2)
                    st.session_state.benchmark_results["latency"] = results.get("summary", {})

                    if results.get("summary"):
                        # Create comparison chart
                        modes = list(results["summary"].keys())
                        avgs = [results["summary"][m]["avg_latency_ms"] for m in modes]

                        fig = go.Figure(data=[
                            go.Bar(
                                x=modes,
                                y=avgs,
                                marker_color=["#38b2ac", "#ed8936", "#667eea"],
                                text=[f"{v:.1f}ms" for v in avgs],
                                textposition="auto",
                            )
                        ])
                        fig.update_layout(
                            title="Search Latency Comparison",
                            xaxis_title="Search Mode",
                            yaxis_title="Average Latency (ms)",
                            template="plotly_dark",
                            height=400,
                        )
                        st.plotly_chart(fig, use_container_width=True)

                        # Display table
                        st.json(results["summary"])

                except Exception as e:
                    st.error(f"Benchmark failed: {str(e)}")

    with col2:
        if st.button("📊 Run Accuracy Benchmark"):
            with st.spinner("Running accuracy benchmark..."):
                try:
                    results = benchmark_runner._run_relevance_benchmark()
                    st.session_state.benchmark_results["accuracy"] = results.get("summary", {})

                    if results.get("summary"):
                        modes = list(results["summary"].keys())
                        sims = [results["summary"][m]["avg_similarity"] for m in modes]

                        fig = go.Figure(data=[
                            go.Bar(
                                x=modes,
                                y=sims,
                                marker_color=["#38b2ac", "#ed8936", "#667eea"],
                                text=[f"{v:.4f}" for v in sims],
                                textposition="auto",
                            )
                        ])
                        fig.update_layout(
                            title="Average Retrieval Similarity",
                            xaxis_title="Search Mode",
                            yaxis_title="Avg Similarity Score",
                            template="plotly_dark",
                            height=400,
                        )
                        st.plotly_chart(fig, use_container_width=True)

                        st.json(results["summary"])

                except Exception as e:
                    st.error(f"Benchmark failed: {str(e)}")

    # Saved benchmark results
    if st.session_state.benchmark_results:
        st.markdown("#### 📋 Benchmark Results Summary")
        for btype, bdata in st.session_state.benchmark_results.items():
            with st.expander(f"📊 {btype.title()} Results"):
                st.json(bdata)


# ─── Tab 4: About ───────────────────────────────────────────────────────

with tab_about:
    st.markdown("""
    ### 🧠 EndeeRAG — Production-Grade RAG System

    **Built for the Endee AI/ML Internship Hackathon**

    ---

    #### 🏗️ Architecture

    ```
    PDF Upload → Parse → Chunk (512 tokens + overlap)
         ↓
    Embedding (Dense: all-MiniLM-L6-v2 + Sparse: BM25)
         ↓
    Endee Vector Database (Hybrid Index)
         ↓
    Hybrid Retrieval (Dense + Sparse + RRF Fusion)
         ↓
    LLM Generation (Google Gemini) with Citations
         ↓
    Streamlit UI with Live Dashboard
    ```

    ---

    #### ⭐ Features

    | Feature | Description |
    |---------|-------------|
    | **Hybrid Search** | Dense (semantic) + Sparse (BM25) + RRF fusion |
    | **Metadata Filtering** | Filter by document name, use `$eq`, `$in` operators |
    | **Client-Side Encryption** | 🔐 AES encryption before storing in Endee |
    | **Live Dashboard** | 📊 Real-time latency tracking with Plotly charts |
    | **Conversation Memory** | 💬 Multi-turn context-aware conversations |
    | **Multi-Document Search** | 📚 Search across or within specific documents |
    | **Citation Support** | 📖 Every answer includes source references |
    | **Benchmarking** | 🏃 Compare dense/sparse/hybrid latency & accuracy |

    ---

    #### 🔧 Tech Stack

    - **Vector DB**: Endee (hybrid index with `endee_bm25`)
    - **Embeddings**: `all-MiniLM-L6-v2` (dense) + `endee/bm25` (sparse)
    - **LLM**: Google Gemini 2.0 Flash
    - **UI**: Streamlit + Plotly
    - **Encryption**: Fernet (AES-128-CBC)
    - **PDF Parsing**: PyMuPDF

    ---

    #### 🔑 Why Endee?

    1. **Native Hybrid Search**: Built-in RRF fusion of dense + sparse vectors
    2. **Server-Side BM25**: `endee_bm25` sparse model with IDF weighting
    3. **Advanced Filtering**: `$eq`, `$in`, `$range` operators on metadata
    4. **High Performance**: HNSW algorithm, INT8 quantization, sub-100ms queries
    5. **Easy SDK**: Clean Python API for index management and vector operations
    """)

    st.markdown("---")
    st.markdown("*Built with ❤️ using Endee Vector Database*")
