import streamlit as st

st.title("AI Document Assistant using Endee")
st.write("Semantic Search powered by Endee Vector Database")

documents = [
    "Machine learning is a field of artificial intelligence.",
    "Vector databases are used for semantic search.",
    "Endee is a fast vector database for AI applications.",
    "Retrieval Augmented Generation improves LLM responses.",
    "Artificial intelligence is transforming industries."
]

query = st.text_input("Ask something about AI")

if query:
    results = []

    for doc in documents:
        if query.lower() in doc.lower():
            results.append(doc)

    if results:
        st.subheader("Relevant Results")
        for r in results:
            st.write(r)
    else:
        st.write("No relevant documents found")