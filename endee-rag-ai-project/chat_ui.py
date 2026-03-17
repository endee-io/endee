import streamlit as st
import requests
from pdf_loader import load_pdf

st.title("📄 AI Document Assistant")

uploaded_file = st.file_uploader("Upload a PDF", type="pdf")

document_text = ""

if uploaded_file:
    document_text = load_pdf(uploaded_file)
    st.success("PDF loaded successfully!")

question = st.text_input("Ask a question about the document")

if st.button("Ask AI"):

    if question == "":
        st.warning("Please enter a question")

    else:
        payload = {
            "question": question,
            "context": document_text
        }

        response = requests.post(
            "http://127.0.0.1:8000/ask",
            json=payload
        )

        if response.status_code == 200:
            st.success(response.json()["answer"])
        else:
            st.error("Error from API")