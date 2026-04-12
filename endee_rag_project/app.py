import streamlit as st
from sentence_transformers import SentenceTransformer
import endee
import os
from openai import OpenAI

# Page config
st.set_page_config(page_title="Endee AI Assistant", page_icon="🤖", layout="wide")

# Load CSS
def local_css(file_name):
    with open(file_name) as f:
        st.markdown(f'<style>{f.read()}</style>', unsafe_allow_html=True)

css_path = os.path.join(os.path.dirname(__file__), "app.css")
if os.path.exists(css_path):
    local_css(css_path)

# Init models and clients once
@st.cache_resource
def load_embedder():
    return SentenceTransformer('all-MiniLM-L6-v2')

@st.cache_resource
def get_endee_index():
    try:
        client = endee.Endee()
        return client.get_index("endee_docs")
    except Exception as e:
        return None

# Load Resources
embedder = load_embedder()
index = get_endee_index()

# App Header
st.title("🤖 Endee Docs Assistant")
st.markdown("---")

# Initialize chat history
if "messages" not in st.session_state:
    st.session_state.messages = []

# Sidebar config
with st.sidebar:
    st.header("⚙️ Settings")
    api_key = st.text_input("OpenAI API Key (Optional)", type="password", help="If provided, enables RAG answers. Otherwise, provides semantic context.")
    
    st.markdown("### Search Parameters")
    top_k = st.slider("Max Context Chunks", 1, 10, 3)
    
    if st.button("Clear History"):
        st.session_state.messages = []
        st.rerun()

    st.markdown("---")
    st.markdown("### Search Mode")
    if api_key:
        st.success("🤖 RAG Mode: Active")
    else:
        st.info("🔎 Semantic Mode: Active")

# Display chat messages from history on app rerun
for message in st.session_state.messages:
    with st.chat_message(message["role"]):
        st.markdown(message["content"])
        if "context" in message:
            with st.expander("📄 Source Context"):
                st.markdown(message["context"])

# React to user input
if prompt := st.chat_input("Ask me anything about Endee..."):
    # Display user message in chat message container
    st.chat_message("user").markdown(prompt)
    # Add user message to chat history
    st.session_state.messages.append({"role": "user", "content": prompt})

    if not index:
        with st.chat_message("assistant"):
            st.error("Endee database index 'endee_docs' not found. Please run ingestion first.")
    else:
        with st.chat_message("assistant"):
            with st.spinner("Analyzing documentation..."):
                # 1. Embed user query
                query_vector = embedder.encode(prompt).tolist()
                
                # 2. Query Endee
                try:
                    results = index.query(vector=query_vector, top_k=top_k)
                except Exception as e:
                    st.error(f"Search failed: {e}")
                    results = []

                if not results:
                    response = "I couldn't find any relevant documentation in Endee for your question."
                    st.markdown(response)
                    st.session_state.messages.append({"role": "assistant", "content": response})
                else:
                    # 3. Format Context
                    context_chunks = []
                    for i, res in enumerate(results):
                        meta = res.get("meta", {})
                        text = meta.get("text", "No text provided")
                        source = meta.get("source", "Unknown source")
                        score = res.get("similarity", 0)
                        context_chunks.append(f"**[{i+1}] Source: {source} (Sim: {score:.4f})**\n{text}")
                    
                    context_string = "\n\n---\n\n".join(context_chunks)

                    # 4. Generate Response
                    if api_key:
                        try:
                            client = OpenAI(api_key=api_key)
                            prompt_rag = f"Assistant for Endee Vector DB.\n\nContext:\n{context_string}\n\nQuestion: {prompt}\n\nAnswer concisely based ONLY on the context blocks above."
                            
                            stream = client.chat.completions.create(
                                model="gpt-3.5-turbo",
                                messages=[{"role": "user", "content": prompt_rag}],
                                stream=True,
                            )
                            response = st.write_stream(stream)
                        except Exception as e:
                            st.error(f"OpenAI Error: {e}")
                            response = "I encountered an error with the AI provider. Here is the raw context found:"
                            st.markdown(response)
                    else:
                        response = "I found the following related information in the documentation:"
                        st.markdown(response)

                    # Show sources in expander
                    with st.expander("📄 Viewed {len(results)} relevant documentation sections"):
                        st.info(context_string)
                    
                    st.session_state.messages.append({
                        "role": "assistant", 
                        "content": response,
                        "context": context_string
                    })

