import os

from dotenv import load_dotenv
import requests
import streamlit as st

load_dotenv()

st.set_page_config(
    page_title="Document RAG Chat",
    page_icon="🤖",
    layout="wide",
)

if "messages" not in st.session_state:
    st.session_state.messages = [
        {
            "role": "assistant",
            "content": "Welcome! Upload a document and ask a question to get started.",
        }
    ]

if "sources" not in st.session_state:
    st.session_state.sources = []

if "query_text" not in st.session_state:
    st.session_state.query_text = ""

if "clear_query" not in st.session_state:
    st.session_state.clear_query = False

if st.session_state.clear_query:
    st.session_state.query_text = ""
    st.session_state.clear_query = False

backend_url = os.getenv("BACKEND_URL", "http://127.0.0.1:8000")

st.markdown(
    """
    <style>
    :root {
        color-scheme: dark;
        font-family: Inter, system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    }

    .main .block-container {
        padding: 1.5rem 2rem 7rem;
        background: #090b14;
        color: #e2e8f0;
    }

    .sidebar .css-1d391kg {
        background: #0f172a !important;
        border-radius: 20px;
        padding: 1rem 1rem 0.8rem 1rem;
        border: 1px solid rgba(255,255,255,0.06);
    }

    .sidebar-card {
        background: rgba(255, 255, 255, 0.03);
        border: 1px solid rgba(255,255,255,0.08);
        border-radius: 20px;
        padding: 1rem;
        margin-bottom: 1rem;
    }

    .chat-panel {
        background: rgba(255,255,255,0.03);
        border: 1px solid rgba(255,255,255,0.08);
        border-radius: 24px;
        padding: 1.4rem;
        min-height: 60vh;
    }

    .message-row {
        display: flex;
        width: 100%;
        margin-bottom: 0.9rem;
    }

    .message-bubble {
        max-width: 78%;
        border-radius: 18px;
        padding: 1rem 1.2rem;
        line-height: 1.65;
        word-wrap: break-word;
        box-shadow: 0 12px 30px rgba(0, 0, 0, 0.18);
    }

    .user-bubble {
        margin-left: auto;
        background: linear-gradient(135deg, #3b82f6 0%, #06b6d4 100%);
        color: #ffffff;
        border-bottom-right-radius: 4px;
    }

    .assistant-bubble {
        margin-right: auto;
        background: rgba(15, 23, 42, 0.96);
        color: #e2e8f0;
        border-bottom-left-radius: 4px;
    }

    .message-meta {
        margin-bottom: 0.5rem;
        font-size: 0.9rem;
        color: #94a3b8;
    }

    .fixed-footer {
        position: fixed;
        left: 50%;
        bottom: 0;
        transform: translateX(-50%);
        width: calc(100% - 3rem);
        max-width: 1180px;
        background: rgba(15, 23, 42, 0.98);
        border-top: 1px solid rgba(255,255,255,0.08);
        padding: 1rem 1rem 1.1rem;
        z-index: 100;
        box-shadow: 0 -12px 30px rgba(0, 0, 0, 0.22);
        border-bottom-left-radius: 16px;
        border-bottom-right-radius: 16px;
    }

    .fixed-footer textarea {
        border-radius: 16px !important;
        border: 1px solid rgba(255,255,255,0.1) !important;
        background: #111827 !important;
        color: #e2e8f0 !important;
        min-height: 100px;
    }

    .stButton>button {
        border-radius: 14px;
        background: linear-gradient(135deg, #22c55e 0%, #14b8a6 100%);
        color: #fff;
        font-weight: 700;
        border: none;
        padding: 0.9rem 1.1rem;
        transition: transform 0.18s ease, box-shadow 0.18s ease;
    }

    .stButton>button:hover {
        transform: translateY(-1px);
        box-shadow: 0 20px 40px rgba(34, 197, 94, 0.18);
    }

    .stButton>button:focus {
        outline: none;
    }

    .stAlert {
        border-radius: 16px;
    }
    </style>
    """,
    unsafe_allow_html=True,
)

