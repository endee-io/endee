import streamlit as st
from PyPDF2 import PdfReader
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.metrics.pairwise import cosine_similarity
import re

st.set_page_config(page_title="AI-Powered RAG System", layout="centered")
st.title("🎯 AI-Powered RAG(VECTOR SEARCH) System ")

def get_text_data(pdf_file):
    reader = PdfReader(pdf_file)
    sentences = []
    for page in reader.pages:
        text = page.extract_text()
        # Resume lines-ah bullet points and line breaks vachi split pandrom
        lines = re.split(r'\n|•|', text)
        for line in lines:
            clean_line = line.strip()
            if len(clean_line) > 5:
                sentences.append(clean_line)
    return sentences

uploaded_file = st.file_uploader("Upload Resume PDF", type="pdf")

if uploaded_file:
    sentences = get_text_data(uploaded_file)
    st.success(f"Indexed {len(sentences)} logic points!")

    user_query = st.chat_input("Ask: What is my first achievement?")

    if user_query:
        # Step 1: Query-ah clean panni context edukirom
        # Namma stop_words remove pannama, exact match-ku priority tharuvom
        vectorizer = TfidfVectorizer(ngram_range=(1, 3), analyzer='char_wb') 
        tfidf_matrix = vectorizer.fit_transform(sentences + [user_query])
        
        # Step 2: Vector similarity calculation
        cosine_sim = cosine_similarity(tfidf_matrix[-1], tfidf_matrix[:-1]).flatten()
        
        # Step 3: Top match index edukirom
        best_match_idx = cosine_sim.argsort()[-1]
        score = cosine_sim[best_match_idx]

        st.subheader("✅ Proper Answer:")
        
        # Score threshold-ah korachurukom so result miss aagathu
        if score > 0.05:
            # Result highlight
            st.info(f"**Found:** {sentences[best_match_idx]}")
            
            # Additional Context: Resume-la andha line-ku mela keela irukra 1 line-um kaatuvom
            # Idhu dhaan 'First Achievement' nu kekum pothu correct-ana paragraph-ah thookum
            with st.expander("Show related lines from PDF"):
                start = max(0, best_match_idx - 1)
                end = min(len(sentences), best_match_idx + 2)
                for i in range(start, end):
                    st.write(f"- {sentences[i]}")
        else:
            st.warning("Sorry boss, Try simple keywords .")
            #py -m streamlit run app.py