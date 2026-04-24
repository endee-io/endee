import streamlit as st
import numpy as np
import pickle
from sentence_transformers import SentenceTransformer

st.set_page_config(page_title="Semantic Code Search", layout="wide")

st.title("🔍 Offline Semantic Code Search Engine")
st.write("100% local AI-powered code search")

# Load model
model = SentenceTransformer("BAAI/bge-small-en-v1.5")

# Load index
with open("index.pkl", "rb") as f:
    vectors, metadata = pickle.load(f)

vectors = np.array(vectors)

def search(query, top_k=5):
    query_vec = model.encode(query, normalize_embeddings=True)

    scores = np.dot(vectors, query_vec)

    top_indices = np.argsort(scores)[::-1][:top_k]

    results = []
    for idx in top_indices:
        item = metadata[idx]
        item["score"] = float(scores[idx])
        results.append(item)

    return results

query = st.text_input("Enter your search query")

if st.button("Search") and query.strip():

    results = search(query)

    st.subheader("Top Matches")

    for i, r in enumerate(results, 1):

        st.markdown(f"### 🔹 Rank {i}")
        st.write(f"📄 File: `{r.get('file')}`")
        st.write(f"⭐ Score: `{round(r.get('score', 4), 4)}`")

        # SAFE CODE DISPLAY (NO ERROR EVER)
        code = r.get("code", "")

        if code:
            st.code(code, language="python")
        else:
            st.warning("No code found in index")