with st.sidebar:
    st.markdown("<div class='sidebar-card'><h2>Document Q&A</h2><p>Upload a PDF or TXT file, then ask questions about the indexed content.</p></div>", unsafe_allow_html=True)

    file_uploader = st.file_uploader("Upload PDF or TXT file", type=["pdf", "txt"], key="file_uploader")
    if st.button("Upload Document"):
        if file_uploader is None:
            st.warning("Please choose a file first.")
        else:
            with st.spinner("Indexing document..."):
                try:
                    files = {"file": (file_uploader.name, file_uploader.getvalue())}
                    response = requests.post(
                        f"{backend_url.rstrip('/')}/upload",
                        files=files,
                        timeout=120,
                    )
                    response.raise_for_status()
                    data = response.json()
                    st.success(data.get("message", "Document uploaded successfully."))
                except requests.exceptions.RequestException as exc:
                    st.error(f"Upload failed: {exc}")
                except Exception as exc:
                    st.error(f"Unexpected error: {exc}")

    st.markdown(f"<div class='sidebar-card'><h3>Backend</h3><p>{backend_url}</p></div>", unsafe_allow_html=True)
    st.markdown("<div class='sidebar-card'><h3>Tips</h3><ul style='padding-left: 18px; color:#cbd5e1;'><li>Upload a file first.</li><li>Ask a focused question.</li><li>Review the source snippets when available.</li></ul></div>", unsafe_allow_html=True)

st.markdown("<div class='chat-panel'>", unsafe_allow_html=True)
st.markdown("<div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:1.2rem;'><div><h1 style='margin:0; color:#f8fafc;'>RAG Chat</h1><p style='margin:0.35rem 0 0 0; color:#94a3b8;'>Ask questions about the uploaded document and get concise answers.</p></div></div>", unsafe_allow_html=True)

for message in st.session_state.messages:
    if message["role"] == "user":
        st.markdown(
            f"<div class='message-row'><div class='message-bubble user-bubble'><div class='message-meta'>You</div>{message['content']}</div></div>",
            unsafe_allow_html=True,
        )
    else:
        st.markdown(
            f"<div class='message-row'><div class='message-bubble assistant-bubble'><div class='message-meta'>Assistant</div>{message['content']}</div></div>",
            unsafe_allow_html=True,
        )

if st.session_state.sources:
    with st.expander("Source snippets", expanded=False):
        for source in st.session_state.sources:
            st.markdown(f"**{source.get('filename')}[{source.get('chunk_id')}]** — score {source.get('score', 0):.4f}")
            st.markdown(f"<div style='padding:0.9rem; background:#111827; border-radius:12px; color:#cbd5e1; margin-bottom:0.8rem;'>{source.get('snippet', '')}</div>", unsafe_allow_html=True)

st.markdown("</div>", unsafe_allow_html=True)

st.markdown("<div class='fixed-footer'>", unsafe_allow_html=True)
question = st.text_area("Ask a question", placeholder="Type your question here...", key="query_text", label_visibility="collapsed")
ask_button = st.button("Ask")
st.markdown("</div>", unsafe_allow_html=True)

if ask_button:
    if not st.session_state.query_text.strip():
        st.warning("Please enter a question before asking.")
    else:
        user_question = st.session_state.query_text.strip()
        st.session_state.messages.append({"role": "user", "content": user_question})
        st.session_state.clear_query = True
        with st.spinner("Thinking..."):
            try:
                payload = {"question": user_question, "top_k": 5}
                response = requests.post(
                    f"{backend_url.rstrip('/')}/query",
                    json=payload,
                    timeout=120,
                )
                response.raise_for_status()
                data = response.json()
                st.session_state.messages.append({"role": "assistant", "content": data.get("answer", "No answer returned.")})
                st.session_state.sources = data.get("sources", [])
                st.rerun()
            except requests.exceptions.RequestException as exc:
                st.error(f"Query failed: {exc}")
            except Exception as exc:
                st.error(f"Unexpected error: {exc}")
