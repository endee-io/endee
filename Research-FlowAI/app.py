import os
os.environ["KMP_DUPLICATE_LIB_OK"] = "TRUE"
from dotenv import load_dotenv
load_dotenv()
import streamlit as st
from pypdf import PdfReader
from sentence_transformers import SentenceTransformer
from groq import Groq
import endee

st.set_page_config(page_title="ResearchFlow AI", page_icon="🧠", layout="wide")

# Read API Key
api_key = os.getenv("GROQ_API_KEY", "")

# Initialize Groq client
if "groq_client" not in st.session_state and api_key:
    st.session_state.groq_client = Groq(api_key=api_key)

# Initialize Endee Client
if "endee_client" not in st.session_state:
    try:
        # Connect to Endee
        client = endee.Endee()
        client.set_base_url("http://localhost:8081/api/v1")
        
        # Ensure 384-dim index exists for all-MiniLM-L6-v2
        try:
            client.create_index(
                name="research_flow",
                dimension=384,
                space_type="cosine"
            )
        except Exception:
            # Assume exception might be because index already exists
            pass
            
        st.session_state.endee_index = client.get_index("research_flow")
        st.session_state.endee_client = client
    except Exception as e:
        st.sidebar.error(f"Endee Connection Error: {e}")

@st.cache_resource
def load_embedder():
    return SentenceTransformer("all-MiniLM-L6-v2")

embedder = load_embedder()

st.title("ResearchFlow AI 🧠")
st.markdown("A highly-performant tool for technical document retrieval and synthesis leveraging **Endee Vector DB**.")

if not api_key:
    st.warning("Groq API key not found. Please set the `GROQ_API_KEY` environment variable.")

# Sidebar - Indexing
with st.sidebar:
    st.header("1. Upload Context")
    st.markdown("Upload a PDF or paste text to add to your Endee Vector DB.")
    
    uploaded_file = st.file_uploader("Upload a PDF Document", type=["pdf"])
    st.markdown("---")
    st.markdown("**OR** Paste text directly:")
    raw_text = st.text_area("Paste document text here:", height=150)
    
    if st.button("Index Document", type="primary"):
        doc_text = ""
        
        if uploaded_file is not None:
            try:
                reader = PdfReader(uploaded_file)
                for page in reader.pages:
                    page_text = page.extract_text()
                    if page_text:
                        doc_text += page_text + "\n\n"
            except Exception as e:
                st.error(f"Failed to read PDF: {e}")
        elif raw_text.strip():
            doc_text = raw_text.strip()
            
        if not doc_text.strip():
            st.error("Please upload a PDF or provide valid text.")
        elif "endee_client" not in st.session_state:
            st.error("Endee Vector DB is not connected.")
        else:
            with st.spinner("Chunking and Upserting to Endee..."):
                # Basic chunking: split by double newlines or single newlines
                chunks = [chunk.strip() for chunk in doc_text.split("\n\n") if chunk.strip()]
                if not chunks:
                    chunks = [chunk.strip() for chunk in doc_text.split("\n") if chunk.strip()]
                
                # Generate embeddings (384-dim)
                embeddings = embedder.encode(chunks)
                
                # Prepare payload for upsert
                input_array = []
                for i, chunk in enumerate(chunks):
                    input_array.append({
                        "id": f"doc_{hash(chunk)}_{i}",
                        "vector": embeddings[i].tolist(),
                        "meta": {"text": chunk}
                    })
                
                try:
                    st.session_state.endee_index.upsert(input_array)
                    st.success(f"Indexed {len(chunks)} chunks successfully into Endee!")
                except Exception as e:
                    st.error(f"Upsert failed: {e}")

# Main window - RAG Chat
st.header("2. Ask Questions")
if "messages" not in st.session_state:
    st.session_state.messages = []

# Display chat history
for message in st.session_state.messages:
    with st.chat_message(message["role"]):
        st.markdown(message["content"])

if prompt := st.chat_input("E.g., What are the key features of this document?"):
    st.session_state.messages.append({"role": "user", "content": prompt})
    with st.chat_message("user"):
        st.markdown(prompt)

    with st.chat_message("assistant"):
        if "endee_client" not in st.session_state:
            st.error("Endee Vector DB is not connected.")
            st.stop()
            
        if "groq_client" not in st.session_state:
            st.error("Groq client not initialized. Check API Key.")
            st.stop()

        message_placeholder = st.empty()
        
        # 1. Embed user query
        query_vector = embedder.encode([prompt])[0].tolist()
        
        # 2. Retrieve top-2 from Endee
        context_texts = []
        try:
            # Search via index.query
            results = st.session_state.endee_index.query(
                vector=query_vector,
                top_k=2
            )
            
            # Extract text from Endee client search results
            if isinstance(results, list):
                for res in results:
                    if isinstance(res, dict) and "meta" in res and "text" in res["meta"]:
                        context_texts.append(res["meta"]["text"])
        except Exception as e:
            st.warning(f"Error retrieving from Endee: {e}")

        # 3. Format Prompt
        context_block = "\n\n---\n\n".join(context_texts) if context_texts else "No context retrieved from Endee."
        
        system_prompt = f"""You are ResearchFlow AI, an intelligent technical assistant.
Answer the user's question precisely using ONLY the following context retrieved from our Endee Vector database.
If the context does not contain the answer, politely state that you do not have enough information.

<context>
{context_block}
</context>
"""

        # 4. Generate Answer with Groq LLM
        try:
            stream = st.session_state.groq_client.chat.completions.create(
                model="llama-3.3-70b-versatile",
                messages=[
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": prompt}
                ],
                stream=True,
            )
            
            full_response = ""
            for chunk in stream:
                if chunk.choices[0].delta.content is not None:
                    full_response += chunk.choices[0].delta.content
                    message_placeholder.markdown(full_response + "▌")
            
            if context_texts:
                full_response += "\n\n**Sources retrieved via Endee Similarity Search:**\n"
                for i, text in enumerate(context_texts):
                    snippet = text[:100].replace("\n", " ") + "..." if len(text) > 100 else text.replace("\n", " ")
                    full_response += f"- *[{i+1}] {snippet}*\n"
                    
            message_placeholder.markdown(full_response)
            st.session_state.messages.append({"role": "assistant", "content": full_response})
            
        except Exception as e:
            message_placeholder.error(f"LLM Generation Error: {e}")
