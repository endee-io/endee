import streamlit as st
import PyPDF2
from sentence_transformers import SentenceTransformer
from endee import Endee, Precision
import ollama

INDEX_NAME = "documents"
DIMENSION = 384

@st.cache_resource
def load_model():
    return SentenceTransformer("all-MiniLM-L6-v2")

@st.cache_resource
def get_index():
    client = Endee()
    client.set_base_url("http://localhost:8080/api/v1")
    existing = [idx["name"] for idx in client.list_indexes()["indexes"]]
    if INDEX_NAME not in existing:
        client.create_index(
            name=INDEX_NAME,
            dimension=DIMENSION,
            space_type="cosine",
            precision=Precision.INT8
        )
    return client.get_index(name=INDEX_NAME)

def chunk_text(text, chunk_size=200):
    words = text.split()
    return [" ".join(words[i:i+chunk_size]) for i in range(0, len(words), chunk_size)]

def extract_text_from_pdf(pdf_file):
    reader = PyPDF2.PdfReader(pdf_file)
    text = ""
    for page in reader.pages:
        text += page.extract_text() + "\n"
    return text

def ask_ollama(context, question):
    response = ollama.chat(model="llama3.2", messages=[
        {"role": "system", "content": "Answer the question using only the context provided. Be concise."},
        {"role": "user", "content": f"Context:\n{context}\n\nQuestion: {question}"}
    ])
    return response["message"]["content"]

# --- UI ---
st.set_page_config(page_title="DocuSearch", page_icon="📄")
st.title("📄 DocuSearch")
st.caption("Upload a PDF and ask questions — powered by Endee Vector DB + Ollama AI")

model = load_model()
index = get_index()

uploaded_file = st.file_uploader("Upload a PDF file", type="pdf")

if uploaded_file:
    with st.spinner("Reading and indexing your document..."):
        text = extract_text_from_pdf(uploaded_file)
        chunks = chunk_text(text)
        embeddings = model.encode(chunks)
        vectors = [
            {
                "id": f"chunk_{i}",
                "vector": emb.tolist(),
                "meta": {"text": chunk}
            }
            for i, (chunk, emb) in enumerate(zip(chunks, embeddings))
        ]
        for i in range(0, len(vectors), 100):
            index.upsert(vectors[i:i+100])
    st.success(f"✅ Document indexed! ({len(chunks)} chunks stored in Endee)")

st.divider()
question = st.text_input("💬 Ask a question about your document")

if question:
    with st.spinner("Searching Endee and generating answer..."):
        q_vec = model.encode([question])[0].tolist()
        results = index.query(vector=q_vec, top_k=5)

        if results:
            context = "\n\n".join([r["meta"]["text"] for r in results])
            answer = ask_ollama(context, question)

            st.subheader("🤖 Answer")
            st.write(answer)

            with st.expander("📚 Source chunks retrieved from Endee"):
                for i, r in enumerate(results):
                    st.markdown(f"**Chunk {i+1}** (similarity: {r['similarity']:.3f})")
                    st.write(r["meta"]["text"])
        else:
            st.warning("No results found. Please upload a document first!")
